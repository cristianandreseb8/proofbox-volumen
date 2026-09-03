// ============================================================
//  ProofBox — Módulo VOLUMEN v1.2
//  VL53L0X (volumen) + conductividad de 2 electrodos + WiFi + MQTT
//  Modo Ratio × y Modo Meta (dedo / ratio / cm)
//
//  PLACA: ESP32 clásico (DevKit / WROOM-32).
//  En Arduino IDE: "ESP32 Dev Module".
//
//  VL53L0X:       SDA → GPIO 21 | SCL → GPIO 22 | VIN → 3V3 | GND → GND
//  Conductividad: GPIO 25 y 26 (excitación) | GPIO 34 (medida)
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

// Conductividad. El ADC2 no funciona con el WiFi encendido, así que la medida
// TIENE que ir en un pin de ADC1: 32, 33, 34, 35, 36 o 39. El GPIO 34 es de solo
// entrada, que para leer es justo lo que hace falta, y además no tiene pull-ups
// internos que falseen la medida.
// 25 y 26 son salidas normales, sin función especial al arrancar.
#define PIN_COND_A    25
#define PIN_COND_B    26
#define PIN_COND_ADC  34
#define COND_R_SERIES 4700.0f   // resistencia conocida en serie, en ohmios

WiFiClient   espClient;
PubSubClient mqtt(espClient);
Preferences  prefs;
VL53L0X      laser;

// ─── ESTADO DEL SENSOR ────────────────────────────────────────
float distanceMM   = 0;
bool  laserInit    = false;  // el chip respondió a init()
bool  laserOk      = false;  // hay lecturas válidas ahora mismo
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

// ─── CONDUCTIVIDAD ────────────────────────────────────────────
float condOhms   = 0;      // resistencia medida entre electrodos
float condUS     = 0;      // conductancia en microsiemens (1e6 / ohmios)
float condBaseUS = 0;      // referencia marcada por el usuario
float condRel    = 0;      // % respecto a esa referencia
bool  condOk     = false;  // hay circuito: los electrodos tocan algo

// ─── OFFSET DE CALIBRACIÓN DEL SENSOR ─────────────────────────
// Ajusta este valor si la medida no coincide con la regla.
// offset = lectura_del_sensor - distancia_real_medida
float sensorOffset = 49.0;

unsigned long lastSensorRead = 0;
unsigned long lastCondRead   = 0;
unsigned long lastPublish    = 0;
unsigned long startMillis    = 0;
const unsigned long SENSOR_INTERVAL  = 300;
const unsigned long COND_INTERVAL    = 2000;
const unsigned long PUBLISH_INTERVAL = 1000;

// ─── LECTURA VL53L0X ───────────────────────────────────────────
float readDistanceRaw() {
  const int N = 3;
  uint16_t readings[N];
  int valid = 0;

  if (laserInit) {
    for (int i = 0; i < N; i++) {
      uint16_t d = laser.readRangeSingleMillimeters();
      if (!laser.timeoutOccurred() && d > 0 && d < 8190) {
        readings[valid++] = d;
      }
    }
  }

  if (valid == 0) {
    laserFails++;
    // ~1s sin una sola lectura válida: el sensor está caído, que la app lo diga.
    if (laserFails >= 3) laserOk = false;
    if (laserFails > 10) {
      laserFails = 0;
      Serial.println("⚠️ Sensor no responde — reiniciando I2C...");
      Wire.begin(PIN_SDA, PIN_SCL);
      laserInit = laser.init();
      if (laserInit) {
        laser.setMeasurementTimingBudget(20000);
        Serial.println("✅ Sensor recuperado");
      }
    }
    return distanceMM + sensorOffset;
  }
  laserFails = 0;
  laserOk = true;

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

// ─── CONDUCTIVIDAD ─────────────────────────────────────────────
// Dos electrodos de inox en serie con una resistencia conocida. Se excitan
// invirtiendo la polaridad en cada medida: así el líquido ve corriente alterna
// y no continua. Sin continua no hay electrólisis, ni burbujas en el electrodo,
// ni metal comiéndose. Entre medida y medida los dos pines quedan en alta
// impedancia, así que en reposo no circula nada.
//
//   GPIO A ─[electrodo 1]~~ líquido ~~[electrodo 2]─┬─ R ─ GPIO B
//                                                    └─ ADC (GPIO 34)
//
//   Fase +  (A=3V3, B=0):  v1 = Vdd · R  / (R+Rx)
//   Fase −  (A=0, B=3V3):  v2 = Vdd · Rx / (R+Rx)
//
// Al dividir v2/v1 la Vdd se cancela sola:  Rx = R · v2/v1. No hace falta saber
// la tensión de alimentación real ni que el ADC esté bien calibrado, solo que
// sea lineal — que para esto sobra.
float readResistanceOhms() {
  const int PAIRS = 9;
  float rs[PAIRS];
  int n = 0;

  pinMode(PIN_COND_A, OUTPUT);
  pinMode(PIN_COND_B, OUTPUT);

  for (int i = 0; i < PAIRS; i++) {
    digitalWrite(PIN_COND_A, HIGH); digitalWrite(PIN_COND_B, LOW);
    delayMicroseconds(1200);
    float v1 = analogReadMilliVolts(PIN_COND_ADC);

    digitalWrite(PIN_COND_A, LOW); digitalWrite(PIN_COND_B, HIGH);
    delayMicroseconds(1200);
    float v2 = analogReadMilliVolts(PIN_COND_ADC);

    // v1 ≈ 0 significa que no pasa corriente: electrodos al aire, secos,
    // o un cable suelto. No es "conductividad cero", es que no hay medida.
    if (v1 < 15.0f) continue;
    rs[n++] = COND_R_SERIES * (v2 / v1);
  }

  pinMode(PIN_COND_A, INPUT);   // alta impedancia en reposo
  pinMode(PIN_COND_B, INPUT);

  if (n == 0) return -1;

  // Mediana: una burbuja de gas justo entre los electrodos dispara la lectura.
  for (int i = 0; i < n-1; i++)
    for (int j = 0; j < n-i-1; j++)
      if (rs[j] > rs[j+1]) { float t=rs[j]; rs[j]=rs[j+1]; rs[j+1]=t; }
  return rs[n/2];
}

void readConductivity() {
  float r = readResistanceOhms();
  if (r < 0) {
    condOk = false; condOhms = 0; condUS = 0; condRel = 0;
    return;
  }
  condOhms = max(r, 1.0f);
  float us = 1000000.0f / condOhms;
  // Suavizado exponencial encima de la mediana: en masa madre el gas hace que
  // la lectura tiemble aunque la mezcla no cambie.
  condUS = condOk ? (condUS * 0.7f + us * 0.3f) : us;
  condOk = true;
  condRel = (condBaseUS > 0) ? (condUS / condBaseUS) * 100.0f : 0.0f;
}

// ─── SESIÓN PERSISTENTE ────────────────────────────────────────
// El avance de la masa sobrevive a los cortes de corriente: si desenchufás el
// ProofBox y lo volvés a enchufar, sigue contando desde donde estaba. Volver a
// cero es siempre una decisión explícita — "Nueva masa", metaReset o calibrate.
void saveSession() {
  prefs.putFloat("ratioInit", ratioInitDist);
  prefs.putBool ("ratioCal",  ratioCal);
  prefs.putFloat("mStart",    metaStartDist);
  prefs.putFloat("mGoal",     metaGoalDist);
  prefs.putFloat("mGoalR",    metaRatioGoal);
  prefs.putFloat("mGrams",    metaGrams);
  prefs.putBool ("mStartSet", metaStartSet);
  prefs.putBool ("mGoalSet",  metaGoalSet);
  prefs.putBool ("mNotified", metaNotified);
}

void loadSession() {
  ratioInitDist = prefs.getFloat("ratioInit", 0.0);
  ratioCal      = prefs.getBool ("ratioCal",  false);
  metaStartDist = prefs.getFloat("mStart",    0.0);
  metaGoalDist  = prefs.getFloat("mGoal",     0.0);
  metaRatioGoal = prefs.getFloat("mGoalR",    0.0);
  metaGrams     = prefs.getFloat("mGrams",    0.0);
  metaStartSet  = prefs.getBool ("mStartSet", false);
  metaGoalSet   = prefs.getBool ("mGoalSet",  false);
  metaNotified  = prefs.getBool ("mNotified", false);
}

// ─── NOTIFICACIÓN META ─────────────────────────────────────────
void checkMetaNotification() {
  if (metaStartSet && metaGoalSet && metaProgress >= 100.0 && !metaNotified) {
    metaNotified = true;
    prefs.putBool("mNotified", true);   // que no vuelva a avisar si se reinicia
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
  StaticJsonDocument<512> doc;
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
  doc["condOk"]         = condOk;
  doc["cond"]           = round(condUS * 10) / 10.0;      // microsiemens
  doc["condKohm"]       = round(condOhms / 100.0) / 10.0; // kΩ, 1 decimal
  doc["condRel"]        = round(condRel * 10) / 10.0;     // % sobre la base
  doc["condBaseSet"]    = condBaseUS > 0;
  char buf[512]; serializeJson(doc, buf);
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
      // Marcar el inicio de la masa marca también la base de conductividad:
      // las dos señales arrancan del mismo instante y se pueden comparar.
      if (condOk && condUS > 0) {
        condBaseUS = condUS; condRel = 100.0;
        prefs.putFloat("condBase", condBaseUS);
      }
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
  else if (cmd == "condBase") {
    if (condOk && condUS > 0) {
      condBaseUS = condUS; condRel = 100.0;
      prefs.putFloat("condBase", condBaseUS);
      Serial.printf("⚡ Base conductividad: %.1f µS (%.1f kΩ)\n", condBaseUS, condOhms/1000.0);
    } else {
      Serial.println("⚡ Sin contacto en los electrodos — base no marcada");
    }
  }
  else if (cmd == "condReset") {
    condBaseUS = 0; condRel = 0;
    prefs.remove("condBase");
    Serial.println("↺ Base conductividad borrada");
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

  saveSession();
  publishStatus();
}

// ─── RED: RECONEXIÓN SIN BLOQUEAR ─────────────────────────────
// Esto era un while con delay(5000) dentro. Mientras el broker no contestaba,
// el loop entero se paraba: durante esos segundos el ESP32 no publicaba nada y
// la app mostraba "sin señal" aunque el aparato estuviera perfectamente. Ahora
// reintenta cada 3 s sin frenar el resto, así que los publish no se cortan.
unsigned long lastMqttTry = 0;
unsigned long lastWifiTry = 0;

void netEnsure() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry >= 5000) {
      lastWifiTry = millis();
      Serial.println("📶 WiFi caído — reconectando...");
      WiFi.reconnect();
    }
    return;
  }
  if (mqtt.connected()) return;
  if (millis() - lastMqttTry < 3000) return;
  lastMqttTry = millis();
  String cid = "PBVOL-" + String(random(0xffff), HEX);
  if (mqtt.connect(cid.c_str())) {
    mqtt.subscribe(TOPIC_CMD);
    publishStatus();
    Serial.println("🔌 MQTT ✅");
  } else {
    Serial.printf("MQTT ❌ rc=%d\n", mqtt.state());
  }
}

// ─── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(500);
  pinMode(PIN_LED, OUTPUT);

  prefs.begin("proofboxvol", false);
  sensorOffset = prefs.getFloat("offset", 49.0);
  condBaseUS   = prefs.getFloat("condBase", 0.0);
  loadSession();

  Serial.println("\n📏 ProofBox Volumen v1.2");

  // Conductividad: los pines de excitación arrancan en alta impedancia para no
  // meter continua en el líquido antes de la primera medida.
  pinMode(PIN_COND_A, INPUT);
  pinMode(PIN_COND_B, INPUT);
  // La atenuación por defecto del core ESP32 ya es 11 dB (rango completo).

  // VL53L0X
  delay(100);
  Wire.begin(PIN_SDA, PIN_SCL);
  laser.setTimeout(100);
  for (int i = 0; i < 3; i++) {
    if (laser.init()) {
      laser.setMeasurementTimingBudget(20000);
      laserInit = true;
      Serial.println("✅ VL53L0X listo");
      break;
    }
    Serial.printf("VL53L0X intento %d fallido...\n", i+1);
    delay(200);
  }
  // Aunque falle aquí, readDistanceRaw() reintenta init() cada ~3s: si el sensor
  // aparece más tarde (o se vuelve a enchufar), se recupera solo.
  if (!laserInit) Serial.println("❌ VL53L0X no encontrado — verifica SDA/SCL");

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
  mqtt.setBufferSize(768);

  startMillis = millis();

  delay(500); readSensors(); readConductivity();
  if (condOk) Serial.printf("⚡ Conductividad: %.1f kΩ / %.1f µS\n", condOhms/1000.0, condUS);
  else        Serial.println("⚡ Conductividad: electrodos sin contacto");
  // Solo auto-calibra si NO había sesión guardada. Si se reinició a mitad de una
  // fermentación, el punto cero es el de antes — no el de ahora, que ya subió.
  if (ratioCal || metaStartSet) {
    Serial.printf("↩ Sesión recuperada: cero %.1fmm%s\n", ratioInitDist,
                  metaStartSet ? " · meta activa" : "");
  } else if (distanceMM > 5) {
    ratioInitDist = distanceMM; ratioCal = true;
    saveSession();
    Serial.printf("📏 Cal auto: %.1fmm\n", ratioInitDist);
  }
}

// ─── LOOP ─────────────────────────────────────────────────────
void loop() {
  netEnsure();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    checkMetaNotification();
  }
  if (now - lastCondRead >= COND_INTERVAL) {
    lastCondRead = now;
    readConductivity();
  }
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    publishStatus();
    Serial.printf("📏%.0fmm | 📈%.2f× | 🎯%.0f%% | ⚡%s\n",
                  distanceMM, riseRatio, metaProgress,
                  condOk ? String(condUS, 1).c_str() : "--");
  }
}
