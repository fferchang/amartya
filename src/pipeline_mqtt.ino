// Nodo de nivel de cisterna: mide la distancia al agua con un JSN-SR04T, la
// convierte en porcentaje con la calibración del tanque y la publica por MQTT
// sobre TLS. El formato del dato está en el README; qué es y qué NO es todavía
// (un prototipo con fuente y WiFi del lugar, sin buffer), en platformio.ini.
//
// Cableado (dibujo completo en el README):
//   5V->5V  GND->G  Trig->GPIO3  Echo->divisor 1:2 (1k/2k, o 22k + 2x22k)->GPIO4
// El divisor NO es opcional: alimentado a 5 V el módulo devuelve el eco a 5 V,
// y los GPIO del C3 no lo toleran. R27 de la placa del JSN tiene que estar
// vacío (modo trigger/echo; con una resistencia ahí habla por UART y este
// código no ve nunca un eco).

// Error entendible si falta el archivo de secretos, en vez del "No such file"
// del compilador, que no dice qué hacer.
#if !__has_include("config.local.h")
#error "Falta include/config.local.h: copiar config.local.h.example y completarlo."
#endif
#include "config.local.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

// --- Formato del dato ---
// false: texto plano, un número por tópico (sensores/<SITIO>/nivel). Es lo que
// confirmó UMA NET el 08/10/2026 para esta primera etapa.
// true: un JSON con todo en sensores/<SITIO>/lectura. Queda para la versión en
// JSON que UMA NET ofreció armar más adelante, para sumar sensores.
// Es lo ÚNICO que cambia entre uno y otro, y vive en publicar().
static const bool PUBLICAR_JSON = false;

// Los tópicos se arman al compilar: SITIO es un literal, y dos literales
// seguidos en C se pegan en uno solo.
static const char* TOPICO_JSON  = "sensores/" SITIO "/lectura";
static const char* TOPICO_NIVEL = "sensores/" SITIO "/nivel";

// Identificador de este cliente ante el broker. TIENE QUE SER ÚNICO: si dos
// clientes se conectan con el mismo, el broker desconecta al primero, y dos
// nodos con el mismo SITIO se echarían entre sí cada vez que reconectan.
static const char* CLIENT_ID = "amartya-" SITIO;

// --- Sensor ---
// GPIO3/4 esquivan los pines del SuperMini que traen algo de fábrica: GPIO8
// (LED y strapping), GPIO9 (BOOT: en bajo durante un reset, no arranca el
// programa) y GPIO2 (strapping). GPIO5/6 quedan libres para un sensor de clima
// por I2C.
#define TRIG_PIN 3
#define ECHO_PIN 4

// 20 µs y no los 10 del HC-SR04: la versión 2.0 del JSN a veces no dispara con
// 10. El largo NO cambia el alcance: el pulso solo le dice al módulo cuándo
// empezar, y lo que emite hacia el agua es siempre la misma ráfaga.
static const uint32_t PULSO_TRIG_US = 20;

// Espera máxima del eco: 30 ms de ida y vuelta son ~5 m, más que los ~4,5 m
// que alcanza el módulo, así que esto no le recorta alcance.
static const uint32_t TIMEOUT_ECO_US = 30000;

// La velocidad del sonido depende de la temperatura (331,3 + 0,606·T m/s): de
// 0 a 30 °C cambia 5,5%, ~8 cm sobre 150 cm. Este nodo no tiene sensor de
// clima y la supone fija.
static const float TEMPERATURA_AIRE_C = 20.0f;

// Cada cuánto se dispara el sensor. El piso es 100 ms y no se baja más: el
// datasheet pide ≥ 50 ms entre disparos para que el eco del anterior —que sigue
// rebotando en las paredes— no se tome como el de este, y en un tanque cerrado
// rebota más que en una habitación.
static const uint32_t PERIODO_DISPARO_MS = 250;

// Imprimir cada disparo en el monitor serie, o solo la línea de cada
// publicación. PRENDIDO para la instalación: al apuntar la sonda y calibrar se
// ve la distancia en vivo, cuatro veces por segundo, y se compara con la cinta.
//
// Dejarlo prendido sin una PC enchufada no cuesta nada: sin nadie del otro lado
// del USB, el core descarta lo que no puede mandar en vez de esperar (ver
// HWCDC::write, que con la PC ausente solo vacía su buffer), así que el loop no
// se frena. Para mirar el sensor más fino, bajar PERIODO_DISPARO_MS a 100.
static const bool IMPRIMIR_CADA_DISPARO = true;

// Cada cuánto se PUBLICA: 1 por minuto, el ritmo de UMA NET. El nivel de una
// cisterna no cambia en segundos. Para ver moverse el dato con la mano en el
// banco, bajarlo a 5000.
static const uint32_t CICLO_MS = 60000;

// Cuántos disparos CON ECO se guardan para la mediana. Con 60 s a 250 ms la
// ventana tiene 240 disparos y se quedan los ÚLTIMOS 64, o sea los últimos
// ~16 s: lo que se publica es "el nivel ahora", y la parte más reciente de la
// ventana es la que mejor lo dice. 64 alcanzan de sobra para una mediana.
static const uint8_t MAX_DISPAROS_POR_VENTANA = 64;

// mediana() recorre el arreglo con un índice int8_t, que llega hasta 127. Si
// alguien sube el tope por encima de eso, que falle al compilar y no en silencio.
static_assert(MAX_DISPAROS_POR_VENTANA <= 127, "mediana() usa int8_t como indice");

// La PRIMERA publicación sale apenas la ventana juntó sus disparos (64 × 250 ms
// = 16 s), y no a un CICLO_MS de arrancar: no hay por qué esperar un minuto
// entero para ver el primer dato, y lo que se publica es la misma mediana que
// cualquier otra. De ahí en más, cada CICLO_MS. Si CICLO_MS es más corto (5 s
// en el banco), manda el más corto.
static const uint32_t PRIMERA_PUBLICACION_MS =
    MAX_DISPAROS_POR_VENTANA * PERIODO_DISPARO_MS < CICLO_MS
        ? MAX_DISPAROS_POR_VENTANA * PERIODO_DISPARO_MS
        : CICLO_MS;

// --- Topes de espera de la red ---
// Cuánto se espera a que el WiFi se asocie. Si no conecta, se sigue y el
// próximo ciclo reintenta: sin tope, un router apagado colgaría el loop.
static const uint32_t ESPERA_WIFI_MS = 15000;

// Topes de cada paso de la conexión al broker. Los defaults del core son 30 s
// para abrir la conexión TCP y 120 s para el handshake TLS, más 15 s de
// PubSubClient esperando la respuesta del broker: un solo intento trabado
// paraba el loop hasta 165 s, o sea dos publicaciones perdidas. Y como el reloj
// del ciclo se toma ANTES de conectar, el ciclo siguiente se disparaba enseguida
// con la ventana vacía y el log decía "sin eco (0/0)": una falla de sensor que
// no existía.
//
// Con estos, el peor caso de un ciclo que tiene que reconectar todo es WiFi 15 +
// DNS ~15 (lo fija el core, no se puede bajar) + TCP 5 + TLS 15 + MQTT 5 = 55 s,
// menos que CICLO_MS, así que la publicación siguiente sale a tiempo. TLS cuenta
// 15 y no 10 porque, abierta la conexión, cada lectura dentro del handshake
// puede esperar hasta TOPE_TCP_S, y el tope del handshake se revisa recién entre
// lectura y lectura. Un handshake sano tarda pocos segundos: 10 es margen de sobra.
//
// OJO CON LAS UNIDADES: los tres van en SEGUNDOS. setTimeout() de un Stream
// común es en milisegundos, pero WiFiClientSecure lo redefine en segundos: un
// 5000 acá serían 83 minutos.
static const uint32_t TOPE_TCP_S  = 5;
static const uint32_t TOPE_TLS_S  = 10;
static const uint16_t TOPE_MQTT_S = 5;

// Por debajo de esto el JSN-SR04T está en su zona ciega: no dice "sin eco",
// devuelve un número fijo cerca de 20 cm que parece una medición normal.
// constexpr (y no `static const`) para poder usarla en el static_assert de abajo.
constexpr float ZONA_CIEGA_CM = 25.0f;

// La calibración se revisa al COMPILAR: un error acá daría porcentajes
// absurdos con total confianza, y es más barato que no compile.
static_assert(DIST_FONDO_CM > DIST_LLENO_CM,
              "DIST_FONDO_CM tiene que ser mayor que DIST_LLENO_CM");
static_assert(DIST_LLENO_CM >= ZONA_CIEGA_CM,
              "DIST_LLENO_CM cae en la zona ciega del sensor: montarlo mas arriba");

// Qué se pudo decir de esta ventana. Viaja en el JSON como `estado`.
// `enum class` y no `enum` a secas: el core del C3 ya declara un `OK` global
// (en ets_sys.h, de la ROM), y un enum común choca con él al compilar.
enum class Estado { Ok, SinEco, ZonaCiega };

#if MQTT_TLS
// El certificado raíz con el que se valida al broker: ISRG Root X1, la raíz de
// Let's Encrypt. Es PÚBLICO (viene en cualquier navegador), por eso puede ir en
// el código. Vence en 2035.
//
// Verificado contra el broker el 09/10/2026: la cadena es *.s1.eu.hivemq.cloud
// -> YR1 -> Root YR, la jerarquía nueva de Let's Encrypt, y el broker manda Root
// YR firmada por ISRG Root X1. Esa firma cruzada es lo que hace que esta raíz
// alcance. Si Let's Encrypt la deja de mandar en alguna renovación, el TLS va a
// fallar y hay que sumar acá el PEM de Root YR, debajo de este.
//
// Sin esto el TLS cifra pero no verifica CON QUIÉN habla, y la contraseña del
// broker se le podría entregar a cualquiera que se haga pasar por él.
// `setInsecure()` haría eso mismo y por eso no se usa.
static const char CA_RAIZ[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";
static WiFiClientSecure red;
#else
// Sin TLS: solo para probar contra un broker en la red local.
static WiFiClient red;
#endif
static PubSubClient mqtt(red);

static uint32_t ultimoEnvio = 0;
static uint32_t ultimoDisparo = 0;

// Cuánto esperar desde ultimoEnvio hasta publicar: PRIMERA_PUBLICACION_MS la
// primera vez, CICLO_MS de ahí en más (loop() la cambia al publicar).
static uint32_t esperaEnvio = PRIMERA_PUBLICACION_MS;

// Los disparos CON ECO de la ventana actual, como anillo: el que llega cuando
// está lleno pisa al más viejo (ver MAX_DISPAROS_POR_VENTANA). Los contadores
// son de la ventana ENTERA y no de lo guardado, así el log dice la proporción
// real de ecos ("200/240") y no una que parece de sensor roto ("64/240").
// uint16_t porque con un ciclo largo pasan de 255. Se vacían en cada publicación.
static float ventana[MAX_DISPAROS_POR_VENTANA];
static uint16_t conEco = 0;
static uint16_t intentados = 0;

// Espera a que la PC abra el puerto USB. El SuperMini no tiene chip USB-serie:
// el puerto lo crea el propio C3 y existe recién cuando el host lo abre, así
// que sin esto se pierde el primer segundo de log. El tope de 3 s evita que,
// enchufada a una fuente sin PC, la placa espere para siempre.
static void esperarMonitorSerie() {
  uint32_t inicio = millis();
  while (!Serial && millis() - inicio < 3000) {
    delay(10);
  }
  delay(200);
}

// Un disparo: distancia en cm, o NAN si no hubo eco.
//
// POR QUÉ NAN Y NUNCA 0 NI -1: 0 cm de distancia al agua es tanque LLENO, así
// que un sensor sin eco se vería como la mejor noticia posible. Un -1 termina,
// tarde o temprano, sumado en un promedio. NAN no se confunde con una medición.
static float medirUnaVez() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(PULSO_TRIG_US);
  digitalWrite(TRIG_PIN, LOW);

  uint32_t idaYVueltaUs = pulseIn(ECHO_PIN, HIGH, TIMEOUT_ECO_US);
  if (idaYVueltaUs == 0) {
    return NAN;
  }
  float velocidadMps = 331.3f + 0.606f * TEMPERATURA_AIRE_C;
  return idaYVueltaUs * 1e-6f * velocidadMps / 2.0f * 100.0f;
}

// La lectura que se publica = la mediana de los disparos con eco de la
// ventana, o NAN si NINGUNO tuvo eco.
//
// Mediana y no promedio: un eco espurio (una pared, una gota en la sonda) mueve
// un promedio y no mueve una mediana. Con que un solo disparo tenga eco ya hay
// número: pedir mayoría convertiría un sensor que anda a medias en "sensor
// roto", y eso es esconder información. Que ande a medias se ve en el log.
//
// Ordena `v` en el lugar; el que llama la vacía después, así que no importa.
static float mediana(float* v, uint8_t n) {
  if (n == 0) return NAN;

  // Inserción: son pocos elementos, y se lee sin ir a la biblioteca estándar.
  for (uint8_t i = 1; i < n; i++) {
    float x = v[i];
    int8_t j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

// Qué se puede afirmar con esta distancia.
//
// LA ZONA CIEGA NO ES "LLENO". Con algo a menos de 25 cm el sensor devuelve
// ~20 cm, que convertido a porcentaje da 100%: la mejor noticia posible, salida
// de un artefacto del sensor. Como el nodo es quien calcula el porcentaje,
// ninguno de los que reciben el dato tiene cómo darse cuenta después. Por eso
// se corta acá: una lectura en la zona ciega no lleva nivel.
static Estado clasificar(float distancia) {
  if (isnan(distancia)) return Estado::SinEco;
  if (distancia < ZONA_CIEGA_CM) return Estado::ZonaCiega;
  return Estado::Ok;
}

// Nivel en % de ALTURA (0–100) a partir de la distancia y la calibración.
//
// Recortado a propósito en los dos extremos:
//   - más lejos que el fondo -> 0%: el agua no puede estar debajo del fondo,
//     así que es un tanque vacío más un eco que rebotó de más.
//   - más cerca que "lleno" (pero fuera de la zona ciega) -> 100%: un tanque
//     rebalsando o una calibración un poco corta. Un 104% no le dice nada más
//     útil a nadie.
//
// Es % de altura y no de volumen: coinciden solo si el tanque tiene la misma
// sección de arriba abajo (cilindro parado, prisma).
static float nivelPct(float distancia) {
  float pct = (DIST_FONDO_CM - distancia) / (DIST_FONDO_CM - DIST_LLENO_CM) * 100.0f;
  if (pct < 0.0f) return 0.0f;
  if (pct > 100.0f) return 100.0f;
  return pct;
}

// Conecta al WiFi si no lo está. Devuelve true si quedó conectado.
//
// Se llama en cada ciclo y no una vez en setup(): el WiFi se corta, y un nodo
// que solo se conecta al arrancar queda mudo hasta que alguien lo resetee.
static bool asegurarWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  // Salto de línea primero: loop() deja la línea de la medición abierta para
  // completarla con el resultado del envío.
  Serial.println();
  Serial.printf("WiFi: conectando a \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // PECULIARIDAD DEL C3 SUPERMINI: muchas unidades no logran asociarse al
  // router con la potencia de transmisión por defecto —el diseño de la antena
  // de la placa está mal adaptado y a potencia máxima distorsiona—. Bajarla a
  // 8,5 dBm es el arreglo conocido. Es un dato de foros y no está medido acá:
  // si conecta sin esto, se puede sacar y ganar alcance.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // Espera acotada (ver ESPERA_WIFI_MS).
  uint32_t inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < ESPERA_WIFI_MS) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: no conecto. Se reintenta el proximo ciclo.");
    return false;
  }
  Serial.printf("WiFi: conectado, IP %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // La hora NO viaja en el dato (la pone quien recibe), y hoy el TLS tampoco la
  // usa: el core instalado compila mbedTLS sin MBEDTLS_HAVE_TIME_DATE (se ve en
  // su sdkconfig del C3), así que no mira las fechas del certificado. Se sigue
  // pidiendo como seguro contra una actualización del core que lo prenda: con
  // el reloj en 1970 el certificado del broker parecería "todavía no válido" y
  // el nodo no conectaría nunca. Cuesta un paquete UDP y configTime() no
  // bloquea. Si eso llegara a pasar, fallaría el primer intento (el de setup,
  // antes de sincronizar) y el del ciclo siguiente ya tendría hora.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  return true;
}

// Conecta al broker si no lo está. Devuelve true si quedó conectado.
//
// Mismo criterio que asegurarWifi(): se reintenta en cada ciclo, una vez por
// minuto, que ya es un ritmo de reintento razonable y no martilla al broker.
static bool asegurarMqtt() {
  if (mqtt.connected()) return true;

  Serial.println();
  Serial.printf("MQTT: conectando a %s:%d como \"%s\"...\n", MQTT_HOST, MQTT_PORT, CLIENT_ID);
  if (mqtt.connect(CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println("MQTT: conectado.");
    return true;
  }

  // state() dice por qué falló. Los negativos son de la red (no llegó a hablar
  // MQTT); los positivos son el broker contestando que no.
  int estado = mqtt.state();
  const char* causa;
  switch (estado) {
    case MQTT_CONNECTION_TIMEOUT: causa = "el broker no contesto a tiempo"; break;
    case MQTT_CONNECT_FAILED:     causa = "no se pudo abrir la conexion (host, puerto o TLS)"; break;
    case MQTT_CONNECT_BAD_CREDENTIALS: causa = "usuario o contrasena rechazados"; break;
    case MQTT_CONNECT_UNAUTHORIZED:    causa = "el usuario no tiene permiso"; break;
    default: causa = "ver codigos de PubSubClient"; break;
  }
  Serial.printf("MQTT: no conecto (%d: %s). Se reintenta el proximo ciclo.\n", estado, causa);

#if MQTT_TLS
  // Si falló abriendo la conexión, lo más común es el certificado o la hora:
  // WiFiClientSecure guarda el último error de mbedTLS, que dice cuál.
  if (estado == MQTT_CONNECT_FAILED) {
    char detalle[100];
    red.lastError(detalle, sizeof(detalle));
    Serial.printf("      TLS: %s\n", detalle);
  }
#endif
  return false;
}

// Arma el mensaje y lo publica. El formato está documentado en el README ("El
// dato que publica el nodo"). Es la ÚNICA función que cambia entre el JSON y
// el texto plano por tópico.
//
// QoS 0 y sin retain, lo que pide UMA NET: el mensaje sale una vez y nadie
// confirma que llegó. Por eso publish() devolviendo true quiere decir "se
// escribió en la conexión", no "lo recibieron".
static void publicar(Estado estado, float distancia) {
  bool enviado;

  if (PUBLICAR_JSON) {
    // Tres formas según el estado. Las dos reglas que se juegan acá:
    //   - nivel_pct viaja SOLO si el estado es ok. Ni null ni 0.
    //   - distancia_cm viaja si hubo eco, aunque sea de la zona ciega: es la
    //     medición cruda, y es lo que permite ver qué pasó.
    char cuerpo[128];
    int rssi = WiFi.RSSI();
    if (estado == Estado::Ok) {
      snprintf(cuerpo, sizeof(cuerpo),
               "{\"estado\":\"ok\",\"nivel_pct\":%.1f,\"distancia_cm\":%.1f,\"rssi_dbm\":%d}",
               nivelPct(distancia), distancia, rssi);
    } else if (estado == Estado::ZonaCiega) {
      snprintf(cuerpo, sizeof(cuerpo),
               "{\"estado\":\"zona_ciega\",\"distancia_cm\":%.1f,\"rssi_dbm\":%d}",
               distancia, rssi);
    } else {
      snprintf(cuerpo, sizeof(cuerpo),
               "{\"estado\":\"sin_eco\",\"rssi_dbm\":%d}", rssi);
    }
    enviado = mqtt.publish(TOPICO_JSON, cuerpo, false);
    Serial.printf("  -> %s %s %s\n", TOPICO_JSON, cuerpo, enviado ? "" : "(NO SALIO)");
    return;
  }

  // Texto plano: un número por tópico, y no hay dónde decir "no sé". Así que
  // sin un nivel creíble NO SE PUBLICA NADA y queda un hueco, que es como el
  // contrato de UMA NET trata los cortes. El costo: del otro lado un sensor
  // roto se ve igual que un nodo apagado.
  //
  // No se agrega un tópico /estado para salvar eso: si el permiso del usuario
  // en el broker no lo incluye, HiveMQ corta la conexión al publicar ahí, y se
  // perdería también el nivel.
  if (estado != Estado::Ok) {
    Serial.println("  -> no se publica (sin nivel creible)");
    return;
  }
  char numero[8];
  snprintf(numero, sizeof(numero), "%.1f", nivelPct(distancia));
  enviado = mqtt.publish(TOPICO_NIVEL, numero, false);
  Serial.printf("  -> %s %s %s\n", TOPICO_NIVEL, numero, enviado ? "" : "(NO SALIO)");
}

void setup() {
  Serial.begin(115200);
  esperarMonitorSerie();

  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);

#if MQTT_TLS
  red.setCACert(CA_RAIZ);
  // Los topes de espera (ver TOPE_TCP_S). Los dos en SEGUNDOS. setTimeout()
  // vale para abrir la conexión y para cada lectura y escritura después.
  red.setTimeout(TOPE_TCP_S);
  red.setHandshakeTimeout(TOPE_TLS_S);
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(TOPE_MQTT_S);

  Serial.println();
  Serial.println("=== Nodo de nivel JSN-SR04T (MQTT) ===");
  Serial.printf("sitio \"%s\" -> %s:%d %s | primera publicacion a los %lu s, despues cada %lu s | calibracion fondo %.0f cm, lleno %.0f cm\n",
                SITIO, MQTT_HOST, MQTT_PORT, MQTT_TLS ? "TLS" : "SIN TLS",
                PRIMERA_PUBLICACION_MS / 1000, CICLO_MS / 1000, DIST_FONDO_CM, DIST_LLENO_CM);

  // Al broker se conecta YA, y no recién en la primera publicación: si el host,
  // la contraseña o el certificado están mal, el monitor lo dice a los pocos
  // segundos de enchufar. Si falla, no pasa nada más: cada publicación vuelve a
  // intentar.
  if (asegurarWifi()) {
    asegurarMqtt();
  }

  // La cuenta hasta la primera publicación arranca ACÁ y no en el boot: los
  // disparos empiezan recién en loop(), y conectar puede haber tardado decenas
  // de segundos. Contando desde el boot, la primera ventana podría salir vacía.
  ultimoEnvio = millis();
}

void loop() {
  // Dos relojes por millis() y ninguno con delay(): uno para disparar (cada
  // PERIODO_DISPARO_MS) y otro para publicar (cada CICLO_MS).

  // Mantiene viva la conexión con el broker: contesta y manda los PING del
  // keepalive. Hay que llamarlo seguido; si pasan más de ~15 s sin esto, el
  // broker da al nodo por muerto y lo desconecta.
  mqtt.loop();

  // --- Disparo ---
  if (millis() - ultimoDisparo >= PERIODO_DISPARO_MS) {
    ultimoDisparo = millis();
    float d = medirUnaVez();
    intentados++;
    if (isnan(d)) {
      if (IMPRIMIR_CADA_DISPARO) Serial.println("  sin eco");
    } else {
      // Anillo: la posición es el resto de dividir por el tope (el `%`), así
      // que pasado el tope se pisa el más viejo y nunca se escribe fuera del
      // arreglo.
      ventana[conEco % MAX_DISPAROS_POR_VENTANA] = d;
      conEco++;
      if (IMPRIMIR_CADA_DISPARO) {
        Serial.printf("  %6.1f cm%s\n", d, d < ZONA_CIEGA_CM ? "  (zona ciega: no confiable)" : "");
      }
    }
  }

  // --- Publicación: la primera a PRIMERA_PUBLICACION_MS, después cada CICLO_MS ---
  if (millis() - ultimoEnvio < esperaEnvio) {
    return;
  }
  ultimoEnvio = millis();
  esperaEnvio = CICLO_MS;

  // Lo guardado es como mucho el tope del anillo, aunque hayan llegado más.
  uint8_t guardados = conEco < MAX_DISPAROS_POR_VENTANA ? conEco : MAX_DISPAROS_POR_VENTANA;
  uint16_t ecos = conEco, total = intentados;
  float distancia = mediana(ventana, guardados);
  conEco = 0;
  intentados = 0;
  Estado estado = clasificar(distancia);

  if (estado == Estado::SinEco) {
    Serial.printf("== sin eco (0/%u)", (unsigned)total);
  } else if (estado == Estado::ZonaCiega) {
    Serial.printf("== mediana %.1f cm: ZONA CIEGA, sin nivel (%u/%u con eco)", distancia,
                  (unsigned)ecos, (unsigned)total);
  } else {
    Serial.printf("== mediana %.1f cm = %.1f%% (%u/%u con eco)", distancia, nivelPct(distancia),
                  (unsigned)ecos, (unsigned)total);
  }

  // Se mide aunque no haya red, así el log sigue mostrando el sensor mientras
  // se pelea con el WiFi o el broker. Lo que no se puede mandar se pierde: no
  // hay buffer, y el hueco queda como hueco.
  if (!asegurarWifi()) {
    Serial.println("  (sin WiFi, no se envia)");
    return;
  }
  if (!asegurarMqtt()) {
    Serial.println("  (sin broker, no se envia)");
    return;
  }
  publicar(estado, distancia);
}
