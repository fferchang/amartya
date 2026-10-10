# Amartya — nodo de nivel de cisterna

Firmware del nodo que mide el nivel de agua de una cisterna: un **ESP32-C3
SuperMini** con un sensor ultrasónico estanco **JSN-SR04T** montado en la tapa.
El nodo mide la distancia hasta el agua, la convierte en porcentaje con la
calibración del tanque y la publica por **MQTT sobre TLS**, una vez por minuto.

> **Estado: prototipo para el primer montaje.** Alimentado por fuente y con el
> WiFi del lugar. Mide solo nivel: no tiene sensor de temperatura ni mide
> batería. Batería, panel solar y enlace para sitios sin WiFi quedan para otra
> iteración.

## El dato que publica el nodo

Un número por minuto en el tópico **`sensores/<sitio>/nivel`**, como texto
plano (`62.4`), con **QoS 0 y sin retain**: el nivel de agua en % de la
**altura** útil del tanque, entre 0 y 100, con un decimal. No lleva hora ni ID:
la hora la pone quien recibe y el sitio va en el tópico. Es el formato que
confirmó UMA NET para la primera etapa.

**Sin un nivel creíble no se publica nada.** Con `sin_eco` o `zona_ciega` (ver
abajo) en texto plano no hay dónde decir "no sé", así que ese minuto queda como
hueco. El costo: del otro lado, un sensor roto se ve igual que un nodo apagado.

### La variante en JSON

UMA NET ofreció armar más adelante una versión en JSON para sumar sensores. El
nodo ya la tiene: con `PUBLICAR_JSON` en `true` (en `src/pipeline_mqtt.ino`)
publica un JSON por minuto en **`sensores/<sitio>/lectura`**, que sí puede
decir por qué no hay nivel:

```json
{ "estado": "ok", "nivel_pct": 62.4, "distancia_cm": 48.9, "rssi_dbm": -47 }
```

| campo | tipo | qué es |
|---|---|---|
| `estado` | texto | `ok`, `zona_ciega` o `sin_eco` (ver abajo) |
| `nivel_pct` | número, 1 decimal | nivel de agua en % de la **altura** útil del tanque, entre 0 y 100. **Solo viene si `estado` es `ok`** |
| `distancia_cm` | número, 1 decimal | distancia medida desde la cara del sensor hasta el agua: la mediana de los disparos de los últimos ~16 s. No viene si `estado` es `sin_eco` |
| `rssi_dbm` | entero | señal WiFi del nodo, para diagnóstico |

Los otros dos estados:

```json
{ "estado": "zona_ciega", "distancia_cm": 20.4, "rssi_dbm": -47 }
{ "estado": "sin_eco", "rssi_dbm": -47 }
```

### Cuatro reglas para quien recibe el dato

1. **Una clave que no está es "no se midió"; nunca es `0`.** 0% es tanque
   vacío: un sensor roto que mandara `0` dispararía una alarma falsa.
2. **`sin_eco` es "no sé", no "vacío".** Puede ser un cable suelto, el sensor
   roto o un tanque más hondo que el alcance del sensor. Se muestra como una
   falla del equipo, no como un nivel de agua.
3. **`zona_ciega` tampoco es "lleno".** Con algo a menos de 25 cm, el sensor no
   avisa que no puede medir: devuelve unos 20 cm, que convertidos darían 100%.
   El nodo lo detecta y no manda nivel. Si pasa seguido, hay algo delante de la
   sonda (condensación, una telaraña) o el agua subió más de lo previsto.
4. **Un minuto sin mensaje es un hueco**, no un valor. El nodo no guarda
   lecturas para reenviarlas: si el WiFi o el broker no están, esa lectura se
   pierde.

### Cómo se calcula el nivel

```
nivel % = (DIST_FONDO_CM − distancia_cm) / (DIST_FONDO_CM − DIST_LLENO_CM) × 100
```

recortado entre 0 y 100. Las dos distancias de calibración se miden desde la
cara del sensor: hasta el fondo, y hasta el agua con el tanque lleno. Son de
cada tanque y viven en el firmware, así que **cambiar de tanque es volver a
flashear**. Y en texto plano, si la calibración resulta estar mal, la historia
ya guardada no se puede corregir: por eso la variante JSON manda también
`distancia_cm`, con la que sí se puede recalcular.

Es porcentaje de **altura**, no de volumen. Los dos coinciden solo si el tanque
tiene la misma sección de arriba abajo (cilindro parado o prisma). Para un
tanque acostado o cónico hace falta una tabla de conversión, del lado de quien
recibe.

## Configuración

Todo lo de cada instalación está en `include/config.local.h` (se copia de
`config.local.h.example`, y está en el `.gitignore` porque tiene contraseñas).
**Ese archivo no se sube nunca a ningún repo**, y tampoco se comparte el
`.bin` compilado (`.pio/build/`): lleva las contraseñas adentro, en texto
plano.

| constante | qué es |
|---|---|
| `WIFI_SSID`, `WIFI_PASS` | la red del lugar. Solo 2,4 GHz |
| `MQTT_HOST`, `MQTT_PORT` | el broker. HiveMQ Cloud: el host del cluster y `8883` |
| `MQTT_TLS` | `1` para el broker real. `0` solo para probar contra un broker local sin cifrar |
| `MQTT_USER`, `MQTT_PASS` | las credenciales del broker |
| `SITIO` | va en el tópico. Sin espacios, acentos ni barras |
| `DIST_FONDO_CM`, `DIST_LLENO_CM` | la calibración del tanque, en cm. Si `DIST_LLENO_CM` es menor que 25, no compila |

El certificado con el que se valida al broker (ISRG Root X1, la raíz de Let's
Encrypt) va en el código: es público y vence en 2035. Verificado contra el
broker: HiveMQ Cloud usa la jerarquía nueva de Let's Encrypt (YR1 → Root YR) y
manda Root YR firmada por ISRG Root X1, que es lo que hace que esta raíz
alcance. Si en una renovación dejara de mandar esa firma, la conexión falla con
un error de TLS en el monitor (ver *Si algo no anda*) y hay que sumar Root YR
en el código.

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
   completarlo (ver *Configuración*).
2. Con la placa conectada por USB:

   ```
   pio run -t upload
   pio device monitor
   ```

   La primera compilación baja las herramientas del C3 (~200 MB) y la librería
   PubSubClient, y tarda.

El monitor serie muestra cada disparo (cuatro por segundo) y, una vez por
minuto, la línea de la publicación:

```
WiFi: conectado, IP 192.168.0.45, RSSI -47 dBm
MQTT: conectando a xxxx.s1.eu.hivemq.cloud:8883 como "amartya-huergo_prueba"...
MQTT: conectado.
    48.9 cm
    49.1 cm
  sin eco
    48.9 cm
== mediana 48.9 cm = 87.4% (63/64 con eco)  -> sensores/huergo_prueba/nivel 87.4
```

El nodo se conecta al broker **apenas arranca**, así que un error de
contraseña o de certificado aparece en el monitor a los pocos segundos. La
primera publicación sale **16 s después** (lo que tarda en llenarse la
primera ventana de mediciones) y de ahí en más, una por minuto.

**Para flashear, el monitor tiene que estar cerrado**, con Ctrl+C: el puerto lo
usa un solo programa a la vez, y cerrar la pestaña de la terminal a veces deja el
monitor andando por detrás.

### La tabla de particiones

`partitions.csv` reemplaza la tabla por defecto: saca la partición `spiffs`
(que no se usa), agranda las dos apps de OTA y reserva **una partición
`buffer` de 512 KB que hoy nadie usa**. Está pensada para guardar lecturas
mientras no haya enlace y reenviarlas después. Esa parte del firmware todavía
no existe, porque solo sirve si quien recibe acepta lecturas reenviadas con su
hora original.

Se declara desde ahora porque **la tabla de particiones no se cambia por OTA**,
solo por cable. Así, el día que se escriba ese código, alcanza con una
actualización remota.

`pio run -t upload` graba la tabla en cada flasheo, así que una placa que venía
con la tabla por defecto queda con la nueva sin hacer nada aparte.

### Qué se puede ajustar

En `src/pipeline_mqtt.ino`, comentado:

| constante | hoy | qué es |
|---|---|---|
| `PUBLICAR_JSON` | `false` | el nivel como texto en `/nivel`, o un JSON con todo en `/lectura` (ver arriba) |
| `CICLO_MS` | 60000 | cada cuánto publica. 5000 para ver moverse el dato en el banco |
| `PERIODO_DISPARO_MS` | 250 | cada cuánto dispara el sensor. No menos de 100: el eco anterior tiene que apagarse |
| `IMPRIMIR_CADA_DISPARO` | `true` | el monitor muestra cada disparo en vivo, para apuntar la sonda y calibrar. Sin una PC enchufada no frena nada. En `false`, solo la línea de cada publicación |
| `TEMPERATURA_AIRE_C` | 20 | temperatura supuesta para la velocidad del sonido. De 0 a 30 °C la distancia cambia un 5,5 % |
| `TOPE_TCP_S`, `TOPE_TLS_S`, `TOPE_MQTT_S` | 5, 10, 5 | cuánto se espera, **en segundos**, cada paso de la conexión al broker. Están elegidos para que una reconexión, en el peor caso, tarde menos que `CICLO_MS` y no se pierda la publicación siguiente |

## Si algo no anda

| en el monitor | causa probable |
|---|---|
| en blanco | con `IMPRIMIR_CADA_DISPARO` prendido aparecen cuatro líneas por segundo, así que un monitor quieto es que no está leyendo: desenchufar y volver a enchufar la placa con el monitor abierto |
| `WiFi: no conecto` | red de 5 GHz, contraseña equivocada, o el router lejos |
| `MQTT: no conecto (-2: ...)` con un renglón `TLS:` | el host o el puerto, o el certificado: el broker no usa la raíz de Let's Encrypt, o el reloj todavía no tiene hora |
| `MQTT: no conecto (-2: ...)` sin `TLS:` y con `MQTT_TLS` en 0 | el broker local apagado, o un firewall |
| `MQTT: no conecto (4: ...)` | usuario o contraseña del broker |
| `MQTT: no conecto (5: ...)` | el usuario existe pero no tiene permiso para conectarse |
| se conecta, publica y se desconecta enseguida | el usuario no tiene permiso para publicar en ese tópico: revisar `SITIO` contra lo que habilitó el broker |
| `sin eco` | el cableado de Echo, R27, o nada delante del sensor |
| pocos ecos con el sensor quieto (`80/240 con eco`) | el divisor da menos de 3,3 V (dos resistencias iguales), o la superficie es irregular o inclinada |
| `ZONA CIEGA` | hay algo a menos de 25 cm de la sonda |

## Medido con el sensor real

- **Estable**: a distancia fija, entre 48,7 y 49,1 cm. Avanza en pasos de
  ~0,4 cm, que es lo más fino que mide el módulo.
- **Zona ciega**: ~20 cm.
- **Alcance**: ~2,3 m apuntando a superficies de una habitación. El módulo
  promete ~4,5 m, y sobre agua quieta, que es un blanco mucho mejor, debería
  llegar más lejos.

## Pendiente

- Prueba de punta a punta junto con UMA NET (ya probado en placa contra el
  broker real con el sitio `huergo_prueba`: el dato llega a su tablero).
- Medir el tanque y cargar la calibración.
- Compensar la temperatura con un sensor de verdad, en vez de suponer 20 °C.
- Batería, panel y enlace para sitios sin WiFi (otra iteración).
