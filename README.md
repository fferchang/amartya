# Amartya — nodo de nivel de cisterna

Firmware del nodo que mide el nivel de agua de una cisterna: un **ESP32-C3
SuperMini** con un sensor ultrasónico estanco **JSN-SR04T** montado en la tapa.
El nodo mide la distancia hasta el agua y la manda por WiFi, como JSON, a un
servidor.

> **Estado: prototipo probado con el sensor real.** Mide y publica de punta a
> punta. Todavía no es el firmware definitivo: publica cada 5 segundos (un ritmo
> de prueba), no guarda lecturas si se corta el WiFi y habla HTTP plano. El
> transporte definitivo se acuerda con UMA NET.

## El dato que publica el nodo

Cada lectura es un objeto JSON:

```json
{
  "node_id": "cisterna-1",
  "timestamp_unix": 1790292145,
  "valid": true,
  "distance_cm": 73.6,
  "rssi_dbm": -47
}
```

| campo | tipo | qué es |
|---|---|---|
| `node_id` | texto | quién mide. Se configura en `NODE_ID` |
| `timestamp_unix` | entero | cuándo se midió, en segundos UTC, con la hora de NTP (ver la regla 4) |
| `valid` | booleano | si el ultrasónico devolvió eco |
| `distance_cm` | número, 1 decimal | distancia desde la cara del sensor hasta el agua: la mediana de ~20 disparos. **No viene** si `valid` es `false` |
| `rssi_dbm` | entero | señal WiFi del nodo, para diagnóstico |

Cuando el sensor no recibe eco, la lectura viaja igual, sin `distance_cm`:

```json
{ "node_id": "cisterna-1", "timestamp_unix": 1790292150, "valid": false, "rssi_dbm": -47 }
```

El formato ya prevé `temperature_c` y `humidity_pct` para un nodo con sensor de
clima. Este no los manda: una magnitud que no se mide es una clave que no está.

### Cuatro reglas para quien recibe el dato

1. **Una clave que no está es "no se midió"; nunca es `0`.** Cero centímetros
   hasta el agua es tanque **lleno**: un sensor roto que mandara `0` se vería
   como la mejor noticia posible.
2. **`valid: false` es "no sé", no "vacío".** Sin eco puede ser un cable
   suelto, el sensor roto o un tanque más hondo que el alcance del sensor. Se
   muestra como una falla del equipo, no como un nivel de agua.
3. **El nodo manda distancia, no porcentaje.** El nivel depende de la geometría
   de cada tanque, y así cambiar de tanque no obliga a reprogramar un equipo que
   está arriba de una cisterna. La cuenta, del lado de quien recibe:

   ```
   nivel % = (dist_fondo − distance_cm) / (dist_fondo − dist_lleno) × 100
   ```

   recortada entre 0 y 100. Las dos distancias se miden desde la cara del
   sensor: hasta el fondo, y hasta el agua con el tanque lleno. `dist_lleno` no
   puede ser menor que 25 cm (ver *Montaje*).
4. **Un `timestamp_unix` anterior al 1/1/2020 es un reloj sin sincronizar, no
   una fecha.** Pasa en los primeros segundos después de un reinicio, antes de
   que NTP conteste: el nodo manda los segundos desde que arrancó. Quien recibe
   tiene que fechar esa lectura con la hora de llegada, y no tirarla. Si prefiere
   fechar todas las lecturas por su cuenta, puede ignorar el campo.

### Cómo viaja

Un `POST` a la dirección de `API_URL`, con estos encabezados:

```
Content-Type: application/json
Authorization: Bearer <API_TOKEN>
```

| respuesta | qué hace el nodo |
|---|---|
| `201` | la da por aceptada |
| `429` | espera lo que diga `Retry-After` (en segundos; 10 si no viene) antes del próximo envío |
| `400`, `401`, `403` u otra | lo informa por el monitor serie y sigue con la próxima lectura |
| ninguna (no conecta, o conecta y no contesta en 1,5 s) | lo mismo. La lectura se pierde: no hay buffer |

## Hardware

- ESP32-C3 SuperMini.
- JSN-SR04T (sonda estanca más su placa), en modo trigger/echo: **el lugar
  marcado R27 en su placa tiene que estar vacío**. Con una resistencia ahí, el
  módulo habla por UART y este firmware nunca ve un eco.
- Resistencias para un divisor 1 a 2 (ver abajo).

### Cableado

```
 JSN-SR04T                ESP32-C3 SuperMini
   5V   ───────────────── 5V
   GND  ───────────────── G
   Trig ───────────────── 3
   Echo ──[1k]──┬──────── 4
                └─[2k]─── G
```

**El divisor en Echo es obligatorio.** Alimentado a 5 V, el módulo devuelve el
eco a 5 V, y los pines del C3 no toleran más de 3,3. El divisor tiene que ser 1
a 2: 1k y 2k, o 22k en serie y dos de 22k en serie hacia GND. **Dos resistencias
iguales no sirven**: dan 2,5 V, justo en el umbral en que el C3 lee un "1", y se
pierden ecos.

GPIO5 y GPIO6 quedan libres para un sensor de clima por I2C. No usar GPIO2,
GPIO8 ni GPIO9: vienen con algo conectado de fábrica en esta placa (strapping,
el LED y el botón BOOT).

### Montaje

- **Adentro de la cisterna va solo la sonda.** El ESP32 y la placa del JSN van
  afuera, en una caja estanca: con la humedad que hay arriba del agua, una placa
  se corroe en semanas. El divisor, soldado.
- **La sonda, perpendicular al agua**, pasada por un agujero de la tapa y
  sujeta con un prensacable o con silicona neutra. Con cinta doble faz no: con la
  humedad se despega, y el sensor termina en el agua.
- **Al menos 25 cm por encima del nivel de rebalse.** Más cerca, el sensor está
  en su zona ciega: en vez de avisar que no mide, devuelve unos 20 cm, que se
  leerían como tanque lleno.
- **Lejos de la entrada de agua** (el chorro hace olas) **y de paredes, caños y
  escaleras**, que devuelven ecos antes que el agua.

## Compilar y flashear

Hace falta [PlatformIO](https://platformio.org/): la extensión de VS Code o la
línea de comandos.

1. Copiar `include/config.local.h.example` a `include/config.local.h` y
   completarlo: la red WiFi (solo 2,4 GHz), `API_URL`, `API_TOKEN` y `NODE_ID`.
   Ese archivo está en el `.gitignore` porque tiene contraseñas.
2. Con la placa conectada por USB:

   ```
   pio run -t upload
   pio device monitor
   ```

   La primera compilación baja las herramientas del C3 (~200 MB) y tarda.

El monitor serie muestra una línea por envío:

```
WiFi: conectado, IP 192.168.137.45, RSSI -47 dBm
== publica: mediana 48.7 cm (20/20 con eco)  -> 201 aceptada
```

**Para flashear, el monitor tiene que estar cerrado**, con Ctrl+C: el puerto lo
usa un solo programa a la vez, y cerrar la pestaña de la terminal a veces deja el
monitor andando por detrás.

### Qué se puede ajustar

Todo está en `src/pipeline_http.ino`, comentado:

| constante | hoy | qué es |
|---|---|---|
| `CICLO_MS` | 5000 | cada cuánto publica. 5 s es para pruebas; en operación alcanza con 1 por minuto o menos |
| `PERIODO_DISPARO_MS` | 250 | cada cuánto dispara el sensor. No menos de 100: el eco anterior tiene que apagarse |
| `IMPRIMIR_CADA_DISPARO` | `false` | en `true` (con el período en 100), el monitor muestra cada disparo en vivo |
| `TEMPERATURA_AIRE_C` | 20 | temperatura supuesta para la velocidad del sonido. De 0 a 30 °C la distancia cambia un 5,5 % |

## Si algo no anda

| en el monitor | causa probable |
|---|---|
| en blanco | esperar 5 s (hay una línea por envío). Si sigue en blanco, desenchufar y volver a enchufar la placa con el monitor abierto |
| `WiFi: no conecto` | red de 5 GHz, contraseña equivocada, o el router lejos |
| `sin respuesta (connection refused)` | `API_URL` equivocada, o el servidor apagado |
| `sin respuesta (read Timeout)` | un firewall del lado del servidor |
| `401` o `403` | `API_TOKEN` no es el que espera el servidor |
| `sin eco -> valid:false` | el cableado de Echo, R27, o nada delante del sensor |
| pocos ecos con el sensor quieto (`8/20 con eco`) | el divisor da menos de 3,3 V (dos resistencias iguales), o la superficie es irregular o inclinada |
| siempre ~20 cm | hay algo a menos de 25 cm: zona ciega |

## Medido con el sensor real

- **Estable**: a distancia fija, entre 48,7 y 49,1 cm. Avanza en pasos de
  ~0,4 cm, que es lo más fino que mide el módulo.
- **Zona ciega**: ~20 cm.
- **Alcance**: ~2,3 m apuntando a superficies de una habitación. El módulo
  promete ~4,5 m, y sobre agua quieta, que es un blanco mucho mejor, debería
  llegar más lejos.

## Pendiente

- Acordar con UMA NET el transporte (HTTP, HTTPS o MQTT) y el ritmo de
  publicación.
- **HTTPS.** Hoy habla HTTP plano, así que el token viaja sin cifrar. Sirve en
  una red local; por internet hace falta TLS, y probablemente más de 1,5 s de
  espera, porque un ESP32 tarda en negociarlo.
- Un buffer para los cortes de WiFi, si el acuerdo lo pide.
- Compensar la temperatura con un sensor de verdad, en vez de suponer 20 °C.
