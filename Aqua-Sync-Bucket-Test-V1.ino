/*
 * AquaSync V1 - Roof tank monitor (ESP32 + JSN-SR04T)
 * Optimized for simultaneous Wi-Fi & BLE stability without lag/disconnects.
 *
 * Pins: TRIG = GPIO5, ECHO = GPIO18 (via 1k/2k voltage divider)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ================= USER SETTINGS =================
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* AP_NAME       = "AquaSync_AP";
const char* AP_PASS       = "12345678";

const int PIN_TRIG = 5;
const int PIN_ECHO = 18;

// Calibration values (Bucket test: Empty = 50cm, Full = 20cm)
const float TANK_HEIGHT_CM   = 50.0f;  // Tank bottom distance when empty
const float SENSOR_OFFSET_CM = 20.0f;  // Water surface distance when full
const int   FULL_PCT         = 95;

const long  GMT_OFFSET_SEC   = 19800;   // UTC+5:30 (India)
// =================================================

const float    MIN_VALID_CM   = 18.0f;
const float    MAX_VALID_CM   = TANK_HEIGHT_CM + 15.0f;
const float    FILL_RISE_PCT  = 8.0f;
const uint32_t FILL_STOP_MS   = 120000UL;
const uint32_t HIST_EVERY_MS  = 600000UL;
const int      HIST_N         = 144;

#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_MOTOR_UUID  "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"

WebServer server(80);
Preferences prefs;
BLEServer* pServer = nullptr;
BLECharacteristic* pStatusChar = nullptr;
bool bleConnected = false;

// ---------- sensor state ----------
float win[8];
int   winN = 0;
int   badNoEcho = 0, badClose = 0, badFar = 0;
float smoothDist = -1;
float levelF = 0;
int   levelPct = 0;
bool  sensorOk = false;
int   failStreak = 0;
const char* errText = "starting";

// ---------- fill tracking ----------
bool     trackerInit = false;
bool     filling = false;
float    lowLevel = 0, peakLevel = 0;
uint32_t lastRiseMs = 0;
uint32_t fillCount = 0;
uint32_t lastFillEpoch = 0;

// ---------- history ----------
uint8_t  hist[HIST_N];
uint16_t histHead = 0;
uint32_t histEnd = 0;

// ---------- timing loop variables ----------
bool     ntpStarted = false;
uint32_t lastPing = 0;
uint32_t lastProcess = 0;
uint32_t lastBleNotify = 0;
uint32_t lastHist = 0;
uint32_t lastLog = 0;
uint32_t lastWifiCheck = 0;

uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (uint32_t)t : 0;
}

// 18ms timeout covers up to ~300 cm comfortably without lagging CPU loops
float pingCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 18000UL);
  if (us == 0) return -1.0f;
  return us * 0.0343f / 2.0f;
}

float medianOf(float* a, int n) {
  float t[8];
  for (int i = 0; i < n; i++) t[i] = a[i];
  for (int i = 1; i < n; i++) {
    float k = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return t[n / 2];
}

void sampleSensor() {
  float d = pingCm();
  if (d < 0) badNoEcho++;
  else if (d < MIN_VALID_CM) badClose++;
  else if (d > MAX_VALID_CM) badFar++;
  else if (winN < 8) win[winN++] = d;
}

void trackFills(float lvl, uint32_t nowMs) {
  if (!trackerInit) { lowLevel = lvl; trackerInit = true; return; }
  if (!filling) {
    if (lvl < lowLevel) lowLevel = lvl;
    if (lvl >= lowLevel + FILL_RISE_PCT) {
      filling = true;
      peakLevel = lvl;
      lastRiseMs = nowMs;
      Serial.println("[FILL] started");
    }
  } else {
    if (lvl > peakLevel + 0.8f) { peakLevel = lvl; lastRiseMs = nowMs; }
    if ((nowMs - lastRiseMs > FILL_STOP_MS) || lvl >= 98.0f) {
      fillCount++;
      lastFillEpoch = nowEpoch();
      prefs.putUInt("fills", fillCount);
      prefs.putUInt("lastFill", lastFillEpoch);
      Serial.printf("[FILL] finished. Total fills: %u\n", (unsigned)fillCount);
      filling = false;
      lowLevel = lvl;
    }
  }
}

void processWindow() {
  if (winN >= 3) {
    float m = medianOf(win, winN);
    smoothDist = (smoothDist < 0) ? m : (smoothDist * 0.65f + m * 0.35f);
    float usable = TANK_HEIGHT_CM - SENSOR_OFFSET_CM;
    float pct = (TANK_HEIGHT_CM - smoothDist) / usable * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    levelF = pct;
    levelPct = (int)lroundf(pct);
    failStreak = 0;
    sensorOk = true;
    errText = "";
    trackFills(levelF, millis());
  } else {
    if (++failStreak >= 4) {
      sensorOk = false;
      if (badClose > badNoEcho && badClose >= badFar) errText = "too close";
      else if (badFar > badNoEcho)                    errText = "out of range";
      else                                            errText = "no echo";
    }
  }
  winN = 0; badNoEcho = 0; badClose = 0; badFar = 0;
}

void pushHistory() {
  hist[histHead] = sensorOk ? (uint8_t)levelPct : 255;
  histHead = (histHead + 1) % HIST_N;
  histEnd = nowEpoch();
  prefs.putBytes("hist", hist, HIST_N);
  prefs.putUShort("head", histHead);
  prefs.putUInt("histT", histEnd);
}

String statusJson(bool compact) {
  String j;
  j.reserve(240);
  j += "{\"hw\":1,\"level\":";
  j += levelPct;
  j += ",\"distance\":";
  j += (sensorOk ? (int)lroundf(smoothDist) : 0);
  j += ",\"sensor\":";
  j += (sensorOk ? "true" : "false");
  if (!sensorOk) {
    j += ",\"error\":\"";
    j += errText;
    j += "\"";
  }
  j += ",\"motor\":false,\"full\":";
  j += ((sensorOk && levelPct >= FULL_PCT) ? "true" : "false");
  j += ",\"fills\":";
  j += fillCount;
  if (!compact) {
    j += ",\"last_fill\":";
    j += lastFillEpoch;
    j += ",\"time\":";
    j += nowEpoch();
    j += ",\"rssi\":";
    j += (WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    j += ",\"up\":";
    j += (uint32_t)(millis() / 1000);
  }
  j += "}";
  return j;
}

void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}

void handleStatus() {
  cors();
  server.send(200, "application/json", statusJson(false));
}

void handleMotor() {
  cors();
  server.send(200, "application/json", statusJson(false));
}

void handleReset() {
  winN = 0; smoothDist = -1; failStreak = 0;
  trackerInit = false; filling = false;
  cors();
  server.send(200, "application/json", statusJson(false));
}

void handleHistory() {
  String j;
  j.reserve(900);
  j += "{\"interval_min\":10,\"end\":";
  j += histEnd;
  j += ",\"levels\":[";
  for (int i = 0; i < HIST_N; i++) {
    uint8_t v = hist[(histHead + i) % HIST_N];
    if (i) j += ",";
    j += ((v == 255) ? -1 : (int)v);
  }
  j += "]}";
  cors();
  server.send(200, "application/json", j);
}

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) {
    bleConnected = true;
    Serial.println("[BLE] Client connected");
  }
  void onDisconnect(BLEServer* s) {
    bleConnected = false;
    Serial.println("[BLE] Client disconnected, re-advertising...");
    pServer->startAdvertising();
  }
};

class MotorCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {}
};

void notifyBle() {
  if (pStatusChar == nullptr || !bleConnected) return;
  String p = statusJson(true);
  pStatusChar->setValue((uint8_t*)p.c_str(), p.length());
  pStatusChar->notify();
}

void startNtp() {
  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  ntpStarted = true;
}

void wifiMaintenance(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpStarted) startNtp();
  } else {
    // Background auto-reconnect handles station link without freezing BLE
    WiFi.reconnect();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  Serial.println("\n--- AquaSync V1 (Optimized Dual Radio) ---");

  prefs.begin("aqua", false);
  fillCount = prefs.getUInt("fills", 0);
  lastFillEpoch = prefs.getUInt("lastFill", 0);
  memset(hist, 255, sizeof(hist));
  if (prefs.getBytesLength("hist") == HIST_N) {
    prefs.getBytes("hist", hist, HIST_N);
    histHead = prefs.getUShort("head", 0) % HIST_N;
    histEnd = prefs.getUInt("histT", 0);
  }

  // Pure Station mode with auto reconnect prevents radio thrashing
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("aquasync");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[Wi-Fi] Connecting");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[Wi-Fi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    startNtp();
  } else {
    Serial.println("\n[Wi-Fi] Primary AP down, enabling Fallback SoftAP");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_NAME, AP_PASS);
    Serial.print("[Wi-Fi] SoftAP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/status", HTTP_GET, handleStatus);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/motor", HTTP_POST, handleMotor);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound([]() {
    cors();
    if (server.method() == HTTP_OPTIONS) server.send(204);
    else server.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  server.begin();
  Serial.println("[HTTP] Server ready on Port 80");

  // BLE Configuration with stable transmission settings
  BLEDevice::init("AquaSync_BLE");
  BLEDevice::setPower(ESP_PWR_LVL_P7); // Stable transmission power
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  BLEService* svc = pServer->createService(SERVICE_UUID);
  pStatusChar = svc->createCharacteristic(
                  CHAR_STATUS_UUID,
                  BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
                );
  pStatusChar->addDescriptor(new BLE2902());

  BLECharacteristic* pMotorChar = svc->createCharacteristic(
                                    CHAR_MOTOR_UUID,
                                    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                                  );
  pMotorChar->setCallbacks(new MotorCB());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(false);
  adv->setMinPreferred(0x06); // Fast BLE connect parameters
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] AquaSync_BLE ready and advertising");
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  // 1. Ultrasonic sampling every 120ms
  if (now - lastPing >= 120) {
    lastPing = now;
    sampleSensor();
  }

  // 2. Filter & data processing every 800ms
  if (now - lastProcess >= 800) {
    lastProcess = now;
    processWindow();
  }

  // 3. Dedicated BLE notify interval (1.2s prevents BLE buffer lockups)
  if (now - lastBleNotify >= 1200) {
    lastBleNotify = now;
    notifyBle();
  }

  // 4. History log every 10 minutes
  if (now - lastHist >= HIST_EVERY_MS) {
    lastHist = now;
    pushHistory();
  }

  // 5. Silent Wi-Fi link maintenance every 10 seconds
  if (now - lastWifiCheck >= 10000) {
    lastWifiCheck = now;
    wifiMaintenance(now);
  }

  // 6. Serial monitor debug prints every 3 seconds
  if (now - lastLog >= 3000) {
    lastLog = now;
    if (sensorOk) {
      Serial.printf("dist=%.1f cm | level=%d%% | BLE=%s | IP=%s\n",
                    smoothDist, levelPct,
                    bleConnected ? "ON" : "OFF",
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DISCONNECTED");
    } else {
      Serial.printf("Sensor Warning: %s\n", errText);
    }
  }
}