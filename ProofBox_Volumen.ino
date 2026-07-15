// ============================================================
//  ProofBox — Módulo VOLUMEN v1.0
//  Solo sensor VL53L0X + WiFi + MQTT
//  Modo Ratio × y Modo Meta (dedo / ratio / cm)
//  SDA → GPIO 21 | SCL → GPIO 22 | VIN → 3.3V
// ============================================================

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <VL53L0X.h>

// ─── CONFIGURACIÓN ────────────────────────────────────────────
#define DEVICE_ID    "proofbox-vol01"
#define CMD_SECRET   "pb-vol-b3ccb5"
#define MQTT_SERVER  "broker.hivemq.com"
#define MQTT_PORT    1883
#define TOPIC_STATUS "proofboxvol/" DEVICE_ID "/status"
#define TOPIC_CMD    "proofboxvol/" DEVICE_ID "/cmd"
#define TOPIC_NOTIFY "proofboxvol/" DEVICE_ID "/notify"

#define PIN_SDA  21
#define PIN_SCL  22
#define PIN_LED  2

WiFiClient   espClient;
PubSubClient mqtt(espClient);
Preferences  prefs;
VL53L0X      laser;

// ─── ESTADO DEL SENSOR ────────────────────────────────────────
float distanceMM   = 0;
bool  laserOk      = false;
int   laserFails   = 0;

// ─── MODO RATIO ───────────────────────────────────────────────
float ratioInitDist = 0;
float riseRatio      = 1.0;
bool  ratioCal        = false;

// ─── MODO META ────────────────────────────────────────────────
float metaStartDist = 0;
float metaGoalDist  = 0;
float metaProgress  = 0.0;
float metaRatioGoal = 0.0;
float metaRatioCurr = 1.0;
float metaGrams     = 0;
bool  metaStartSet  = false;
bool  metaGoalSet   = false;
bool  metaNotified  = false;

// ─── OFFSET DE CALIBRACIÓN DEL SENSOR ─────────────────────────
// Ajusta este valor si la medida no coincide con la regla.
// offset = lectura_del_sensor - distancia_real_medida
float sensorOffset = 49.0;

unsigned long lastSensorRead = 0;
unsigned long lastPublish    = 0;
unsigned long startMillis    = 0;
const unsigned long SENSOR_INTERVAL  = 300;
const unsigned long PUBLISH_INTERVAL = 1000;

// ─── LECTURA VL53L0X ───────────────────────────────────────────
float readDistanceRaw() {
  if (!laserOk) return distanceMM + sensorOffset;

  const int N = 3;
  uint16_t readings[N];
  int valid = 0;

  for (int i = 0; i < N; i++) {
    uint16_t d = laser.readRangeSingleMillimeters();
    if (!laser.timeoutOccurred() && d > 0 && d < 8190) {
      readings[valid++] = d;
    }
  }

  if (valid == 0) {
    laserFails++;
    if (laserFails > 10) {
      laserFails = 0;
      Serial.println("⚠️ Sensor no responde — reiniciando I2C...");
      Wire.begin(PIN_SDA, PIN_SCL);
      if (laser.init()) {
        laser.setMeasurementTimingBudget(20000);
        Serial.println("✅ Sensor recuperado");
      }
    }
    return distanceMM + sensorOffset;
  }
  laserFails = 0;

  // Ordenar y tomar mediana
  for (int i = 0; i < valid-1; i++)
    for (int j = 0; j < valid-i-1; j++)
      if (readings[j] > readings[j+1]) { uint16_t t=readings[j]; readings[j]=readings[j+1]; readings[j+1]=t; }

  return readings[valid/2];
}

void readSensors() {
  float raw = readDistanceRaw();
  float newDist = max(0.0f, raw - sensorOffset);

  // Rechazar salto brusco mayor a 100mm, pero no más de 5 veces seguidas
  static int rejectCount = 0;
  if (distanceMM > 0 && abs(newDist - distanceMM) > 100.0 && rejectCount < 5) {
    Serial.printf("Lectura rechazada: %.0fmm (anterior: %.0fmm)\n", newDist, distanceMM);
    rejectCount++;
  } else {
    distanceMM = newDist;
    rejectCount = 0;
  }

  if (ratioCal && ratioInitDist > 5)
    riseRatio = constrain(ratioInitDist / max(distanceMM, 1.0f), 1.0, 10.0);

  if (metaStartSet && metaGoalSet && metaStartDist > metaGoalDist) {
    metaProgress  = constrain((metaStartDist - distanceMM) / (metaStartDist - metaGoalDist) * 100.0, 0.0, 100.0);
    metaRatioCurr = constrain(metaStartDist / max(distanceMM, 1.0f), 1.0, 10.0);
  }
}

// ─── NOTIFICACIÓN META ─────────────────────────────────────────
void checkMetaNotification() {
  if (metaStartSet && metaGoalSet && metaProgress >= 100.0 && !metaNotified) {
    metaNotified = true;
    Serial.println("🎉 Meta alcanzada");
    StaticJsonDocument<128> notif;
    notif["event"] = "meta_reached";
    notif["ratio"] = round(metaRatioCurr * 100) / 100.0;
    notif["grams"] = metaGrams;
    char buf[128]; serializeJson(notif, buf);
    mqtt.publish(TOPIC_NOTIFY, buf, false);
  }
  if (metaProgress < 95.0) metaNotified = false;
}

// ─── MQTT PUBLICAR ──────────────────────────────────────────────
void publishStatus() {
  StaticJsonDocument<384> doc;
  doc["dist"]          = round(distanceMM);
  doc["rise"]          = round(riseRatio * 100) / 100.0;
  doc["uptime"]         = (millis()-startMillis)/1000;
  doc["rssi"]           = WiFi.RSSI();
  doc["laserOk"]        = laserOk;
  doc["metaProgress"]   = round(metaProgress*10)/10.0;
  doc["metaRatioCurr"]  = round(metaRatioCurr*100)/100.0;
  doc["metaRatioGoal"]  = round(metaRatioGoal*100)/100.0;
  doc["metaGrams"]      = metaGrams;
  doc["metaStartSet"]   = metaStartSet;
  doc["metaGoalSet"]    = metaGoalSet;
  float cmGrown = metaStartSet ? max(0.0f, (metaStartDist - distanceMM) / 10.0f) : 0.0f;
  doc["cmGrown"]        = round(cmGrown * 10) / 10.0;
  char buf[384]; serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATUS, buf, true);
}

// ─── MQTT RECIBIR ────────────────────────────────────────────────
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0';
  StaticJsonDocument<192> doc;
  deserializeJson(doc, (char*)payload);
  if (String(doc["secret"]|"") != String(CMD_SECRET)) return;

  String cmd = doc["cmd"]|"";
  Serial.printf("📩 %s\n", cmd.c_str());

  if (cmd == "calibrate") {
    if (distanceMM > 5) {
      ratioInitDist = distanceMM; riseRatio = 1.0; ratioCal = true;
      Serial.printf("✅ Ratio cal: %.1fmm\n", ratioInitDist);
    }
  }
  else if (cmd == "metaSetStart") {
    if (distanceMM > 5) {
      metaStartDist = distanceMM; metaGrams = doc["grams"]|0.0;
      metaProgress = 0; metaRatioCurr = 1.0; metaStartSet = true;
      metaNotified = false;
      if (metaGoalSet && metaStartDist > metaGoalDist)
        metaRatioGoal = metaStartDist / metaGoalDist;
      Serial.printf("✅ Meta inicio: %.1fmm\n", metaStartDist);
    }
  }
  else if (cmd == "metaSetGoalFinger") {
    if (distanceMM > 5 && metaStartSet && distanceMM < metaStartDist) {
      metaGoalDist = distanceMM; metaRatioGoal = metaStartDist / metaGoalDist;
      metaGoalSet = true; metaNotified = false;
      Serial.printf("✅ Meta dedo: %.1fmm = %.2f×\n", metaGoalDist, metaRatioGoal);
    }
  }
  else if (cmd == "metaSetGoalRatio") {
    float r = doc["ratio"]|0.0;
    if (r > 1.0 && r <= 10.0 && metaStartSet) {
      metaRatioGoal = r; metaGoalDist = metaStartDist / r;
      metaGoalSet = true; metaNotified = false;
      Serial.printf("✅ Meta ratio: %.2f×\n", r);
    }
  }
  else if (cmd == "metaSetGoalCm") {
    float cm = doc["cm"]|0.0;
    if (cm > 0 && metaStartSet) {
      metaGoalDist = max(5.0f, metaStartDist - (cm * 10.0f));
      metaRatioGoal = metaStartDist / metaGoalDist;
      metaGoalSet = true; metaNotified = false;
      Serial.printf("✅ Meta cm: %.1fcm = %.2f×\n", cm, metaRatioGoal);
    }
  }
  else if (cmd == "metaReset") {
    metaStartSet = false; metaGoalSet = false; metaProgress = 0;
    metaRatioCurr = 1.0; metaRatioGoal = 0; metaGrams = 0; metaNotified = false;
    Serial.println("↺ Meta reset");
  }
  else if (cmd == "setOffset") {
    float o = doc["offset"]|sensorOffset;
    sensorOffset = o;
    prefs.putFloat("offset", sensorOffset);
    Serial.printf("🔧 Offset ajustado: %.1fmm\n", sensorOffset);
  }
  else if (cmd == "resetwifi") {
    delay(500); WiFiManager wm; wm.resetSettings(); ESP.restart();
  }

  publishStatus();
}

// ─── MQTT RECONECTAR ──────────────────────────────────────────
void mqttReconnect() {
  while (!mqtt.connected()) {
    String cid = "PBVOL-" + String(random(0xffff), HEX);
    if (mqtt.connect(cid.c_str())) {
      mqtt.subscribe(TOPIC_CMD);
      publishStatus();
      Serial.println("🔌 MQTT ✅");
    } else {
      Serial.printf("MQTT ❌ rc=%d\n", mqtt.state());
      delay(5000);
    }
  }
}

// ─── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(500);
  pinMode(PIN_LED, OUTPUT);

  prefs.begin("proofboxvol", false);
  sensorOffset = prefs.getFloat("offset", 49.0);

  Serial.println("\n📏 ProofBox Volumen v1.0");

  // VL53L0X
  delay(100);
  Wire.begin(PIN_SDA, PIN_SCL);
  laser.setTimeout(100);
  for (int i = 0; i < 3; i++) {
    if (laser.init()) {
      laser.setMeasurementTimingBudget(20000);
      laserOk = true;
      Serial.println("✅ VL53L0X listo");
      break;
    }
    Serial.printf("VL53L0X intento %d fallido...\n", i+1);
    delay(200);
  }
  if (!laserOk) Serial.println("❌ VL53L0X no encontrado — verifica SDA/SCL");

  // WiFi
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("ProofBoxVol-Setup")) ESP.restart();
  Serial.println("✅ WiFi: " + WiFi.SSID());
  digitalWrite(PIN_LED, HIGH);

  // MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(512);

  startMillis = millis();

  delay(500); readSensors();
  if (distanceMM > 5) {
    ratioInitDist = distanceMM; ratioCal = true;
    Serial.printf("📏 Cal auto: %.1fmm\n", ratioInitDist);
  }
}

// ─── LOOP ─────────────────────────────────────────────────────
void loop() {
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    checkMetaNotification();
  }
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    publishStatus();
    Serial.printf("📏%.0fmm | 📈%.2f× | 🎯%.0f%%\n", distanceMM, riseRatio, metaProgress);
  }
}
