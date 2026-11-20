# panel_web — el otro terminal (Clase 6)

Una página web que muestra las lecturas de todas las placas de la clase y les manda órdenes. Es **otro terminal MQTT**, igual que la placa: se suscribe a los temas de temperatura, estado y eco, y publica en el de comandos. La placa y el panel no se hablan nunca directamente; los dos hablan con el **broker**.

![El panel en modo demostración](../../imagen/panel-web.png)

## Cómo se usa

1. Abre [`index.html`](index.html) con **doble clic**. No hace falta instalar nada ni levantar un servidor: necesita Internet para llegar al broker y para cargar la biblioteca MQTT.js.
2. Se conecta sola al broker de HiveMQ con la raíz `ucu-se2026`. Si la placa usa otro broker, cámbialo arriba: **tienen que ser el mismo**.
3. Cada placa aparece en su tarjeta, por orden alfabético, en cuanto publica. Con el **filtro** se deja solo la tuya.

| En la tarjeta | Qué es |
|---|---|
| Número grande | La última temperatura, con coma decimal |
| «ahora», «hace 12 s» | Cuándo llegó; en **naranja** si pasan más de tres periodos sin noticias de una placa que dice estar en línea |
| «último valor retenido (hora desconocida)» | Lo que el broker guardaba de una placa que no está publicando ahora |
| La gráfica | Los últimos 10 minutos, cada punto en el instante en que se **midió** (llegada menos `edad`) |
| RSSI, periodo, secuencia, IP | Lo que la placa contó en su último mensaje y en su estado |
| Huecos | Medidas que no llegaron, por saltos en `seq`; también cuenta los reinicios |
| **Identificar** | La placa parpadea en blanco 5 s |
| **Ping** | Ida y vuelta panel → broker → placa → broker → panel, en ms |
| **Periodo…** | Segundos entre medidas; la placa **confirma** republicando su estado |

El registro de abajo («Mensajes recibidos») enseña los temas y los mensajes tal como llegan del broker, con los retenidos marcados.

## Opciones en la dirección

| Al final de la dirección | Qué hace |
|---|---|
| `?demo` | Simula tres placas y una que ya no está; no se conecta a ningún broker. Sirve para probar el panel sin placa |
| `?raiz=grupo-3` | Usa otra raíz de temas (la misma que `MQTT_RAIZ` en la placa) |
| `?tema=claro` o `?tema=oscuro` | Fuerza el tema; si no, sigue el del sistema |

Se combinan con `&`: `index.html?demo&tema=claro`.

## Cómo funciona

- **MQTT dentro de un WebSocket.** Un navegador no puede abrir una conexión TCP al puerto 1883. Abre un **WebSocket seguro** (`wss://broker.hivemq.com:8884/mqtt`) y habla MQTT por dentro; el broker traduce. La placa usa TCP en el 1883, el panel WSS en el 8884, y los dos se ven porque comparten broker, no transporte.
- **MQTT 5 en el panel, 3.1.1 en la placa.** Tampoco importa: cada terminal negocia su versión con el broker. El panel usa la 5 porque, si el broker rechaza la conexión, explica el motivo (por ejemplo, `0x9F`: demasiadas conexiones desde esta IP).
- **Suscripciones:** `ucu-se2026/+/temperatura`, `ucu-se2026/+/estado` y `ucu-se2026/+/eco`. El `+` es el comodín de un nivel: una sola suscripción para todas las placas.
- **Órdenes:** a `ucu-se2026/<ID>/comando`, con QoS 1 y **sin retener**.
- **Reintentos:** si se cae la conexión, espera 5 s, luego 10, 20… hasta 60, con un poco de azar. Los brokers públicos limitan las conexiones seguidas desde una misma IP, y el aula entera comparte una.

## Lo que llega de un broker público es entrada no confiable

Cualquiera puede publicar en `ucu-se2026/...`. El panel lo trata así:

- solo acepta identificadores de 1 a 32 caracteres `[A-Za-z0-9_-]`, mensajes de hasta 512 bytes, JSON que se pueda leer y temperaturas entre −40 y 150 °C;
- como mucho 60 placas: un *script* que publique miles de temas no llena la página;
- **escribe siempre con `textContent`, nunca con `innerHTML`**: un identificador como `<img src=x onerror=...>` aparece como texto, no se ejecuta.

## Alternativas sin programar

- El **cliente web de HiveMQ** (*MQTT Browser Client*, enlazado desde la página de su broker público): conectar a `broker.hivemq.com`, puerto `8884`, con SSL, y suscribirse a `ucu-se2026/#`. Enseña los mensajes en crudo, y sirve para publicar órdenes a mano.
- Cualquier aplicación de **paneles MQTT** para el teléfono, configurando un indicador para el tema `ucu-se2026/<ID>/temperatura` y el campo `t` del JSON.

## Estado

Probado en Chrome, en modo demostración y contra el broker público de HiveMQ con una placa **simulada** que habla MQTT 3.1.1 con los mismos temas, mensajes y comprobaciones que `nodo_mqtt`: el panel recibió las lecturas y el estado, y las tres órdenes llegaron a la placa simulada sin el indicador de retenido. Con una Kaluga real no se ha probado todavía.
