// ProofBox — módulo Cámara
// Placa: Seeed XIAO ESP32S3 Sense (OV2640/OV3660 en la placa de expansión).
//
// Hace una foto cada CAPTURE_MS y la sube a Supabase Storage como
// cam/<CAM_ID>/latest.jpg. La app la lee de ahí.
//
// Por qué Storage y no un servidor web en la placa: la app vive en GitHub Pages
// (https) y el navegador bloquea cargar imágenes de un http:// de la red local.
// Además así la foto se ve fuera de casa, desde el móvil con datos.
//
// Compilar con PSRAM=opi: sin PSRAM la cámara no tiene dónde guardar un
// fotograma de más de 320x240 y esp_camera_init falla con "fb alloc".

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// ── Destino ──────────────────────────────────────────────────────────────────
// La clave publicable es la misma que ya va en el HTML público de la app.
const char* SUPA_HOST = "blqcppmjejtnvoqlhshp.supabase.co";
const char* SUPA_KEY  = "sb_publishable_7AiXScdSAxipUu_Mi8Fulg_jFCZoKYJ";
const char* BUCKET    = "proofbox-photos";
const char* CAM_ID    = "proofbox-cam01";

const unsigned long CAPTURE_MS = 10000;   // una foto cada 10 s

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

bool camOk = false;
unsigned long lastShot = 0;
unsigned long okCount = 0, failCount = 0;

bool initCamera() {
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
  c.grab_mode    = CAMERA_GRAB_LATEST;   // la más reciente, no una que llevaba segundos en cola
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  // 800x600 con calidad 12: ~40-80 KB, suficiente para ver la cúpula y el
  // alveolado a través del bote sin que cada subida tarde.
  c.frame_size   = FRAMESIZE_SVGA;
  c.jpeg_quality = 12;
  c.fb_count     = 1;

  if (!psramFound()) {
    Serial.println("⚠️ Sin PSRAM: compilar con PSRAM=opi. Bajo a 320x240.");
    c.frame_size = FRAMESIZE_QVGA;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("❌ esp_camera_init 0x%x — ¿está bien encajada la placa de la cámara?\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    Serial.printf("📷 Sensor PID 0x%x\n", s->id.PID);
  }
  // Las primeras capturas salen verdosas mientras el sensor ajusta exposición.
  for (int i = 0; i < 3; i++) { camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); delay(200); }
  return true;
}

// Sube el JPEG con upsert: siempre el mismo nombre, así la app no tiene que
// buscar cuál es la última.
bool uploadJpeg(const uint8_t* buf, size_t len) {
  WiFiClientSecure tls;
  // Sin verificar el certificado: la foto no es secreta y fijar la CA de
  // Supabase obliga a regrabar cuando la rotan. Si un día importa, se fija aquí.
  tls.setInsecure();
  HTTPClient http;
  String url = String("https://") + SUPA_HOST + "/storage/v1/object/" + BUCKET + "/cam/" + CAM_ID + "/latest.jpg";
  if (!http.begin(tls, url)) return false;
  http.setTimeout(15000);
  http.addHeader("apikey", SUPA_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPA_KEY);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("x-upsert", "true");
  // Sin caché en la CDN: si no, el navegador vería la misma foto un rato.
  http.addHeader("cache-control", "no-cache");
  int code = http.POST((uint8_t*)buf, len);
  if (code != 200) {
    Serial.printf("⚠️ Subida HTTP %d: %s\n", code, http.getString().c_str());
  }
  http.end();
  return code == 200;
}

// La cámara se enciende solo para la foto. Encendida sin parar, el sensor
// captura fotogramas continuamente aunque nadie los use, y la placa se pone
// caliente al tacto; con una foto cada 10 s pasa casi todo el tiempo apagada.
void shoot() {
  if (!initCamera()) { failCount++; return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("⚠️ Sin fotograma"); failCount++; esp_camera_deinit(); return; }
  digitalWrite(LED_PIN, LOW);
  unsigned long t = millis();
  bool ok = uploadJpeg(fb->buf, fb->len);
  digitalWrite(LED_PIN, HIGH);
  Serial.printf("%s %u KB en %lu ms (ok %lu / fallos %lu)\n",
                ok ? "✅" : "❌", (unsigned)(fb->len / 1024), millis() - t,
                ok ? ++okCount : okCount, ok ? failCount : ++failCount);
  esp_camera_fb_return(fb);
  esp_camera_deinit();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ProofBox Cam ===");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  camOk = initCamera();
  if (camOk) esp_camera_deinit();

  // La primera vez abre la red "ProofBox-Cam": conectarse desde el móvil y
  // elegir la WiFi. Después recuerda la red sola.
  WiFi.mode(WIFI_STA);
  // Antes de nada, qué redes se oyen y con qué fuerza. Sin la antena externa
  // enchufada esta placa apenas oye nada, y eso se ve aquí en un segundo.
  int n = WiFi.scanNetworks();
  Serial.printf("📡 %d redes:\n", n);
  for (int i = 0; i < n; i++)
    Serial.printf("   %-28s %4d dBm  canal %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  WiFi.scanDelete();

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  // Por defecto oculta las redes con menos de un 8% de señal: con antena floja
  // la red de casa no aparecía en la lista y parecía que el portal no funcionaba.
  wm.setMinimumSignalQuality(0);
  wm.setRemoveDuplicateAPs(true);
  wm.setConnectTimeout(30);
  wm.setConnectRetries(3);
  if (!wm.autoConnect("ProofBox-Cam")) ESP.restart();
  // Con ahorro de energía la subida tarda algo más, pero entre foto y foto la
  // radio descansa y la placa se calienta bastante menos. A 10 s sobra tiempo.
  WiFi.setSleep(true);
  Serial.println("📶 " + WiFi.SSID() + "  " + WiFi.localIP().toString());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("📶 WiFi caída, reconectando…");
    WiFi.reconnect();
    delay(3000);
    return;
  }
  if (lastShot == 0 || millis() - lastShot >= CAPTURE_MS) {
    lastShot = millis();
    shoot();
  }
  delay(50);
}
