/*
 * wifi_estacion — Clase 6 (Sistemas Embebidos, UCU 2026)
 *
 * Ejemplo 1. La Kaluga-1 se conecta a una red WiFi y enseña cada paso: qué
 * redes ve y con qué seguridad, con cuál se conecta, cuánto tarda, por qué se
 * cae, y si desde esa red se llega al broker MQTT que usa el ejemplo 2.
 *
 *   app_main:  red_wifi_iniciar() ── escanear_redes() ── red_wifi_conectar()
 *
 *   [sys_evt p20] eventos WiFi e IP ──aviso──▶ [conexion p3] reintentos
 *        │                                       (red_wifi.c)
 *        └── bit EV_WIFI_OK ──▶ [prueba p4]   DNS + TCP al broker, cada 60 s
 *                               [interfaz p2] LED según el estado
 *
 *   [timer software] informe cada 20 s: TODAS las tareas, también las que
 *                    crea ESP-IDF para la red (wifi, tcpip, sys_evt...)
 *
 * Lo que hay que ver:
 *   - la red son TAREAS, y de prioridad más alta que las del programa;
 *   - el manejador de eventos no espera: anota, avisa y vuelve;
 *   - ninguna tarea pregunta "¿hay red?" en un bucle: se bloquean en el bit
 *     EV_WIFI_OK del grupo de eventos y despiertan cuando se pone.
 *
 * Hardware: solo la placa, con el JUMPER RGB colocado (LED en GPIO45).
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "led_strip.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "red_wifi.h"

/* ======================================================================== */
/*  CONFIGURACIÓN DE LA RED: editar antes de compilar                        */
/* ======================================================================== */

/* El nombre de la red (SSID), tal como lo enseña el teléfono. */
#define WIFI_SSID       "nombre-de-tu-red"

/* La clave de la red: la misma para todos los que se conectan a ella. */
#define WIFI_CLAVE      "clave-de-tu-red"

/* Seguridad de la red: descomenta UNA sola línea. Es el MÍNIMO que se acepta:
 * si la red ofrece algo mejor, la placa lo negocia sola. El escaneo del
 * arranque dice qué seguridad usa tu red. */
//#define WIFI_SEGURIDAD_WPA        /* WPA  (TKIP): routers muy antiguos   */
#define WIFI_SEGURIDAD_WPA2         /* WPA2 (AES):  la gran mayoría        */
//#define WIFI_SEGURIDAD_WPA3       /* WPA3 (SAE):  redes WPA3 o WPA2/WPA3 */

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

/* La prueba de alcance: el broker del ejemplo 2 y sus dos puertos TCP. */
#define PRUEBA_HOST         "broker.hivemq.com"
#define PRUEBA_PUERTO_MQTT  1883     /* MQTT sin cifrar                      */
#define PRUEBA_PUERTO_TLS   8883     /* MQTT sobre TLS (Clase 7)             */
#define PRUEBA_PLAZO_MS     5000     /* sin respuesta en 5 s: bloqueado       */
#define PRUEBA_PERIODO_MS   60000

#define RGB_LED_GPIO        45
#define RGB_LED_COUNT       1

/* --- Prioridades ---------------------------------------------------------
 *
 * Por DEBAJO de toda la pila de red de ESP-IDF: wifi 23, sys_evt 20, tcpip 18.
 * La guía de ESP-IDF lo pide para cualquier tarea que use sockets: si una
 * tarea de la aplicación le quita la CPU a tcpip, se queda esperando la
 * respuesta de quien no la deja correr. */
#define PRIO_PRUEBA         4
#define PRIO_INTERFAZ       2

#define PILA_PRUEBA         4096     /* getaddrinfo() y los sockets piden pila */
#define PILA_INTERFAZ       3072

#define INFORME_PERIODO_MS  20000
#define MAX_REDES           16       /* las 16 de mejor señal               */

static const char *TAG = "wifi_estacion";

/* ======================================================================== */
/*  Estado                                                                   */
/* ======================================================================== */

/* Resultado de la última prueba de alcance. Lo escribe "prueba"; lo leen
 * "interfaz" y el informe. */
typedef struct {
    bool     hecha;           /* hubo al menos una prueba con red          */
    bool     alcanzable;      /* algún puerto del broker respondió         */
    uint32_t pruebas;
    uint32_t ms_dns;
    int32_t  ms_mqtt;         /* conexión TCP al 1883; -1 si no respondió   */
    int32_t  ms_tls;          /* conexión TCP al 8883; -1 si no respondió   */
} prueba_t;

static EventGroupHandle_t s_eventos;
static SemaphoreHandle_t  s_mutex_prueba;
static prueba_t           s_prueba = { .ms_mqtt = -1, .ms_tls = -1 };
static TimerHandle_t      s_timer_informe;
static led_strip_handle_t s_led;

/* ======================================================================== */
/*  LED                                                                      */
/* ======================================================================== */

static void led(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_led));
}

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
    ESP_ERROR_CHECK(led_strip_clear(s_led));
}

/* ======================================================================== */
/*  Escaneo: qué redes ve la placa y qué seguridad usan                      */
/* ======================================================================== */

static bool es_empresarial(wifi_auth_mode_t m)
{
    return m == WIFI_AUTH_WPA_ENTERPRISE || m == WIFI_AUTH_ENTERPRISE ||
           m == WIFI_AUTH_WPA3_ENTERPRISE ||
           m == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || m == WIFI_AUTH_WPA3_ENT_192;
}

/* El WPA más alto que ofrece una red de clave compartida: 1, 2 o 3. Un 0 si
 * no es WPA con clave compartida. */
static int nivel_maximo(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_WPA_PSK:
        return 1;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return 2;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return 3;
    default:
        return 0;
    }
}

/* Compara lo que ofrece la red con lo configurado y dice qué cambiar. */
static void aconsejar(wifi_auth_mode_t red)
{
    static const char *LINEA[] = { "", "WIFI_SEGURIDAD_WPA",
                                   "WIFI_SEGURIDAD_WPA2", "WIFI_SEGURIDAD_WPA3" };
    static const char *NIVEL[] = { "", "WPA", "WPA2", "WPA3" };
    const int pedido = (int)WIFI_SEGURIDAD + 1;
    const int maximo = nivel_maximo(red);

    if (es_empresarial(red)) {
        ESP_LOGW(TAG, "  -> la red pide un usuario a cada persona (802.1X): "
                      "este ejemplo solo se conecta a redes de clave compartida");
    } else if (maximo == 0) {
        ESP_LOGW(TAG, "  -> la red es %s: este ejemplo solo se conecta a redes "
                      "WPA, WPA2 o WPA3", red_wifi_nombre_seguridad(red));
    } else if (maximo < pedido) {
        ESP_LOGW(TAG, "  -> la red llega como mucho a %s y se pidio %s: "
                      "descomenta %s", NIVEL[maximo], NIVEL[pedido], LINEA[maximo]);
    } else {
        ESP_LOGI(TAG, "  -> la seguridad configurada (%s como minimo) sirve para "
                      "esta red", NIVEL[pedido]);
    }
}

/* Escaneo activo de los 13 canales. Se hace ANTES de conectar: con una
 * conexión en curso, esp_wifi_scan_start() falla. Bloquea a app_main unos
 * dos segundos; a las demás tareas no. */
static void escanear_redes(void)
{
    ESP_LOGI(TAG, "Escaneando los canales de 2,4 GHz...");
    led(16, 0, 16);                                   /* violeta: escaneando */

    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "escaneo: %s", esp_err_to_name(err));
        return;
    }

    uint16_t total = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&total));

    /* En el heap y no en la pila: 16 registros son más de 1 KB. */
    wifi_ap_record_t *redes = calloc(MAX_REDES, sizeof(*redes));
    if (redes == NULL) {
        esp_wifi_clear_ap_list();                     /* libera la lista del driver */
        return;
    }
    uint16_t n = MAX_REDES;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n, redes));   /* por RSSI */

    ESP_LOGI(TAG, "%u redes en %" PRIu32 " ms; las %u de mejor senal:",
             total, ms, n);
    ESP_LOGI(TAG, "  canal   RSSI  seguridad        red");

    int mia = -1;
    for (int i = 0; i < n; i++) {
        const char *nombre = (const char *)redes[i].ssid;
        const bool es_mia = strcmp(nombre, WIFI_SSID) == 0;
        if (es_mia && mia < 0) {
            mia = i;                    /* la primera es la de mejor señal */
        }
        ESP_LOGI(TAG, "  %5u  %5d  %-15s  %s%s", redes[i].primary, redes[i].rssi,
                 red_wifi_nombre_seguridad(redes[i].authmode),
                 nombre[0] != '\0' ? nombre : "(oculta)",
                 es_mia ? "   <-- la configurada" : "");
    }

    if (mia < 0) {
        ESP_LOGW(TAG, "No se ve \"%s\" entre las %u de mejor senal. El ESP32-S2 "
                      "solo tiene radio de 2,4 GHz: una red de 5 GHz no aparece. "
                      "Revisa tambien mayusculas y espacios.", WIFI_SSID, n);
    } else {
        ESP_LOGI(TAG, "\"%s\": canal %u, %d dBm, %s", WIFI_SSID,
                 redes[mia].primary, redes[mia].rssi,
                 red_wifi_nombre_seguridad(redes[mia].authmode));
        aconsejar(redes[mia].authmode);
    }
    free(redes);
}

/* ======================================================================== */
/*  TAREA prueba:  ¿se llega al broker desde esta red?                       */
/* ======================================================================== */

/* Conexión TCP con plazo. Es lo primero que hace cualquier cliente MQTT, y
 * en una red con cortafuegos es lo primero que falla. Devuelve 1 si el
 * puerto aceptó, 0 si no hubo respuesta en el plazo, -1 si se rechazó. */
static int probar_tcp(const struct sockaddr_in *destino, uint16_t puerto,
                      uint32_t *ms)
{
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        return -1;
    }

    /* No bloqueante: connect() vuelve en el acto y select() espera con
     * plazo. Con un connect() bloqueante, el plazo lo pondría lwIP, y son
     * decenas de segundos. */
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in d = *destino;
    d.sin_port = htons(puerto);

    const int64_t t0 = esp_timer_get_time();
    int r = connect(s, (const struct sockaddr *)&d, sizeof(d));
    if (r < 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }

    fd_set escritura;
    FD_ZERO(&escritura);
    FD_SET(s, &escritura);
    struct timeval plazo = {
        .tv_sec = PRUEBA_PLAZO_MS / 1000,
        .tv_usec = (PRUEBA_PLAZO_MS % 1000) * 1000,
    };
    r = select(s + 1, NULL, &escritura, NULL, &plazo);
    *ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    int resultado = 0;                                /* sin respuesta */
    if (r > 0) {
        int error = 0;
        socklen_t largo = sizeof(error);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &largo);
        resultado = (error == 0) ? 1 : -1;
    }
    close(s);
    return resultado;
}

static void informar_tcp(uint16_t puerto, int r, uint32_t ms)
{
    if (r > 0) {
        ESP_LOGI(TAG, "[prueba] TCP %u: abierto, conexion en %" PRIu32 " ms",
                 puerto, ms);
    } else if (r == 0) {
        ESP_LOGW(TAG, "[prueba] TCP %u: sin respuesta en %d s (cortafuegos?)",
                 puerto, PRUEBA_PLAZO_MS / 1000);
    } else {
        ESP_LOGW(TAG, "[prueba] TCP %u: conexion rechazada", puerto);
    }
}

static void probar_alcance(void)
{
    /* 1. DNS: el nombre del broker a una dirección IP. */
    const struct addrinfo pista = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    const int64_t t0 = esp_timer_get_time();
    const int err = getaddrinfo(PRUEBA_HOST, NULL, &pista, &res);
    const uint32_t ms_dns = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (err != 0 || res == NULL) {
        ESP_LOGW(TAG, "[prueba] DNS: no se pudo resolver %s (error %d)",
                 PRUEBA_HOST, err);
        xSemaphoreTake(s_mutex_prueba, portMAX_DELAY);
        s_prueba.hecha = true;
        s_prueba.alcanzable = false;
        s_prueba.pruebas++;
        xSemaphoreGive(s_mutex_prueba);
        return;
    }

    struct sockaddr_in destino;
    memcpy(&destino, res->ai_addr, sizeof(destino));
    freeaddrinfo(res);

    char ip[16];
    inet_ntoa_r(destino.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "[prueba] DNS: %s -> %s en %" PRIu32 " ms",
             PRUEBA_HOST, ip, ms_dns);

    /* 2. TCP a los dos puertos del broker. */
    uint32_t ms_mqtt = 0, ms_tls = 0;
    const int r_mqtt = probar_tcp(&destino, PRUEBA_PUERTO_MQTT, &ms_mqtt);
    informar_tcp(PRUEBA_PUERTO_MQTT, r_mqtt, ms_mqtt);
    const int r_tls = probar_tcp(&destino, PRUEBA_PUERTO_TLS, &ms_tls);
    informar_tcp(PRUEBA_PUERTO_TLS, r_tls, ms_tls);

    if (r_mqtt <= 0 && r_tls <= 0) {
        ESP_LOGW(TAG, "[prueba] desde esta red no se llega al broker: prueba "
                      "otra red (el telefono como punto de acceso, por ejemplo)");
    }

    xSemaphoreTake(s_mutex_prueba, portMAX_DELAY);
    s_prueba.hecha = true;
    s_prueba.alcanzable = (r_mqtt > 0) || (r_tls > 0);
    s_prueba.pruebas++;
    s_prueba.ms_dns = ms_dns;
    s_prueba.ms_mqtt = (r_mqtt > 0) ? (int32_t)ms_mqtt : -1;
    s_prueba.ms_tls = (r_tls > 0) ? (int32_t)ms_tls : -1;
    xSemaphoreGive(s_mutex_prueba);
}

/* Prioridad 4. Se pasa la vida BLOQUEADA: en el bit EV_WIFI_OK mientras no
 * hay red, y en vTaskDelay() entre una prueba y la siguiente. No pregunta
 * nunca "¿hay red?" en un bucle. */
static void tarea_prueba(void *arg)
{
    while (1) {
        xEventGroupWaitBits(s_eventos, EV_WIFI_OK,
                            pdFALSE,          /* no borra el bit al salir     */
                            pdTRUE,           /* todos los bits pedidos (uno) */
                            portMAX_DELAY);
        probar_alcance();
        vTaskDelay(pdMS_TO_TICKS(PRUEBA_PERIODO_MS));
    }
}

/* ======================================================================== */
/*  TAREA interfaz:  el LED cuenta el estado de la conexión                  */
/* ======================================================================== */

/* Prioridad 2, la más baja: si el LED cambia 200 ms tarde, nadie lo nota.
 *
 *   rojo   sin red todavía, o reintentando
 *   ámbar  con IP; la prueba del broker todavía no terminó
 *   azul   con IP, pero el broker no responde desde esta red
 *   verde  con IP y el broker alcanzable
 */
static void tarea_interfaz(void *arg)
{
    while (1) {
        const bool con_red = (xEventGroupGetBits(s_eventos) & EV_WIFI_OK) != 0;

        xSemaphoreTake(s_mutex_prueba, portMAX_DELAY);
        const prueba_t p = s_prueba;
        xSemaphoreGive(s_mutex_prueba);

        if (!con_red) {
            led(24, 0, 0);
        } else if (!p.hecha) {
            led(24, 12, 0);
        } else if (!p.alcanzable) {
            led(0, 0, 24);
        } else {
            led(0, 24, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
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

/* Todas las tareas del sistema, de mayor a menor prioridad. Las de la red
 * no las creó este programa: las crearon esp_netif_init(), el bucle de
 * eventos y esp_wifi_init(). Y están por encima de todas las nuestras. */
static void informe_de_tareas(void)
{
    const UBaseType_t capacidad = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *t = malloc(capacidad * sizeof(TaskStatus_t));
    if (t == NULL) {
        return;
    }
    const UBaseType_t n = uxTaskGetSystemState(t, capacidad, NULL);

    /* Inserción: son una docena, no merece nada más listo. */
    for (UBaseType_t i = 1; i < n; i++) {
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

/* Corre en la tarea "Tmr Svc" (prioridad 1). No se bloquea: lee, copia e
 * imprime. */
static void informe(TimerHandle_t timer)
{
    red_estado_t w;
    red_wifi_estado(&w);

    xSemaphoreTake(s_mutex_prueba, portMAX_DELAY);
    const prueba_t p = s_prueba;
    xSemaphoreGive(s_mutex_prueba);

    ESP_LOGI(TAG, "--- Informe (cada %d s) -----------------------------------",
             INFORME_PERIODO_MS / 1000);
    informe_de_tareas();

    if (w.conectada) {
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);
        ESP_LOGI(TAG, "  WiFi: IP " IPSTR ", canal %u, %s, %d dBm",
                 IP2STR(&w.ip), w.canal, red_wifi_nombre_seguridad(w.seguridad),
                 rssi);
    } else {
        ESP_LOGI(TAG, "  WiFi: sin conexion; proximo intento tras %" PRIu32 " ms",
                 w.espera_ms);
    }
    ESP_LOGI(TAG, "  conexiones %" PRIu32 ", desconexiones %" PRIu32
                  ", primera IP a los %" PRIu32 " ms",
             w.conexiones, w.desconexiones, w.ms_hasta_ip);
    if (w.desconexiones > 0) {
        ESP_LOGI(TAG, "  ultima desconexion: motivo %u, %s", w.ultimo_motivo,
                 red_wifi_motivo(w.ultimo_motivo));
    }
    if (p.hecha) {
        ESP_LOGI(TAG, "  broker %s: %s  (DNS %" PRIu32 " ms, TCP 1883 %" PRId32
                      " ms, TCP 8883 %" PRId32 " ms; -1 = sin respuesta)",
                 PRUEBA_HOST, p.alcanzable ? "alcanzable" : "NO alcanzable",
                 p.ms_dns, p.ms_mqtt, p.ms_tls);
    }
    ESP_LOGI(TAG, "  heap libre: %" PRIu32 " B  (minimo historico %" PRIu32 " B)",
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size());
}

/* ======================================================================== */
/*  main                                                                     */
/* ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== wifi_estacion: la Kaluga-1 en una red WiFi ===");

    configurar_led();
    led(24, 12, 0);                                   /* ámbar: arrancando */

    s_eventos = xEventGroupCreate();
    s_mutex_prueba = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK((s_eventos && s_mutex_prueba) ? ESP_OK : ESP_ERR_NO_MEM);

    /* 1. Pila de red y driver. Aquí nacen las tareas tcpip, sys_evt y wifi. */
    const red_config_t red = {
        .ssid = WIFI_SSID,
        .clave = WIFI_CLAVE,
        .seguridad = WIFI_SEGURIDAD,
    };
    ESP_ERROR_CHECK(red_wifi_iniciar(&red, s_eventos));

    /* 2. Qué redes hay y qué seguridad usan. Antes de conectar. */
    escanear_redes();

    /* 3. Las tareas del programa. Primero las que esperan el bit: así ya
     *    están bloqueadas en él cuando la conexión lo ponga. */
    xTaskCreate(tarea_interfaz, "interfaz", PILA_INTERFAZ, NULL, PRIO_INTERFAZ, NULL);
    xTaskCreate(tarea_prueba, "prueba", PILA_PRUEBA, NULL, PRIO_PRUEBA, NULL);

    /* 4. A conectar. Desde aquí, la tarea "conexion" se ocupa de reintentar. */
    ESP_ERROR_CHECK(red_wifi_conectar());

    s_timer_informe = xTimerCreate("informe", pdMS_TO_TICKS(INFORME_PERIODO_MS),
                                   pdTRUE, NULL, informe);
    ESP_ERROR_CHECK(s_timer_informe != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTimerStart(s_timer_informe, pdMS_TO_TICKS(100)) == pdPASS
                        ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Arranque completo. app_main termina; las tareas siguen.");
}
