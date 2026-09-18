// ProofBox — módulo Cámara
// Placa: Seeed XIAO ESP32S3 Sense (sensor OV3660) + antena externa U.FL.
//
// Dos trabajos:
//  1. EN VIVO: mientras está enchufada publica fotogramas JPEG por MQTT
//     (proofboxcam/<id>/live), mire alguien o no — así la app lo enseña en
//     cuanto se abre, sin esperar a que la cámara arranque. Con ALWAYS_LIVE en
//     false vuelve al modo a demanda (latidos en .../viewer), que calienta menos.
//  2. ARCHIVO: cada SHOT_EVERY_MS una foto grande a Storage, con la hora en el
//     nombre (cam/<id>/shots/<epoch>.jpg), y la misma como latest.jpg.
//
// Por qué MQTT y no un servidor web en la placa: la app va por https y el
// navegador bloquea un http:// de la red local; además así se ve fuera de casa.
// El broker ya es el que usa la app.
//
// Compilar con PSRAM=opi.

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <time.h>
#include <Preferences.h>
#include "esp_camera.h"

// ── Destino ──────────────────────────────────────────────────────────────────
// La clave publicable es la misma que ya va en el HTML público de la app.
const char* SUPA_HOST = "blqcppmjejtnvoqlhshp.supabase.co";
const char* SUPA_KEY  = "sb_publishable_7AiXScdSAxipUu_Mi8Fulg_jFCZoKYJ";
const char* BUCKET    = "proofbox-photos";
const char* CAM_ID    = "proofbox-cam01";

#define MQTT_SERVER "broker.hivemq.com"
#define MQTT_PORT   1883
const char* TOPIC_LIVE   = "proofboxcam/proofbox-cam01/live";
const char* TOPIC_VIEWER = "proofboxcam/proofbox-cam01/viewer";
const char* TOPIC_STATE  = "proofboxcam/proofbox-cam01/state";
const char* TOPIC_CMD    = "proofboxcam/proofbox-cam01/cmd";
const char* TOPIC_FLIP   = "proofboxcam/proofbox-cam01/flip";
const char* TOPIC_QUAL   = "proofboxcam/proofbox-cam01/quality";

const unsigned long SHOT_EVERY_MS  = 10UL * 60UL * 1000UL;  // archivo: cada 10 min
const unsigned long VIEWER_TTL_MS  = 15000;                 // sin latido en 15 s, se corta el vivo
unsigned long FRAME_MS             = 150;                   // lo fija la calidad elegida

// Tamaños. La cámara se inicia al MAYOR que se va a usar: el búfer se reserva
// para ese tamaño y agrandar después en caliente cortaría las fotos.
const framesize_t SIZE_SHOT = FRAMESIZE_XGA;   // 1024x768, ~80-120 KB a calidad 10
const int         QUAL_SHOT = 10;
// Calidad del vivo, elegible desde la app. Más píxeles y menos compresión
// cuestan fotogramas: lo que limita no es el sensor sino subir cada JPEG por
// WiFi al broker. Se guarda en NVS.
struct LiveQ { framesize_t size; int quality; unsigned long frameMs; const char* name; };
const LiveQ LIVE_Q[3] = {
  { FRAMESIZE_HVGA, 20, 150, "fluid"    },   // 480x320, ~5 KB, ~6 fps
  { FRAMESIZE_SVGA, 12, 300, "balanced" },   // 800x600, ~25 KB, ~3 fps
  { FRAMESIZE_XGA,  10, 600, "sharp"    },   // 1024x768, ~45 KB, ~1-2 fps
};
int liveQ = 0;
framesize_t SIZE_LIVE = FRAMESIZE_HVGA;
int         QUAL_LIVE = 20;
void setLiveQ(int q) {
  liveQ = constrain(q, 0, 2);
  SIZE_LIVE = LIVE_Q[liveQ].size; QUAL_LIVE = LIVE_Q[liveQ].quality; FRAME_MS = LIVE_Q[liveQ].frameMs;
}

// ── Pines de la cámara del XIAO ESP32S3 Sense (según Seeed) ─────────────────
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13
#define LED_PIN         21      // LED de usuario, activo en BAJO

WiFiClient   mqttNet;
PubSubClient mqtt(mqttNet);

// Dar la vuelta a la imagen se hace en el SENSOR, no en la pantalla: así sale
// derecha también en las fotos que se guardan, no solo en el vivo. Y se recuerda
// en NVS, que es lo que hace falta si la cámara se queda colgada boca abajo.
Preferences prefs;
bool vflip = false, hmirror = false;

bool camOn = false;
unsigned long lastViewerMs = 0;
unsigned long lastFrameMs = 0;
unsigned long lastShotMs = 0;
bool firstShotDone = false;
unsigned long framesSent = 0;

// Siempre en vivo, por decisión del usuario: el vídeo tiene que estar ahí al
// abrir la app. Cuesta calor (cámara y radio sin descanso) — lleva disipador.
const bool ALWAYS_LIVE = true;
bool viewerPresent() { return ALWAYS_LIVE || (lastViewerMs && millis() - lastViewerMs < VIEWER_TTL_MS); }

bool camStart() {
  if (camOn) return true;
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode    = CAMERA_GRAB_LATEST;   // siempre el fotograma más reciente
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.frame_size   = SIZE_SHOT;
  c.jpeg_quality = QUAL_SHOT;
  c.fb_count     = 2;
  if (!psramFound()) {
    Serial.println("⚠️ Sin PSRAM: compilar con PSRAM=opi.");
    return false;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("❌ esp_camera_init 0x%x — ¿está bien encajada la placa de la cámara?\n", err);
    return false;
  }
  camOn = true;
  tuneSensor();
  applyFlip();
  // Las primeras capturas salen verdosas mientras ajusta la exposición.
  for (int i = 0; i < 3; i++) { camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); delay(150); }
  return true;
}

// Ajustes del sensor para sacar más detalle sin cambiar de cámara: corrección
// de lente (el centro no se lleva todo el brillo), corrección de píxeles
// muertos, gamma, y exposición automática con el modo de poca luz. Ganancia
// limitada: subirla aclara pero llena la imagen de grano, y la masa no se mueve,
// así que es mejor exponer más tiempo.
void tuneSensor() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_lenc(s, 1);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_dcw(s, 1);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_gainceiling(s, GAINCEILING_8X);
  s->set_sharpness(s, 2);
  s->set_denoise(s, 1);
}

void applyFlip() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_vflip(s, vflip ? 1 : 0);
  s->set_hmirror(s, hmirror ? 1 : 0);
}

void publishQuality() {
  mqtt.publish(TOPIC_QUAL, LIVE_Q[liveQ].name, true);
}

void publishFlip() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d,%d", vflip ? 1 : 0, hmirror ? 1 : 0);
  mqtt.publish(TOPIC_FLIP, buf, true);
}

void camStop() {
  if (!camOn) return;
  esp_camera_deinit();
  camOn = false;
}

void camMode(framesize_t size, int quality) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_framesize(s, size);
  s->set_quality(s, quality);
  // Tras cambiar de tamaño los fotogramas ya en cola son del tamaño viejo: con
  // dos búferes hay que tirar al menos dos.
  for (int i = 0; i < 3; i++) { camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); }
}

// ── Storage ──────────────────────────────────────────────────────────────────
bool uploadJpeg(const String& path, const uint8_t* buf, size_t len, bool upsert) {
  WiFiClientSecure tls;
  // Sin verificar el certificado: la foto no es secreta y fijar la CA obliga a
  // regrabar cuando Supabase la rota.
  tls.setInsecure();
  HTTPClient http;
  String url = String("https://") + SUPA_HOST + "/storage/v1/object/" + BUCKET + "/" + path;
  if (!http.begin(tls, url)) return false;
  http.setTimeout(20000);
  http.addHeader("apikey", SUPA_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPA_KEY);
  http.addHeader("Content-Type", "image/jpeg");
  if (upsert) http.addHeader("x-upsert", "true");
  http.addHeader("cache-control", "no-cache");
  int code = http.POST((uint8_t*)buf, len);
  if (code != 200) Serial.printf("⚠️ %s → HTTP %d: %s\n", path.c_str(), code, http.getString().c_str());
  http.end();
  return code == 200;
}

// Foto de archivo: grande, con la hora en el nombre. Si el vivo estaba en
// marcha, se vuelve a él al terminar.
void archiveShot() {
  bool wasLive = camOn;
  // Reinicio limpio a tamaño grande. Cambiar de 480x320 a 1024x768 en caliente
  // con el vivo corriendo daba fotos de 480x320 llenas de bandas: el sensor
  // entregaba un fotograma a medio cambiar. Todas las de archivo salían rotas y
  // Claude no encontraba el frasco en ellas. Reiniciar cuesta ~1 s cada 10 min.
  camStop();
  if (!camStart()) return;   // camStart arranca en SIZE_SHOT y tira 3 fotogramas
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb && fb->width < 1000) {
    Serial.printf("⚠️ foto de archivo de %ux%u, se descarta\n", fb->width, fb->height);
    esp_camera_fb_return(fb); fb = nullptr;
  }
  if (fb) {
    digitalWrite(LED_PIN, LOW);
    time_t now = time(nullptr);
    bool ok1 = false;
    // Sin hora (NTP aún no respondió) no se archiva: un nombre 1970 rompería el orden.
    if (now > 1700000000) {
      ok1 = uploadJpeg(String("cam/") + CAM_ID + "/shots/" + String((long long)now) + ".jpg", fb->buf, fb->len, false);
    }
    bool ok2 = uploadJpeg(String("cam/") + CAM_ID + "/latest.jpg", fb->buf, fb->len, true);
    Serial.printf("🗂️ archivo %u KB  shots:%s latest:%s\n", (unsigned)(fb->len / 1024),
                  now > 1700000000 ? (ok1 ? "ok" : "fallo") : "sin hora", ok2 ? "ok" : "fallo");
    esp_camera_fb_return(fb);
    digitalWrite(LED_PIN, HIGH);
  }
  if (wasLive) camMode(SIZE_LIVE, QUAL_LIVE);
  else camStop();
}

// ── Vivo ─────────────────────────────────────────────────────────────────────
void sendFrame() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;
  // beginPublish escribe el JPEG directamente al socket: no hace falta un
  // búfer de MQTT del tamaño del fotograma.
  if (mqtt.beginPublish(TOPIC_LIVE, fb->len, false)) {
    mqtt.write(fb->buf, fb->len);
    if (mqtt.endPublish()) framesSent++;
  }
  esp_camera_fb_return(fb);
}

void publishState(const char* s) {
  mqtt.publish(TOPIC_STATE, s, true);
}

volatile bool shotAsked = false;

void onMqtt(char* topic, byte* payload, unsigned int len) {
  // Disparo a mano desde la app. Se apunta y se hace en el loop: sacar la foto
  // y subirla aquí dentro bloquearía el cliente MQTT varios segundos.
  if (strcmp(topic, TOPIC_CMD) == 0) {
    String c; for (unsigned int i = 0; i < len; i++) c += (char)payload[i];
    if (c == "shot") { shotAsked = true; Serial.println("📸 foto pedida desde la app"); return; }
    // "flip" / "mirror" alternan; "flip:1" / "mirror:0" fijan.
    bool changed = false;
    if (c == "flip")        { vflip = !vflip; changed = true; }
    else if (c == "mirror") { hmirror = !hmirror; changed = true; }
    else if (c.startsWith("flip:"))   { vflip = c.endsWith("1"); changed = true; }
    else if (c.startsWith("mirror:")) { hmirror = c.endsWith("1"); changed = true; }
    else if (c.startsWith("q:")) {
      setLiveQ(c.substring(2).toInt());
      prefs.putInt("liveq", liveQ);
      if (camOn) camMode(SIZE_LIVE, QUAL_LIVE);
      publishQuality();
      Serial.printf("🎚️ calidad del vivo: %s\n", LIVE_Q[liveQ].name);
      return;
    }
    if (changed) {
      prefs.putBool("vflip", vflip);
      prefs.putBool("hmirror", hmirror);
      if (camOn) applyFlip();
      publishFlip();
      Serial.printf("🔄 vflip=%d hmirror=%d\n", vflip, hmirror);
    }
    return;
  }
  if (strcmp(topic, TOPIC_VIEWER) == 0) {
    if (!ALWAYS_LIVE && !viewerPresent()) Serial.println("👀 alguien mira: vivo encendido");
    lastViewerMs = millis();
  }
}

void mqttEnsure() {
  if (mqtt.connected()) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 3000) return;
  lastTry = millis();
  String cid = String("pbcam-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  // Última voluntad: si la placa se cae, la app ve "offline" en vez de esperar.
  if (mqtt.connect(cid.c_str(), nullptr, nullptr, TOPIC_STATE, 0, true, "offline")) {
    mqtt.subscribe(TOPIC_VIEWER);
    mqtt.subscribe(TOPIC_CMD);
    publishState("idle");
    publishFlip();
    publishQuality();
    Serial.println("MQTT ✅");
  } else {
    Serial.printf("MQTT ❌ rc=%d\n", mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ProofBox Cam (vivo + archivo) ===");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  prefs.begin("pbcam", false);
  vflip   = prefs.getBool("vflip", false);
  hmirror = prefs.getBool("hmirror", false);
  setLiveQ(prefs.getInt("liveq", 0));
  Serial.printf("🔄 vflip=%d hmirror=%d\n", vflip, hmirror);

  if (camStart()) {
    sensor_t* s = esp_camera_sensor_get();
    if (s) Serial.printf("📷 Sensor PID 0x%x\n", s->id.PID);
    camStop();
  }

  WiFi.mode(WIFI_STA);
  // Qué redes se oyen y con qué fuerza: sin la antena externa esta placa apenas
  // oye nada, y aquí se ve en un segundo.
  int n = WiFi.scanNetworks();
  Serial.printf("📡 %d redes:\n", n);
  for (int i = 0; i < n; i++)
    Serial.printf("   %-28s %4d dBm  canal %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  WiFi.scanDelete();

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  // Por defecto oculta las redes con menos de un 8% de señal.
  wm.setMinimumSignalQuality(0);
  wm.setRemoveDuplicateAPs(true);
  wm.setConnectTimeout(30);
  wm.setConnectRetries(3);
  if (!wm.autoConnect("ProofBox-Cam")) ESP.restart();
  Serial.println("📶 " + WiFi.SSID() + "  " + WiFi.localIP().toString());

  configTime(0, 0, "pool.ntp.org", "time.google.com");

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setKeepAlive(20);
  mqtt.setBufferSize(512);   // los fotogramas no pasan por aquí (beginPublish)
  mqtt.setSocketTimeout(10);   // un fotograma nítido de ~45 KB tarda más en salir
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    camStop();
    Serial.println("📶 WiFi caída, reconectando…");
    WiFi.reconnect();
    delay(3000);
    return;
  }
  mqttEnsure();
  mqtt.loop();

  // Foto a mano: manda la orden por delante del reloj de los 10 min.
  if (shotAsked) {
    shotAsked = false;
    archiveShot();
    mqtt.publish(TOPIC_STATE, camOn ? "live" : "idle", true);
  }

  // Archivo: la primera en cuanto hay hora (o a los 30 s si NTP no responde),
  // luego cada 10 min.
  bool timeOk = time(nullptr) > 1700000000;
  if (!firstShotDone) {
    if (timeOk || millis() > 30000) { archiveShot(); lastShotMs = millis(); firstShotDone = true; }
  } else if (millis() - lastShotMs >= SHOT_EVERY_MS) {
    lastShotMs = millis();
    archiveShot();
  }

  if (viewerPresent()) {
    if (!camOn) {
      if (camStart()) { camMode(SIZE_LIVE, QUAL_LIVE); WiFi.setSleep(false); publishState("live"); }
    }
    if (camOn && millis() - lastFrameMs >= FRAME_MS) {
      lastFrameMs = millis();
      sendFrame();
    }
  } else if (camOn) {
    // Nadie mira: cámara apagada y radio a dormir, que es lo que evita el calor.
    Serial.printf("😴 sin espectadores, vivo apagado (%lu fotogramas)\n", framesSent);
    camStop();
    WiFi.setSleep(true);
    publishState("idle");
  }
  delay(5);
}
