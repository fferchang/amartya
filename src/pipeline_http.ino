// Nodo de nivel de cisterna: mide la distancia al agua con un JSN-SR04T y la
// publica por HTTP, como JSON, al servidor de API_URL. El formato del dato y
// cómo viaja están en el README; qué es y qué NO es todavía (un prototipo: sin
// buffer y publicando cada 5 segundos), en platformio.ini.
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
#include <HTTPClient.h>
#include <time.h>

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
// 0 a 30 °C cambia 5,5%, ~8 cm sobre 150 cm. Sin sensor de clima se supone
// fija; con uno, compensar sale gratis.
static const float TEMPERATURA_AIRE_C = 20.0f;

// Cada cuánto se dispara el sensor. 250 ms da ~20 disparos por ventana de 5 s,
// los mismos 20 que el banco usaba para medir el ruido: de sobra para una
// mediana que ignore ecos espurios.
//
// El piso es 100 ms y no se baja más: el datasheet pide ≥ 50 ms entre disparos
// para que el eco del anterior —que sigue rebotando en las paredes— no se tome
// como el de este, y en un tanque cerrado rebota más que en una habitación.
// Con 100 ms e IMPRIMIR_CADA_DISPARO en true es el modo "mover la mano y ver
// el número seguirla", que ya se probó y anda.
static const uint32_t PERIODO_DISPARO_MS = 250;

// Imprimir cada disparo en el monitor serie, o solo la línea de cada
// publicación. Apagado: a 4 disparos por segundo el log de envíos queda
// enterrado entre renglones de distancia. Prenderlo para mirar el sensor en
// vivo (conviene bajar PERIODO_DISPARO_MS a 100 para eso).
static const bool IMPRIMIR_CADA_DISPARO = false;

// Cada cuánto se PUBLICA la mediana de la ventana. Los 5 s de hoy son un ritmo
// de PRUEBA: sirven para ver moverse el dato mientras uno le acerca la mano al
// sensor. En operación alcanza con mucho menos —1 por minuto o menos—, porque
// el nivel de una cisterna no cambia en segundos. Con 5 s son 12 POST por
// minuto y el servidor tiene que aceptar ese ritmo; si no lo acepta y contesta
// 429, el nodo espera lo que le pida (ver publicar()).
static const uint32_t CICLO_MS = 5000;

// Cuántos disparos CON ECO se guardan para la mediana. Con los valores de hoy
// la ventana tiene ~20 (5000/250) y entran todos. Si tiene más —un CICLO_MS
// largo, o un período más corto— se quedan los ÚLTIMOS 64: lo que se publica
// es "el nivel ahora", y la parte más reciente de la ventana es la que mejor lo
// dice. Con CICLO_MS en 60 s, por ejemplo, la mediana es de los últimos 16 s.
static const uint8_t MAX_DISPAROS_POR_VENTANA = 64;

// mediana() recorre el arreglo con un índice int8_t, que llega hasta 127. Si
// alguien sube el tope por encima de eso, que falle al compilar y no en silencio.
static_assert(MAX_DISPAROS_POR_VENTANA <= 127, "mediana() usa int8_t como indice");

// Por debajo de esto el JSN-SR04T está en su zona ciega: no dice "sin eco",
// devuelve un número fijo cerca de 20 cm que parece una medición normal. El
// nodo lo marca en el log y lo manda igual: es quien lo muestre el que tiene
// que tratarlo como dudoso y no como "tanque lleno". Por eso, al montarlo, el
// agua nunca tiene que quedar a menos de esto del sensor.
static const float ZONA_CIEGA_CM = 25.0f;

// Cualquier epoch anterior al 1/1/2020 es un reloj que todavía no sincronizó
// por NTP, no una fecha. Es el corte que usa el servidor de Amartya, y conviene
// que cualquier receptor use el mismo (ver horaActual()).
static const time_t EPOCH_MINIMO = 1577836800;

static uint32_t ultimoEnvio = 0;
static uint32_t ultimoDisparo = 0;
static uint32_t esperaExtraMs = 0;  // la que pide un 429 con Retry-After

// Los disparos CON ECO de la ventana actual, como anillo: el que llega cuando
// está lleno pisa al más viejo (ver MAX_DISPAROS_POR_VENTANA). Los contadores
// son de la ventana ENTERA y no de lo guardado, así el log dice la proporción
// real de ecos ("200/240") y no una que parece de sensor roto ("64/240").
// uint16_t porque con un ciclo largo pasan de 255: 60 s a 100 ms son 600.
// Se vacían en cada publicación.
static float ventana[MAX_DISPAROS_POR_VENTANA];
static uint16_t conEco = 0;
static uint16_t intentados = 0;

// Espera a que la PC abra el puerto USB. El SuperMini no tiene chip USB-serie:
// el puerto lo crea el propio C3 y existe recién cuando el host lo abre, así
// que sin esto se pierde el primer segundo de log. El tope de 3 s evita que,
// enchufada a un cargador sin PC, la placa espere para siempre.
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
// que un sensor sin eco se vería como la mejor noticia posible (la misma
// familia que el bug de `Number(null) === 0` de la interfaz). Un -1 termina,
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
// Con que uno solo tenga eco ya hay número. Es a propósito: pedir mayoría
// convertiría un sensor que anda a medias en "sensor roto", y eso es esconder
// información. Que ande a medias se ve en el log serie.
//
// Ordena `v` en el lugar; el que llama la vacía después, así que no importa.
static float mediana(float* v, uint8_t n) {
  if (n == 0) return NAN;

  // Inserción: son ~20 elementos, y se lee sin ir a la biblioteca estándar.
  for (uint8_t i = 1; i < n; i++) {
    float x = v[i];
    int8_t j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
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

  // Espera acotada: si no conecta en 15 s se sigue, y el próximo ciclo
  // reintenta. Sin tope, un router apagado colgaría el loop para siempre.
  uint32_t inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: no conecto. Se reintenta el proximo ciclo.");
    return false;
  }
  Serial.printf("WiFi: conectado, IP %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // NTP arranca apenas hay red. configTime() no bloquea: sincroniza en segundo
  // plano, y cada envío mira si ya hay hora (ver horaActual()).
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  return true;
}

// El epoch actual, o el reloj crudo si NTP todavía no sincronizó.
//
// Si no hay hora se manda igual, con un número chico (los segundos desde el
// arranque). Pasa en los primeros segundos después de un reinicio, antes de
// que NTP conteste. El receptor tiene que reconocerlo como implausible (menor
// que EPOCH_MINIMO) y fechar la lectura con la hora de llegada. Tirarla sería
// perder una medición buena por culpa de un reloj.
static time_t horaActual(bool* sincronizada) {
  time_t ahora = time(nullptr);
  *sincronizada = ahora > EPOCH_MINIMO;
  return ahora;
}

// Arma el JSON y lo manda. El formato está documentado en el README ("El dato
// que publica el nodo"); las dos reglas que se juegan acá:
//   - sin eco: `valid: false` y distance_cm AUSENTE. Ni null ni 0.
//   - temperatura y humedad ausentes: este nodo no tiene sensor de clima, y
//     una magnitud que no se mide es una clave ausente.
static void publicar(float distancia, time_t ts, bool sincronizada) {
  char cuerpo[160];
  if (isnan(distancia)) {
    snprintf(cuerpo, sizeof(cuerpo),
             "{\"node_id\":\"%s\",\"timestamp_unix\":%ld,\"valid\":false,\"rssi_dbm\":%d}",
             NODE_ID, (long)ts, WiFi.RSSI());
  } else {
    snprintf(cuerpo, sizeof(cuerpo),
             "{\"node_id\":\"%s\",\"timestamp_unix\":%ld,\"valid\":true,"
             "\"distance_cm\":%.1f,\"rssi_dbm\":%d}",
             NODE_ID, (long)ts, distancia, WiFi.RSSI());
  }

  HTTPClient http;
  http.begin(API_URL);
  http.setTimeout(1500);  // bastante menos que el ciclo: mientras espera no se dispara
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " API_TOKEN);

  // Hay que pedir de antemano los headers de respuesta que se quieren leer:
  // HTTPClient descarta todos los que no se nombren acá.
  const char* headersQueInteresan[] = {"Retry-After"};
  http.collectHeaders(headersQueInteresan, 1);

  int codigo = http.POST(cuerpo);

  // Cada respuesta posible dice algo distinto y se imprime distinto: el log
  // serie es la única ventana al nodo mientras se lo pone a andar.
  if (codigo == 201) {
    Serial.printf("  -> 201 aceptada%s\n", sincronizada ? "" : " (sin hora NTP: la fecha el servidor)");
  } else if (codigo == 429) {
    // Respetar Retry-After en vez de reintentar a ciegas: si no, el nodo es
    // exactamente el bucle que el límite del servidor existe para frenar. Sin
    // el header se esperan 10 s, que es un "algo" razonable y no un minuto.
    int segundos = http.header("Retry-After").toInt();
    esperaExtraMs = (segundos > 0 ? segundos : 10) * 1000UL;
    Serial.printf("  -> 429 el servidor pide bajar el ritmo: espero %d s de mas.\n",
                  segundos > 0 ? segundos : 10);
  } else if (codigo == 401 || codigo == 403) {
    Serial.printf("  -> %d token rechazado: API_TOKEN no es el que espera el servidor.\n", codigo);
  } else if (codigo == 400) {
    Serial.printf("  -> 400 el servidor rechazo el cuerpo: %s\n", http.getString().c_str());
  } else if (codigo < 0) {
    // Códigos negativos = no hubo respuesta HTTP (no conectó, timeout). Casi
    // siempre es la dirección de API_URL, el servidor apagado o un firewall.
    Serial.printf("  -> sin respuesta (%s). Revisar API_URL, que el servidor corra y el firewall.\n",
                  http.errorToString(codigo).c_str());
  } else {
    Serial.printf("  -> %d inesperado: %s\n", codigo, http.getString().c_str());
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  esperarMonitorSerie();

  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);

  Serial.println();
  Serial.println("=== Nodo de nivel JSN-SR04T ===");
  Serial.printf("nodo \"%s\" -> %s | dispara cada %lu ms, publica cada %lu ms\n",
                NODE_ID, API_URL, PERIODO_DISPARO_MS, CICLO_MS);
  asegurarWifi();
}

void loop() {
  // Dos relojes por millis() y ninguno con delay(): uno para disparar (cada
  // PERIODO_DISPARO_MS) y otro para publicar (cada CICLO_MS). Con delay() el
  // tiempo que tarda el POST se sumaría al período y los disparos se atrasarían.

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

  // --- Publicación: cada CICLO_MS, más lo que haya pedido un 429 ---
  if (millis() - ultimoEnvio < CICLO_MS + esperaExtraMs) {
    return;
  }
  ultimoEnvio = millis();
  esperaExtraMs = 0;

  // Lo guardado es como mucho el tope del anillo, aunque hayan llegado más.
  uint8_t guardados = conEco < MAX_DISPAROS_POR_VENTANA ? conEco : MAX_DISPAROS_POR_VENTANA;
  uint16_t ecos = conEco, total = intentados;
  float distancia = mediana(ventana, guardados);
  conEco = 0;
  intentados = 0;

  if (isnan(distancia)) {
    Serial.printf("== publica: sin eco (0/%u) -> valid:false", (unsigned)total);
  } else {
    Serial.printf("== publica: mediana %.1f cm (%u/%u con eco)%s", distancia,
                  (unsigned)ecos, (unsigned)total,
                  distancia < ZONA_CIEGA_CM ? " ZONA CIEGA" : "");
  }

  // Se mide aunque no haya WiFi, así el log sigue mostrando el sensor mientras
  // se pelea con la red. Lo que no se puede mandar se pierde: no hay buffer.
  if (!asegurarWifi()) {
    Serial.println("  (sin WiFi, no se envia)");
    return;
  }
  bool sincronizada;
  time_t ts = horaActual(&sincronizada);
  publicar(distancia, ts, sincronizada);
}
