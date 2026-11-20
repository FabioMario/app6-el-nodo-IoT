/*
 * nodo_mqtt — Clase 6 (Sistemas Embebidos, UCU 2026)
 *
 * Ejemplo 2 y base de la App 6. La Kaluga-1 como NODO IoT: mide su
 * temperatura, la publica por MQTT en un broker público y obedece las
 * órdenes que le llegan del panel web, que es otro terminal del mismo broker.
 *
 *   [sensor p6] ──cola de muestras──▶ [publicador p4] ──publish──▶ broker
 *    cada N s;      120 huecos =        espera WiFi Y MQTT          │  ▲
 *    nunca espera   10 min sin red      (grupo de eventos)          │  │
 *                                                                   │  │
 *   [mqtt_task p5] ◀── órdenes del panel ───────────────────────────┘  │
 *    manejador: copia y encola                                         │
 *       └── cola de órdenes ──▶ [comandos p3] ── eco, estado nuevo ────┘
 *
 *   [conexion p3]  (red_wifi.c) reconexión WiFi con espera creciente
 *   [interfaz p2]  LED: estado de la conexión
 *   [timer]        informe de tareas y contadores cada 30 s
 *
 * Los temas (ID = "kaluga-" y los tres últimos bytes de la MAC):
 *
 *   ucu-se2026/<ID>/temperatura   placa → panel   QoS 0, retenido
 *   ucu-se2026/<ID>/estado        placa → panel   QoS 1, retenido; última
 *                                                 voluntad "desconectado"
 *   ucu-se2026/<ID>/comando       panel → placa   QoS 1
 *   ucu-se2026/<ID>/eco           placa → panel   QoS 0 (respuesta al ping)
 *
 * Hardware: solo la placa, con el JUMPER RGB colocado (LED en GPIO45). El
 * sensor es el interno del ESP32-S2.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/temperature_sensor.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "led_strip.h"
#include "mqtt_client.h"
#include "red_wifi.h"
#include "microfono.h"

/* ======================================================================== */
/*  CONFIGURACIÓN: editar antes de compilar                                  */
/* ======================================================================== */

/* --- La red WiFi --------------------------------------------------------- */
/* El nombre de la red (SSID), tal como lo enseña el teléfono. */
#define WIFI_SSID       ""

/* La clave de la red: la misma para todos los que se conectan a ella. */
#define WIFI_CLAVE      ""

/* Seguridad de la red: descomenta UNA sola línea. Es el MÍNIMO que se acepta:
 * si la red ofrece algo mejor, la placa lo negocia sola. El ejemplo 1
 * (wifi_estacion) dice qué seguridad usa tu red. */
//#define WIFI_SEGURIDAD_WPA        /* WPA  (TKIP): routers muy antiguos   */
#define WIFI_SEGURIDAD_WPA2         /* WPA2 (AES):  la gran mayoría        */
//#define WIFI_SEGURIDAD_WPA3       /* WPA3 (SAE):  redes WPA3 o WPA2/WPA3 */

/* --- El broker MQTT: descomenta UNO. Son públicos y gratuitos; el panel web
 *     tiene que apuntar al MISMO broker. --------------------------------- */
#define MQTT_BROKER     "mqtt://broker.hivemq.com:1883"
//#define MQTT_BROKER   "mqtt://broker.emqx.io:1883"
//#define MQTT_BROKER   "mqtts://broker.hivemq.com:8883"  /* con TLS: Clase 7 */

/* La raíz de los temas. Todas las placas de la clase cuelgan de aquí, y el
 * panel se suscribe a ucu-se2026/+/temperatura para verlas a todas. */
#define MQTT_RAIZ       "ucu-se2026"

/* ======================================================================== */

#if (defined(WIFI_SEGURIDAD_WPA) + defined(WIFI_SEGURIDAD_WPA2) + \
     defined(WIFI_SEGURIDAD_WPA3)) != 1
#error "Descomenta exactamente una: WIFI_SEGURIDAD_WPA, _WPA2 o _WPA3"
#endif

#if defined(WIFI_SEGURIDAD_WPA)
#define WIFI_SEGURIDAD  RED_WPA
#elif defined(WIFI_SEGURIDAD_WPA2)
#define WIFI_SEGURIDAD  RED_WPA2
#else
#define WIFI_SEGURIDAD  RED_WPA3
#endif

#define VERSION_NODO        "clase6-1.0"

#define RGB_LED_GPIO        45
#define RGB_LED_COUNT       1

/* --- Prioridades ---------------------------------------------------------
 *
 * Todas por debajo de la pila de red de ESP-IDF (wifi 23, sys_evt 20,
 * tcpip 18): ninguna tarea que use la red debe quitarle la CPU a tcpip,
 * que es quien le tiene que contestar.
 *
 *   6  sensor      periódica y cortísima: mide y encola
 *   5  mqtt_task   la del cliente MQTT (valor por defecto del componente)
 *   4  publicador  puede quedarse segundos dentro de publish(): no molesta,
 *                  porque nadie la espera
 *   3  comandos    responde a una persona, pero la red ya tarda cientos de ms
 *   3  conexion    (red_wifi.c) solo decide cuándo reintentar
 *   2  interfaz    el LED
 */
#define PRIO_SENSOR         6
#define PRIO_MQTT           5
#define PRIO_PUBLICADOR     4
#define PRIO_COMANDOS       3
#define PRIO_INTERFAZ       2

/* En bytes. Se ajustan con la columna "pila libre" del informe. */
#define PILA_SENSOR         3072
#define PILA_MQTT           6144     /* la del componente; con TLS, no bajarla */
#define PILA_PUBLICADOR     4096
#define PILA_COMANDOS       4096
#define PILA_INTERFAZ       3072

/* --- Tiempos y capacidades --------------------------------------------- */
#define PERIODO_INICIAL_S   5
#define PERIODO_MIN_S       1
#define PERIODO_MAX_S       600
#define COLA_MUESTRAS_LARGO 120      /* 120 × 5 s = 10 min de red caída      */
#define COLA_COMANDOS_LARGO 8
#define MQTT_KEEPALIVE_S    30       /* el broker da la placa por muerta a   */
                                     /* los 1,5 × 30 = 45 s de silencio      */
#define INFORME_PERIODO_MS  30000
#define IDENTIFICAR_MS      5000

/* Los bits del grupo de eventos. EV_WIFI_OK (BIT0) es de red_wifi.c. */
#define EV_MQTT_OK          BIT1

/* Los avisos que recibe la tarea interfaz, como bits de su notificación. */
#define AVISO_DESTELLO      (1u << 0)
#define AVISO_IDENTIFICAR   (1u << 1)

#define ESTADO_DESCONECTADO "{\"estado\":\"desconectado\"}"

static const char *TAG = "nodo_mqtt";

/* ======================================================================== */
/*  Tipos                                                                    */
/* ======================================================================== */

/* Lo que viaja por la cola de muestras, POR COPIA. El int64_t va primero:
 * así la estructura ocupa 24 bytes y no 32 por el relleno. */
typedef struct {
    int64_t  marca_us;           /* cuándo se midió (esp_timer)              */
    uint32_t secuencia;          /* el panel ve los huecos                   */
    int32_t  centigrados_x100;   /* 4137 = 41,37 °C: entero, el S2 no tiene FPU */
    int32_t  microfono_x100;
    int8_t   rssi;               /* dBm en el momento de medir; 0 sin red    */
} muestra_t;

/* Una orden recibida, copiada del búfer del cliente MQTT. */
typedef struct {
    char texto[48];
    bool retenida;               /* llegó con el indicador RETAIN puesto    */
} comando_t;

/* Estado compartido. Lo escriben varias tareas; se toca con el mutex. */
typedef struct {
    uint32_t periodo_s;          /* lo cambia "comandos", lo lee "sensor"    */
    uint32_t medidas;
    uint32_t publicadas;
    uint32_t perdidas;           /* tiradas con la cola llena                */
    uint32_t devueltas;          /* publish() fallido: vuelve a la cola       */
    uint32_t comandos_ok;
    uint32_t comandos_rechazados;
    uint32_t conexiones_mqtt;
    uint32_t ms_ip_a_mqtt;       /* de la primera IP al primer CONNECTED     */
    uint32_t publicar_max_us;    /* la publicación más lenta                 */
    int32_t  ultima_x100;
    bool microfono_habilitado;
} estado_t;

/* ======================================================================== */
/*  Objetos del kernel y estado                                              */
/* ======================================================================== */

static EventGroupHandle_t s_eventos;        /* EV_WIFI_OK | EV_MQTT_OK       */
static QueueHandle_t      s_cola_muestras;  /* sensor -> publicador          */
static QueueHandle_t      s_cola_comandos;  /* manejador MQTT -> comandos    */
static SemaphoreHandle_t  s_mutex_estado;
static TaskHandle_t       s_tarea_sensor;
static TaskHandle_t       s_tarea_interfaz;
static TimerHandle_t      s_timer_informe;

static estado_t s_estado = {
    .periodo_s = PERIODO_INICIAL_S,
    .microfono_habilitado = true
};

static esp_mqtt_client_handle_t    s_mqtt;
static temperature_sensor_handle_t s_tsens;
static led_strip_handle_t          s_led;

/* El cliente MQTT guarda PUNTEROS a estas cadenas (identificador, tema de
 * la última voluntad): tienen que vivir todo el programa. */
static char s_id[24];
static char s_tema_temperatura[64];
static char s_tema_microfono[64];
static char s_tema_estado[64];
static char s_tema_comando[64];
static char s_tema_eco[64];

/* ======================================================================== */
/*  Utilidades                                                               */
/* ======================================================================== */

/* Centésimas a texto con PUNTO decimal: es el formato del JSON, no el de la
 * consola. "-0.05" sale bien porque el signo se escribe aparte. */
static void x100_a_texto(char *dst, size_t n, int32_t x100)
{
    const char *signo = (x100 < 0) ? "-" : "";
    const int32_t a = (x100 < 0) ? -x100 : x100;
    snprintf(dst, n, "%s%" PRId32 ".%02" PRId32, signo, a / 100, a % 100);
}

/* {"t":41.37,"rssi":-58,"seq":120,"edad":0}
 * "edad" son los ms que la muestra pasó en la cola: 0 con la red bien, y
 * minutos para las que se publican al volver la red. Con ese número el panel
 * coloca cada punto en el instante en que se midió, no en el que llegó. */
static int formatear_muestra(char *dst, size_t n, const muestra_t *m,
                             uint32_t edad_ms)
{
    char t[16];
    x100_a_texto(t, sizeof(t), m->centigrados_x100);
    return snprintf(dst, n,
                    "{\"t\":%s,\"rssi\":%d,\"seq\":%" PRIu32 ",\"edad\":%" PRIu32 "}",
                    t, m->rssi, m->secuencia, edad_ms);
}

/* Entero decimal estricto: "5" sí; "5x", "", "-3" fuera de rango o "99999"
 * no. sscanf("%u") aceptaría las tres primeras. */
static bool leer_entero(const char *s, long min, long max, long *valor)
{
    char *fin = NULL;
    errno = 0;
    const long v = strtol(s, &fin, 10);
    if (fin == s || *fin != '\0' || errno != 0 || v < min || v > max) {
        return false;
    }
    *valor = v;
    return true;
}

/* El testigo de un ping: de 1 a 32 caracteres de [A-Za-z0-9_-]. */
static bool testigo_valido(const char *s)
{
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= 32) {
            return false;
        }
    }
    return n > 0;
}

/* Lo que llega del broker lo puede haber escrito cualquiera. Antes de
 * imprimirlo, fuera los caracteres de control. */
static void sanear(char *s)
{
    for (; *s != '\0'; s++) {
        if (*s < 32 || *s > 126) {
            *s = '?';
        }
    }
}

static void contar_rechazado(void)
{
    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    s_estado.comandos_rechazados++;
    xSemaphoreGive(s_mutex_estado);
}

/* ======================================================================== */
/*  Publicaciones de estado                                                  */
/* ======================================================================== */

/* {"estado":"en_linea","ip":"192.168.1.37","periodo":5,"version":"clase6-1.0"}
 * Retenido: quien se suscriba más tarde lo recibe en el acto. Se publica al
 * conectar y cada vez que una orden cambia algo: es la CONFIRMACIÓN de que
 * la placa aplicó la orden. */

static void publicar_estado(void)
{
    red_estado_t w;
    red_wifi_estado(&w);

    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    const uint32_t periodo = s_estado.periodo_s;
    const bool microfono = s_estado.microfono_habilitado;
    xSemaphoreGive(s_mutex_estado);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"estado\":\"en_linea\",\"ip\":\"" IPSTR "\",\"periodo\":%" PRIu32
             ",\"microfono\":%d,\"version\":\"%s\"}",
             IP2STR(&w.ip), periodo, microfono ? 1 : 0, VERSION_NODO);
    esp_mqtt_client_publish(s_mqtt, s_tema_estado, json, 0, 1, 1);
}


/* ======================================================================== */
/*  Manejador de eventos MQTT:  corre en la tarea mqtt_task (prioridad 5)    */
/* ======================================================================== */

static const char *texto_rechazo(esp_mqtt_connect_return_code_t c)
{
    switch (c) {
    case MQTT_CONNECTION_REFUSE_PROTOCOL:
        return "version de protocolo no aceptada";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:
        return "identificador rechazado";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
        return "servidor no disponible";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
        return "usuario o clave incorrectos";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
        return "no autorizado (en un broker publico, tambien: demasiadas "
               "conexiones seguidas desde la misma IP)";
    default:
        return "?";
    }
}

static void informar_error(const esp_mqtt_error_codes_t *e)
{
    if (e == NULL) {
        return;
    }
    if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        ESP_LOGW(TAG, "[mqtt] error de red: errno %d (%s), esp-tls 0x%x",
                 e->esp_transport_sock_errno,
                 strerror(e->esp_transport_sock_errno),
                 (unsigned)e->esp_tls_last_esp_err);
    } else if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_LOGW(TAG, "[mqtt] el broker rechazo la conexion: codigo %d, %s",
                 (int)e->connect_return_code,
                 texto_rechazo(e->connect_return_code));
    } else {
        ESP_LOGW(TAG, "[mqtt] error de tipo %d", (int)e->error_type);
    }
}

/* Una orden llegó al tema de comandos. Aquí NO se interpreta: se copia y se
 * encola. Mientras este manejador corre, el cliente MQTT entero está parado:
 * ni lee, ni manda el keepalive, ni publica lo de las demás tareas. */
static void recibir_comando(esp_mqtt_event_handle_t ev)
{
    comando_t c = { 0 };

    /* Solo mensajes enteros y cortos. Uno largo llega en trozos
     * (current_data_offset > 0): ninguna orden válida es tan larga. */
    if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len ||
        ev->data_len <= 0 || ev->data_len >= (int)sizeof(c.texto)) {
        contar_rechazado();
        return;
    }
    /* El tema y los datos NO terminan en '\0': se copian con su largo. */
    if (ev->topic_len != (int)strlen(s_tema_comando) ||
        memcmp(ev->topic, s_tema_comando, ev->topic_len) != 0) {
        contar_rechazado();
        return;
    }
    memcpy(c.texto, ev->data, ev->data_len);
    c.retenida = ev->retain;            /* lo decide "comandos", no el manejador */

    if (xQueueSend(s_cola_comandos, &c, 0) != pdTRUE) {
        contar_rechazado();                     /* cola llena: se descarta */
    }
}

static void al_evento_mqtt(void *arg, esp_event_base_t base, int32_t id,
                           void *datos)
{
    esp_mqtt_event_handle_t ev = datos;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        red_estado_t w;
        red_wifi_estado(&w);
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - w.marca_ip_us) / 1000);

        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.conexiones_mqtt++;
        if (s_estado.conexiones_mqtt == 1) {
            s_estado.ms_ip_a_mqtt = ms;
        }
        xSemaphoreGive(s_mutex_estado);

        ESP_LOGI(TAG, "[mqtt] conectado a %s como %s, %" PRIu32 " ms despues "
                      "de la IP", MQTT_BROKER, s_id, ms);

        /* Con sesión limpia, el broker olvida las suscripciones en cada
         * desconexión: hay que suscribirse en CADA conexión, no una vez. */
        esp_mqtt_client_subscribe(ev->client, s_tema_comando, 1);
        publicar_estado();

        /* Y ahora sí: "publicador" despierta y vacía lo acumulado. */
        xEventGroupSetBits(s_eventos, EV_MQTT_OK);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_eventos, EV_MQTT_OK);
        ESP_LOGW(TAG, "[mqtt] desconectado del broker; el cliente reintenta solo");
        break;

    case MQTT_EVENT_DATA:
        recibir_comando(ev);
        break;

    case MQTT_EVENT_ERROR:
        informar_error(ev->error_handle);
        break;

    default:
        break;
    }
}

/* ======================================================================== */
/*  TAREA sensor:  periódica, produce muestras; nunca espera a la red        */
/* ======================================================================== */

/* Mete la muestra en la cola. Si está llena —la red lleva caída más de
 * COLA_MUESTRAS_LARGO periodos— se tira la MÁS ANTIGUA y entra la nueva:
 * importa más saber qué pasa ahora que qué pasó hace diez minutos.
 *
 * "publicador" no puede colarse entre el xQueueReceive() y el xQueueSend():
 * tiene menos prioridad que "sensor", hay un solo núcleo, y ninguna de las
 * dos llamadas se bloquea (plazo 0). */
static void encolar_muestra(const muestra_t *m)
{
    if (xQueueSend(s_cola_muestras, m, 0) == pdTRUE) {
        return;
    }
    muestra_t vieja;
    (void)xQueueReceive(s_cola_muestras, &vieja, 0);
    (void)xQueueSend(s_cola_muestras, m, 0);

    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    s_estado.perdidas++;
    xSemaphoreGive(s_mutex_estado);
}

static void tarea_sensor(void *arg)
{
    uint32_t secuencia = 0;
    TickType_t proximo = xTaskGetTickCount();

    while (1) {
        float celsius = 0.0f;
        if (temperature_sensor_get_celsius(s_tsens, &celsius) == ESP_OK) {
            int rssi = 0;
            if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) {
                rssi = 0;                                /* sin red */
            }
            /* El driver da float; se pasa a centésimas AQUÍ, redondeando, y
             * el resto del programa no vuelve a ver un float. */
            const muestra_t m = {
                .marca_us = esp_timer_get_time(),
                .secuencia = secuencia++,
                .centigrados_x100 = (int32_t)(celsius * 100.0f +
                                              (celsius >= 0.0f ? 0.5f : -0.5f)),
                .microfono_x100 = microfono_nivel_x100(),
                .rssi = (int8_t)rssi,
            };
            encolar_muestra(&m);

            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.medidas++;
            s_estado.ultima_x100 = m.centigrados_x100;
            xSemaphoreGive(s_mutex_estado);
        } else {
            ESP_LOGW(TAG, "[sensor] lectura fuera de rango, se descarta");
        }

        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        const uint32_t periodo_s = s_estado.periodo_s;
        xSemaphoreGive(s_mutex_estado);

        /* Espera al próximo instante, contado desde el anterior, como
         * xTaskDelayUntil(). Pero esta espera se puede CORTAR: si "comandos"
         * cambia el periodo, avisa con una notificación y el sensor mide ya,
         * sin terminar de esperar el periodo viejo (que podía ser de 10 min). */
        proximo += pdMS_TO_TICKS(periodo_s * 1000);
        const TickType_t ahora = xTaskGetTickCount();
        const TickType_t falta = proximo - ahora;        /* aritmética modular */

        if (falta > 0 && falta <= pdMS_TO_TICKS(PERIODO_MAX_S * 1000)) {
            if (ulTaskNotifyTake(pdTRUE, falta) > 0) {
                proximo = xTaskGetTickCount();          /* periodo nuevo */
            }
        } else {
            proximo = ahora;          /* el instante ya pasó: no se recupera */
        }
    }
}

/* ======================================================================== */
/*  TAREA publicador:  saca muestras de la cola y las publica                */
/* ======================================================================== */

/* Prioridad 4. Es la única tarea del programa que espera a la red, y por
 * eso es una tarea aparte: esp_mqtt_client_publish() puede tardar segundos
 * (hasta 10 s de plazo de red), y si lo llamara "sensor", el sensor perdería
 * sus plazos cada vez que la red va lenta. */
static void tarea_publicador(void *arg)
{
    muestra_t m;
    char json[112];

    while (1) {
        /* 1. BLOQUEADA hasta que haya WiFi Y broker (pdTRUE: los dos bits).
         *    Mientras tanto las muestras se acumulan en la cola. */
        xEventGroupWaitBits(s_eventos, EV_WIFI_OK | EV_MQTT_OK,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* 2. La muestra más antigua. Con plazo: si no llega ninguna en 1 s,
         *    se vuelve arriba a comprobar que la conexión sigue. */
        if (xQueueReceive(s_cola_muestras, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        /* 3. Publicar. QoS 0 y retenido: el panel recibe la última lectura
         *    en cuanto se suscribe, aunque la placa mida cada diez minutos. */
        const int64_t t0 = esp_timer_get_time();
        formatear_muestra(json, sizeof(json), &m,
                          (uint32_t)((t0 - m.marca_us) / 1000));
        
        int id = esp_mqtt_client_publish(s_mqtt, s_tema_temperatura,
                                  json, 0, 0, 1);
        
        if (id >= 0) {
            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            const bool habilitado = s_estado.microfono_habilitado;
            xSemaphoreGive(s_mutex_estado);

            if (habilitado) {
                char db[16];
                x100_a_texto(db, sizeof(db), m.microfono_x100);

                snprintf(json, sizeof(json),
                         "{\"dbfs\":%s,\"seq\":%" PRIu32 ",\"edad\":%" PRIu32 "}",
                         db, m.secuencia,
                         (uint32_t)((t0 - m.marca_us) / 1000));

                id = esp_mqtt_client_publish(
                    s_mqtt, s_tema_microfono, json, 0, 0, 1);
            }
        }

        const uint32_t dur_us = (uint32_t)(esp_timer_get_time() - t0);

        if (id < 0) {
            /* No salió: la conexión se cayó justo ahora. La muestra vuelve
             * al FRENTE de la cola, para no desordenar la serie, y se espera
             * un poco: el manejador todavía no habrá borrado EV_MQTT_OK. */
            const bool devuelta = xQueueSendToFront(s_cola_muestras, &m, 0) == pdTRUE;
            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.devueltas++;
            if (!devuelta) {
                s_estado.perdidas++;
            }
            xSemaphoreGive(s_mutex_estado);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.publicadas++;
        if (dur_us > s_estado.publicar_max_us) {
            s_estado.publicar_max_us = dur_us;
        }
        xSemaphoreGive(s_mutex_estado);

        xTaskNotify(s_tarea_interfaz, AVISO_DESTELLO, eSetBits);
    }
}

/* ======================================================================== */
/*  TAREA comandos:  interpreta las órdenes que llegan del panel             */
/* ======================================================================== */

/* Tres órdenes, en texto:
 *
 *   periodo=N     segundos entre medidas, de 1 a 600
 *   identificar   el LED parpadea en blanco 5 s (¿cuál de las veinte es?)
 *   ping=XYZ      responde XYZ en el tema eco: el panel mide la ida y vuelta
 *
 * Cualquiera puede escribir en un broker público. Por eso cada orden se
 * comprueba entera antes de aplicarla, las retenidas se descartan, y todo lo
 * que se descarta se cuenta. */
static void tarea_comandos(void *arg)
{
    comando_t c;

    while (xQueueReceive(s_cola_comandos, &c, portMAX_DELAY) == pdTRUE) {
        long valor = 0;

        if (c.retenida) {
            /* Una orden retenida la guarda el broker y la vuelve a entregar
             * en CADA reconexión: quien la dejó ahí manda sobre la placa para
             * siempre. El panel nunca retiene órdenes; si llega una así, la
             * escribió otro. Se descarta. */
            sanear(c.texto);
            ESP_LOGW(TAG, "[comandos] orden retenida descartada: \"%s\"", c.texto);
            contar_rechazado();

        } else if (strncmp(c.texto, "periodo=", 8) == 0 &&
            leer_entero(c.texto + 8, PERIODO_MIN_S, PERIODO_MAX_S, &valor)) {

            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.periodo_s = (uint32_t)valor;
            s_estado.comandos_ok++;
            xSemaphoreGive(s_mutex_estado);

            if (s_tarea_sensor != NULL) {
                xTaskNotifyGive(s_tarea_sensor); /* que no espere el periodo viejo */
            }
            publicar_estado();                   /* confirmación para el panel     */
            ESP_LOGI(TAG, "[comandos] periodo = %ld s", valor);
        
        } else if (strcmp(c.texto, "microfono=0") == 0 ||
                   strcmp(c.texto, "microfono=1") == 0) {

            const bool habilitar = strcmp(c.texto, "microfono=1") == 0;

            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.microfono_habilitado = habilitar;
            s_estado.comandos_ok++;
            xSemaphoreGive(s_mutex_estado);

            publicar_estado();
            ESP_LOGI(TAG, "[comandos] microfono = %s",
                     habilitar ? "activado" : "desactivado");


        } else if (strcmp(c.texto, "identificar") == 0) {
            xTaskNotify(s_tarea_interfaz, AVISO_IDENTIFICAR, eSetBits);
            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.comandos_ok++;
            xSemaphoreGive(s_mutex_estado);
            ESP_LOGI(TAG, "[comandos] identificar: LED en blanco %d s",
                     IDENTIFICAR_MS / 1000);

        } else if (strncmp(c.texto, "ping=", 5) == 0 && testigo_valido(c.texto + 5)) {
            esp_mqtt_client_publish(s_mqtt, s_tema_eco, c.texto + 5, 0, 0, 0);
            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.comandos_ok++;
            xSemaphoreGive(s_mutex_estado);

        } else {
            sanear(c.texto);
            ESP_LOGW(TAG, "[comandos] orden rechazada: \"%s\"", c.texto);
            contar_rechazado();
        }
    }
}

/* ======================================================================== */
/*  TAREA interfaz:  el LED cuenta el estado de la conexión                  */
/* ======================================================================== */

/*   rojo           sin WiFi
 *   azul           con WiFi, sin broker
 *   verde tenue    conectado; un destello verde con cada publicación
 *   blanco         parpadea 5 s con la orden "identificar"
 */
static void tarea_interfaz(void *arg)
{
    int64_t identificar_hasta = 0;
    int64_t destello_hasta = 0;
    uint32_t color_anterior = UINT32_MAX;

    while (1) {
        uint32_t avisos = 0;
        xTaskNotifyWait(0, UINT32_MAX, &avisos, pdMS_TO_TICKS(100));

        const int64_t ahora = esp_timer_get_time();
        if (avisos & AVISO_IDENTIFICAR) {
            identificar_hasta = ahora + (int64_t)IDENTIFICAR_MS * 1000;
        }
        if (avisos & AVISO_DESTELLO) {
            destello_hasta = ahora + 150 * 1000;
        }

        const EventBits_t bits = xEventGroupGetBits(s_eventos);
        uint8_t r = 0, g = 0, b = 0;
        if (ahora < identificar_hasta) {
            if ((ahora / 250000) % 2 == 0) {            /* 2 Hz */
                r = g = b = 40;
            }
        } else if (!(bits & EV_WIFI_OK)) {
            r = 24;
        } else if (!(bits & EV_MQTT_OK)) {
            b = 24;
        } else if (ahora < destello_hasta) {
            g = 64;
        } else {
            g = 6;
        }

        /* El LED solo se escribe cuando cambia el color. */
        const uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        if (color != color_anterior) {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, r, g, b));
            ESP_ERROR_CHECK(led_strip_refresh(s_led));
            color_anterior = color;
        }
    }
}

/* ======================================================================== */
/*  TEMPORIZADOR SOFTWARE:  informe                                          */
/* ======================================================================== */

static const char *nombre_estado(eTaskState s)
{
    switch (s) {
    case eRunning:   return "EJECUTANDO";
    case eReady:     return "LISTA";
    case eBlocked:   return "BLOQUEADA";
    case eSuspended: return "SUSPENDIDA";
    case eDeleted:   return "BORRADA";
    default:         return "?";
    }
}

/* Todas las tareas, de mayor a menor prioridad: las del programa y las que
 * creó ESP-IDF (wifi, sys_evt, tcpip, mqtt_task...). */
static void informe_de_tareas(void)
{
    const UBaseType_t capacidad = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *t = malloc(capacidad * sizeof(TaskStatus_t));
    if (t == NULL) {
        return;
    }
    const UBaseType_t n = uxTaskGetSystemState(t, capacidad, NULL);

    for (UBaseType_t i = 1; i < n; i++) {               /* inserción */
        const TaskStatus_t x = t[i];
        UBaseType_t j = i;
        while (j > 0 && t[j - 1].uxCurrentPriority < x.uxCurrentPriority) {
            t[j] = t[j - 1];
            j--;
        }
        t[j] = x;
    }

    ESP_LOGI(TAG, "  %-12s %4s  %-11s %s", "tarea", "prio", "estado", "pila libre");
    for (UBaseType_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %-12s %4u  %-11s %" PRIu32 " B", t[i].pcTaskName,
                 (unsigned)t[i].uxCurrentPriority,
                 nombre_estado(t[i].eCurrentState),
                 (uint32_t)t[i].usStackHighWaterMark);
    }
    free(t);
}

/* Corre en la tarea "Tmr Svc" (prioridad 1). Copia bajo el mutex e imprime
 * después: nunca se imprime con el mutex tomado. */
static void informe(TimerHandle_t timer)
{
    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    const estado_t e = s_estado;
    xSemaphoreGive(s_mutex_estado);

    red_estado_t w;
    red_wifi_estado(&w);
    const EventBits_t bits = xEventGroupGetBits(s_eventos);

    ESP_LOGI(TAG, "--- Informe de %s (cada %d s) ------------------------",
             s_id, INFORME_PERIODO_MS / 1000);
    informe_de_tareas();

    ESP_LOGI(TAG, "  WiFi %s, MQTT %s;  conexiones WiFi %" PRIu32
                  ", desconexiones %" PRIu32 ", conexiones MQTT %" PRIu32,
             (bits & EV_WIFI_OK) ? "SI" : "no", (bits & EV_MQTT_OK) ? "SI" : "no",
             w.conexiones, w.desconexiones, e.conexiones_mqtt);
    ESP_LOGI(TAG, "  arranque: IP a los %" PRIu32 " ms, broker %" PRIu32
                  " ms despues de la IP", w.ms_hasta_ip, e.ms_ip_a_mqtt);
    ESP_LOGI(TAG, "  medidas %" PRIu32 ", publicadas %" PRIu32 ", en cola %u/%d, "
                  "perdidas %" PRIu32 ", devueltas %" PRIu32,
             e.medidas, e.publicadas,
             (unsigned)uxQueueMessagesWaiting(s_cola_muestras),
             COLA_MUESTRAS_LARGO, e.perdidas, e.devueltas);
    ESP_LOGI(TAG, "  ordenes aceptadas %" PRIu32 ", rechazadas %" PRIu32
                  ";  periodo %" PRIu32 " s;  publish mas lento %" PRIu32 " us",
             e.comandos_ok, e.comandos_rechazados, e.periodo_s,
             e.publicar_max_us);
    ESP_LOGI(TAG, "  heap libre: %" PRIu32 " B  (minimo historico %" PRIu32 " B)",
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size());
}

/* ======================================================================== */
/*  Arranque                                                                 */
/* ======================================================================== */

static void configurar_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led));
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, 24, 12, 0));    /* ámbar */
    ESP_ERROR_CHECK(led_strip_refresh(s_led));
}

/* Sensor interno del ESP32-S2: mide la temperatura DEL CHIP, no la del aire.
 * Con la radio transmitiendo, el chip se calienta. */
static void configurar_sensor(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_tsens));
}

/* Identificador y temas. Cada placa necesita un identificador DISTINTO: si
 * dos clientes se conectan con el mismo, el broker echa al primero, el
 * primero reconecta y echa al segundo, y así sin fin. */
static void construir_temas(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_id, sizeof(s_id), "kaluga-%02x%02x%02x", mac[3], mac[4], mac[5]);

    snprintf(s_tema_temperatura, sizeof(s_tema_temperatura),
             MQTT_RAIZ "/%s/temperatura", s_id);
    snprintf(s_tema_microfono, sizeof(s_tema_microfono),
             MQTT_RAIZ "/%s/microfono", s_id);
    snprintf(s_tema_estado, sizeof(s_tema_estado), MQTT_RAIZ "/%s/estado", s_id);
    snprintf(s_tema_comando, sizeof(s_tema_comando), MQTT_RAIZ "/%s/comando", s_id);
    snprintf(s_tema_eco, sizeof(s_tema_eco), MQTT_RAIZ "/%s/eco", s_id);
}

static void crear_cliente_mqtt(void)
{
    /* Reintento del broker: 10 s más un azar de hasta 5 s, distinto en cada
     * placa. Los brokers públicos limitan las conexiones seguidas desde una
     * misma IP, y un aula entera sale a Internet por una sola IP. */
    const int reintento_ms = 10000 + (int)(esp_random() % 5000);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER,
        .credentials.client_id = s_id,
        .session = {
            .keepalive = MQTT_KEEPALIVE_S,
            /* La ÚLTIMA VOLUNTAD: el broker la publica en nombre de la placa
             * si deja de oírla sin una despedida. Retenida, como el estado. */
            .last_will = {
                .topic = s_tema_estado,
                .msg = ESTADO_DESCONECTADO,
                .qos = 1,
                .retain = 1,
            },
        },
        .network.reconnect_timeout_ms = reintento_ms,
        .task = {
            .priority = PRIO_MQTT,
            .stack_size = PILA_MQTT,
        },
    };

    /* Con mqtts:// el broker presenta un certificado y hay que comprobarlo:
     * contra las autoridades públicas que trae ESP-IDF. Es la Clase 7. */
    if (strncmp(MQTT_BROKER, "mqtts://", 8) == 0) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_mqtt = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(s_mqtt != NULL ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY,
                                                   al_evento_mqtt, NULL));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== nodo_mqtt: la Kaluga-1 como nodo IoT ===");

    configurar_led();
    configurar_sensor();
    microfono_iniciar();

    /* --- Objetos del kernel, ANTES de las tareas que los usan ------------ */
    s_eventos       = xEventGroupCreate();
    s_cola_muestras = xQueueCreate(COLA_MUESTRAS_LARGO, sizeof(muestra_t));
    s_cola_comandos = xQueueCreate(COLA_COMANDOS_LARGO, sizeof(comando_t));
    s_mutex_estado  = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK((s_eventos && s_cola_muestras && s_cola_comandos &&
                     s_mutex_estado) ? ESP_OK : ESP_ERR_NO_MEM);

    /* --- Red e identidad --------------------------------------------------- */
    const red_config_t red = {
        .ssid = WIFI_SSID,
        .clave = WIFI_CLAVE,
        .seguridad = WIFI_SEGURIDAD,
    };
    ESP_ERROR_CHECK(red_wifi_iniciar(&red, s_eventos));
    construir_temas();
    ESP_LOGI(TAG, "ID de esta placa: %s", s_id);
    ESP_LOGI(TAG, "  publica en  %s", s_tema_temperatura);
    ESP_LOGI(TAG, "  publica en  %s", s_tema_microfono);
    ESP_LOGI(TAG, "  escucha en  %s", s_tema_comando);
    crear_cliente_mqtt();

    /* --- Las tareas --------------------------------------------------------
     *
     * Primero interfaz, que recibe avisos de las demás: una tarea de
     * prioridad mayor que app_main empieza a correr dentro del mismo
     * xTaskCreate(), y avisar a una tarea que aún no existe es un assert
     * (Clase 5). El sensor también recibe avisos, de "comandos", pero las
     * órdenes solo pueden llegar cuando arranque el cliente MQTT, al final. */
    xTaskCreate(tarea_interfaz, "interfaz", PILA_INTERFAZ, NULL, PRIO_INTERFAZ,
                &s_tarea_interfaz);
    xTaskCreate(tarea_comandos, "comandos", PILA_COMANDOS, NULL, PRIO_COMANDOS, NULL);
    xTaskCreate(tarea_publicador, "publicador", PILA_PUBLICADOR, NULL,
                PRIO_PUBLICADOR, NULL);
    xTaskCreate(tarea_sensor, "sensor", PILA_SENSOR, NULL, PRIO_SENSOR,
                &s_tarea_sensor);
    ESP_ERROR_CHECK((s_tarea_interfaz && s_tarea_sensor) ? ESP_OK : ESP_ERR_NO_MEM);

    s_timer_informe = xTimerCreate("informe", pdMS_TO_TICKS(INFORME_PERIODO_MS),
                                   pdTRUE, NULL, informe);
    ESP_ERROR_CHECK(s_timer_informe != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTimerStart(s_timer_informe, pdMS_TO_TICKS(100)) == pdPASS
                        ? ESP_OK : ESP_FAIL);

    /* --- A conectar --------------------------------------------------------
     *
     * El cliente MQTT se arranca con la primera IP. Arrancarlo antes no
     * rompe nada, pero su primer intento fallaría y el siguiente llegaría
     * 10 s más tarde. Después, si la red se cae y vuelve, reconecta solo. */
    ESP_ERROR_CHECK(red_wifi_conectar());
    xEventGroupWaitBits(s_eventos, EV_WIFI_OK, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));

    ESP_LOGI(TAG, "Arranque completo. app_main termina; las tareas siguen.");
}
