| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# wifi_estacion — Ejemplo 1 (Clase 6)

La **ESP32-S2-Kaluga-1** se conecta a una red WiFi y enseña cada paso: qué redes ve y con qué seguridad, con cuál se conecta, cuánto tarda, por qué se cae y si desde esa red se llega al broker MQTT que usa el ejemplo 2. Es la base de red de la clase: el módulo `red_wifi.c` que usa este ejemplo es **el mismo archivo** que usa [`../nodo_mqtt/`](../nodo_mqtt/).

```
   app_main:  red_wifi_iniciar() ── escanear_redes() ── red_wifi_conectar()

   [sys_evt p20] eventos WiFi e IP ──aviso──▶ [conexion p3] reintentos
        │                                       (red_wifi.c)
        └── bit EV_WIFI_OK ──▶ [prueba p4]   DNS + TCP al broker, cada 60 s
                               [interfaz p2] LED según el estado

   [timer software] informe cada 20 s: TODAS las tareas, también las que
                    crea ESP-IDF para la red (wifi, tcpip, sys_evt...)
```

Lo que hay que ver funcionando:

- la red son **tareas**, y de prioridad **más alta** que las del programa;
- el **manejador de eventos no espera**: anota, avisa a una tarea y vuelve;
- ninguna tarea pregunta «¿hay red?» en un bucle: se **bloquean** en el bit `EV_WIFI_OK` de un **grupo de eventos** y despiertan cuando se pone;
- el driver WiFi **no reconecta solo**: lo hace la tarea `conexion`, con una espera que se dobla en cada fallo.

## Hardware

- Solo la **ESP32-S2-Kaluga-1**. No hace falta ninguna placa de extensión.
- **LED RGB direccionable** en **GPIO45**. ⚠️ **Coloca el jumper RGB**: sin él, el LED no recibe señal.
- La antena es la de PCB del módulo **ESP32-S2-WROVER**: no hay que conectar nada.
- ⚠️ **Kit v1.2:** si la placa de audio sigue montada de la Clase 5, desmóntala para flashear (el GPIO46 queda en alto y el chip no entra en modo de descarga). Para esta clase no hace falta.

## Configuración: lo único que hay que editar

Arriba del todo de [`main/wifi_estacion_main.c`](main/wifi_estacion_main.c):

```c
/* El nombre de la red (SSID), tal como lo enseña el teléfono. */
#define WIFI_SSID       "nombre-de-tu-red"

/* La clave de la red: la misma para todos los que se conectan a ella. */
#define WIFI_CLAVE      "clave-de-tu-red"

/* Seguridad de la red: descomenta UNA sola línea. Es el MÍNIMO que se acepta:
 * si la red ofrece algo mejor, la placa lo negocia sola. */
//#define WIFI_SEGURIDAD_WPA        /* WPA  (TKIP): routers muy antiguos   */
#define WIFI_SEGURIDAD_WPA2         /* WPA2 (AES):  la gran mayoría        */
//#define WIFI_SEGURIDAD_WPA3       /* WPA3 (SAE):  redes WPA3 o WPA2/WPA3 */
```

Si se descomentan dos líneas de seguridad, o ninguna, **no compila**: un `#error` lo dice. Y si no sabes qué seguridad usa tu red, déjalo en WPA2 y compila igual: **el escaneo del arranque te lo dice**.

| Lo que dice el router o el teléfono | Línea que se descomenta |
|---|---|
| WPA2-Personal, WPA2-PSK, «WPA2 (AES)» | `WIFI_SEGURIDAD_WPA2` |
| WPA2/WPA3, «WPA3 Transition» | `WIFI_SEGURIDAD_WPA2` o `_WPA3` |
| WPA3-Personal, «WPA3-SAE» | `WIFI_SEGURIDAD_WPA3` |
| WPA/WPA2, «WPA-PSK (TKIP)» | `WIFI_SEGURIDAD_WPA` |

- **El define es un mínimo.** Con WPA2 pedido y una red WPA2/WPA3, la placa conecta con WPA3: lo ves en la línea `asociada al punto de acceso: ... seguridad WPA3`.
- **Redes de clave compartida.** La de casa, la de la universidad y el teléfono como punto de acceso funcionan igual: un nombre de red y una clave, la misma para todos. Una red que pide un usuario a cada persona (WPA2-Enterprise, 802.1X) no sirve para este ejemplo, y el escaneo del arranque lo avisa.
- ⚠️ **La clave queda escrita en el código.** Antes de hacer *commit*, vuelve a poner `"clave-de-tu-red"`. Un repositorio público con la clave de tu casa es un problema que no se arregla borrando el archivo: queda en el historial.

## Las tareas

| Tarea | Prio | De quién | Espera en | Trabajo |
|---|:-:|---|---|---|
| **wifi** | 23 | ESP-IDF | — | El driver: la radio, las tramas, la autenticación |
| **sys_evt** | 20 | ESP-IDF | Su cola de eventos | Reparte los eventos: llama a `al_evento()` |
| **tcpip** | 18 | ESP-IDF | Su buzón de mensajes | La pila TCP/IP (lwIP): DHCP, DNS, sockets |
| **prueba** | 4 | Este programa | `EV_WIFI_OK`, y `vTaskDelay` | DNS y TCP al broker cada 60 s |
| **conexion** | 3 | `red_wifi.c` | Una notificación | Reintenta con espera de 1, 2, 4… hasta 60 s |
| **interfaz** | 2 | Este programa | `vTaskDelay(200 ms)` | El LED |

> Las prioridades de ESP-IDF salen de su guía de rendimiento (*Built-in Task Priorities*) y del código de lwIP: en ESP-IDF v6 la tarea de lwIP se llama `tcpip`; en versiones anteriores, `tiT`. El informe imprime los nombres y las prioridades reales de tu compilación.

La regla que fija las nuestras, de la misma guía: **una tarea que usa la red va por debajo de `tcpip` (18)**. Si le quitara la CPU, se quedaría esperando una respuesta que nunca llega, porque quien la tiene que dar no puede correr.

## Colores del LED

| Color | Significa |
|---|---|
| **Ámbar** | Arrancando, o con IP mientras corre la primera prueba |
| **Violeta** | Escaneando, en el arranque |
| **Rojo** | Sin red todavía, o reintentando |
| **Azul** | Hay IP, pero el broker no responde desde esta red |
| **Verde** | Hay IP y el broker es alcanzable |

## Estructura

```
wifi_estacion/
├── CMakeLists.txt
├── sdkconfig.defaults        Flash de 4 MB, partición grande, tick de 1 ms, informe
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     Declara la dependencia espressif/led_strip
│   ├── red_wifi.c / .h       La conexión WiFi (el mismo archivo que nodo_mqtt)
│   └── wifi_estacion_main.c  Configuración, escaneo, prueba de alcance, informe
└── README.md   (este archivo)
```

## Cómo compilar, flashear y monitorear

Desde VS Code, con los botones de la extensión de ESP-IDF (*Build*, *Flash*, *Monitor*): este ejemplo **no necesita ninguna opción `-D`**, toda la configuración está en el `.c`. O desde la terminal de ESP-IDF:

```bash
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

- Sustituye `/dev/cu.usbserial-XXXX` por tu puerto (`ls /dev/cu.*` en macOS). El de la UART es el segundo de los dos que crea la placa.
- Salir del monitor: `Ctrl + ]`.
- Si cambiaste `sdkconfig.defaults`, borra `sdkconfig` antes de compilar: solo se aplica al crearlo.

## Salida (ilustrativa)

Los nombres de red, los tiempos y las direcciones son de ejemplo: los tuyos dependen de tu red. El formato es el que imprime el programa.

```
I (312) wifi_estacion: === wifi_estacion: la Kaluga-1 en una red WiFi ===
I (398) red_wifi: WiFi listo: red "MiCasa", minimo WPA2
I (399) wifi_estacion: Escaneando los canales de 2,4 GHz...
I (2511) wifi_estacion: 9 redes en 2112 ms; las 9 de mejor senal:
I (2512) wifi_estacion:   canal   RSSI  seguridad        red
I (2513) wifi_estacion:       6    -47  WPA2/WPA3        MiCasa   <-- la configurada
I (2514) wifi_estacion:      11    -71  WPA2             Vecino_2G
I (2515) wifi_estacion:       1    -79  WPA/WPA2         Impresora-3F2A
I (2521) wifi_estacion: "MiCasa": canal 6, -47 dBm, WPA2/WPA3
I (2522) wifi_estacion:   -> la seguridad configurada (WPA2 como minimo) sirve para esta red
I (2530) wifi_estacion: Arranque completo. app_main termina; las tareas siguen.
I (4870) red_wifi: asociada al punto de acceso: canal 6, seguridad WPA3; pidiendo IP por DHCP
I (5912) red_wifi: IP 192.168.1.37  mascara 255.255.255.0  puerta de enlace 192.168.1.1
I (6094) wifi_estacion: [prueba] DNS: broker.hivemq.com -> 3.124.87.252 en 182 ms
I (6288) wifi_estacion: [prueba] TCP 1883: abierto, conexion en 193 ms
I (6483) wifi_estacion: [prueba] TCP 8883: abierto, conexion en 195 ms
```

Y cada 20 s, el informe. **Fíjate en el orden**: arriba del todo están las tareas que creó ESP-IDF al iniciar la red, no las nuestras.

```
I (20541) wifi_estacion: --- Informe (cada 20 s) -----------------------------------
I (20542) wifi_estacion:   tarea        prio  estado      pila libre
I (20543) wifi_estacion:   wifi           23  BLOQUEADA   ...
I (20544) wifi_estacion:   esp_timer      22  BLOQUEADA   ...
I (20545) wifi_estacion:   sys_evt        20  BLOQUEADA   ...
I (20546) wifi_estacion:   tcpip          18  BLOQUEADA   ...
I (20547) wifi_estacion:   prueba          4  BLOQUEADA   ...
I (20548) wifi_estacion:   conexion        3  BLOQUEADA   ...
I (20549) wifi_estacion:   interfaz        2  BLOQUEADA   ...
I (20550) wifi_estacion:   Tmr Svc         1  EJECUTANDO  ...
I (20551) wifi_estacion:   IDLE            0  LISTA       ...
I (20552) wifi_estacion:   WiFi: IP 192.168.1.37, canal 6, WPA3, -48 dBm
I (20553) wifi_estacion:   conexiones 1, desconexiones 0, primera IP a los 3382 ms
I (20554) wifi_estacion:   broker broker.hivemq.com: alcanzable  (DNS 182 ms, TCP 1883 193 ms, TCP 8883 195 ms; -1 = sin respuesta)
I (20555) wifi_estacion:   heap libre: ...
```

> Este ejemplo **no se ha ejecutado todavía en una placa**: compila sin *warnings* en todas sus variantes, pero la salida de arriba está escrita a partir de los formatos del programa, no capturada. Las pilas libres y el heap se dejan en `...` a propósito: son los números que tienes que medir tú.

## Cómo leer una desconexión

Cada caída llega con un **motivo**, y el programa lo traduce. Los más frecuentes:

| Motivo | Nombre | Qué suele ser |
|:-:|---|---|
| 15 / 204 | `4WAY_HANDSHAKE_TIMEOUT` / `HANDSHAKE_TIMEOUT` | La **clave está mal** (WPA2) |
| 201 | `NO_AP_FOUND` | No se ve la red: nombre mal escrito, o es de **5 GHz** |
| 202 | `AUTH_FAIL` | Autenticación rechazada; con WPA3, la clave |
| 210 | `NO_AP_FOUND_W_COMPATIBLE_SECURITY` | La red usa una seguridad que no se configuró |
| 211 | `NO_AP_FOUND_IN_AUTHMODE_THRESHOLD` | La red es **menos segura** que el mínimo pedido |
| 200 | `BEACON_TIMEOUT` | Se dejó de oír al punto de acceso: lejos, o apagado |

La tabla completa está en `esp_wifi_types_generic.h` (`wifi_err_reason_t`) y en la guía de ESP-IDF, *Wi-Fi Reason Code*.

## La prueba de alcance

Es lo primero que hace **cualquier** cliente MQTT, y en una red con cortafuegos es lo primero que falla:

1. **DNS:** `broker.hivemq.com` a una dirección IP. Si falla, la red no tiene salida a Internet o el DNS no responde.
2. **TCP al 1883** (MQTT sin cifrar) y **al 8883** (MQTT sobre TLS). Si el 1883 no responde y el 8883 sí, la red bloquea el puerto: el ejemplo 2 puede usar `mqtts://` (ver su README).

Si no responde ninguno, prueba con el teléfono como punto de acceso: es la forma rápida de saber si el problema es la red o la placa.

## Detalles de diseño que conviene mirar en el código

- **El manejador de eventos no se bloquea.** Corre en la tarea `sys_evt`, que reparte **todos** los eventos del sistema. Un `vTaskDelay()` ahí dentro retrasaría también el de «ya tienes IP».
- **La reconexión es una tarea.** La espera crece al doble en cada fallo (1, 2, 4… hasta 60 s) y lleva un **azar** de hasta un 25 %: si se cae el router del aula, las placas no vuelven a llamar todas en el mismo milisegundo.
- **El escaneo va antes de conectar.** Con una conexión en curso, `esp_wifi_scan_start()` falla. Los resultados llegan ordenados por señal.
- **`esp_wifi_connect()` puede fallar sin generar evento.** Entonces la tarea no espera el aviso: se quedaría bloqueada para siempre.
- **La configuración no se guarda en la flash.** `esp_wifi_set_storage(WIFI_STORAGE_RAM)`: vive en el `.c`, y el driver no hace una copia que sobreviva a un cambio de programa.
- **`connect()` con plazo.** Con un socket no bloqueante y `select()`, cada prueba TCP tarda como mucho 5 s. Un `connect()` bloqueante esperaría lo que diga lwIP, decenas de segundos.

## Cosas para probar

| Experimento | Qué observar |
|---|---|
| Escribir mal la clave | Motivo 15 o 204, y esperas de 1, 2, 4… s entre intentos |
| Pedir WPA3 en una red WPA2 | Motivo 211, y el consejo del escaneo: «descomenta WIFI_SEGURIDAD_WPA2» |
| Poner el nombre de una red de 5 GHz | No aparece en el escaneo: motivo 201 |
| Apagar el router un minuto y encenderlo | La espera crece hasta 60 s; al volver, una conexión más en el informe |
| El teléfono como punto de acceso | Compara RSSI, tiempo hasta la IP, DNS y TCP con la red de casa |
| Un `vTaskDelay(pdMS_TO_TICKS(3000))` en `al_evento()` | La IP «llega» 3 s tarde, y todo lo demás también |
| Subir `PRIO_PRUEBA` a 19 | Sigue funcionando: la tarea pasa casi todo el tiempo bloqueada. Una que calculara sin parar dejaría a `tcpip` sin CPU |
| Bajar `PILA_PRUEBA` con la columna «pila libre» del informe | Hasta dónde se puede bajar con datos, sin adivinar |

## Solución de problemas

| Síntoma | Solución |
|---|---|
| `#error "Descomenta exactamente una..."` | Hay dos líneas `WIFI_SEGURIDAD_*` descomentadas, o ninguna |
| `una clave WPA tiene entre 8 y 63 caracteres` | Revisa `WIFI_CLAVE` |
| El escaneo dice `la red pide un usuario a cada persona (802.1X)` | Esa red no es de clave compartida: usa otra, o el teléfono como punto de acceso |
| No se ve la red en el escaneo | El ESP32-S2 solo tiene radio de **2,4 GHz**; muchos routers separan la red de 5 GHz con otro nombre |
| Motivo 15 o 204 en bucle | Clave incorrecta (mayúsculas, espacios al final) |
| Motivo 211 | Baja el mínimo: descomenta la línea que sugiere el escaneo |
| Hay IP, pero `TCP 1883: sin respuesta` | Cortafuegos de la red: prueba otra red o el puerto 8883 con TLS |
| El LED no enciende | Coloca el **jumper RGB** |
| `Wrong boot mode detected (0x4)` al flashear | Kit v1.2 con la placa de audio montada: desmóntala |
| `A stack overflow in task X` | Sube la pila de esa tarea y mira su «pila libre» en el informe |

## Referencias

- [../../detalles/wifi-en-el-esp32s2.md](../../detalles/wifi-en-el-esp32s2.md) · [../../detalles/red-y-rtos.md](../../detalles/red-y-rtos.md) · [../../detalles/seguridad-iot-introduccion.md](../../detalles/seguridad-iot-introduccion.md)
- Espressif, *ESP-IDF Programming Guide — Wi-Fi Driver* (*Station scenarios*, *Wi-Fi Reason Code*) y *Wi-Fi Security*.
- Espressif, *ESP-IDF Programming Guide — Speed Optimization*, apartado *Built-in Task Priorities*.
- Espressif, *ESP32-S2 Series Datasheet*, §4.3 *Wireless Communication* y tablas 5-7 y 6-4.
- Guía de conexión e instalación: [../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md](../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md)
