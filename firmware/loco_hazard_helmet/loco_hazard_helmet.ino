/*
 * LOCO Hazard Detection Helmet - ESP32 firmware
 *
 * Reads temperature/humidity (DHT11) and position (GPS), shows status on a
 * 16x2 I2C LCD, drives a buzzer + RGB LED locally, and sends a Telegram
 * message (with a map link and pin) when a hazard is detected.
 *
 * Local alarms never depend on Wi-Fi: if the network is down the helmet still
 * beeps and lights up, and keeps retrying the Telegram alert in the background.
 *
 * Prototype / educational project. It is NOT a certified safety device.
 */

#include <Arduino.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h - copy secrets.h.example to secrets.h and fill in your Wi-Fi and Telegram details."
#endif

#include "hazard_logic.h"

// ---------------------------------------------------------------- pins ----
// See docs/WIRING.md. NOTE: the original prototype sketch put the RGB LED on
// GPIO 21/22/23, but the ESP32's default I2C bus (the LCD) is GPIO 21 (SDA) and
// 22 (SCL), so the LED and LCD fought over the same pins. The LED now uses
// 25/26/27.
constexpr uint8_t GPS_RX_PIN = 16;  // ESP32 RX  <- GPS TX
constexpr uint8_t GPS_TX_PIN = 17;  // ESP32 TX  -> GPS RX
constexpr uint8_t DHT_PIN = 4;
constexpr uint8_t BUZZER_PIN = 19;  // active buzzer
constexpr uint8_t LED_R_PIN = 25;
constexpr uint8_t LED_G_PIN = 26;
constexpr uint8_t LED_B_PIN = 27;
constexpr uint8_t I2C_SDA_PIN = 21;
constexpr uint8_t I2C_SCL_PIN = 22;

constexpr bool LED_COMMON_ANODE = false;  // true if your RGB LED is common-anode
constexpr uint8_t LCD_ADDR = 0x27;        // try 0x3F if the LCD stays blank
#define DHT_TYPE DHT11

// -------------------------------------------------------------- tuning ----
// Defaults carried over from the original prototype. 30 C is below normal
// summer ambient temperature in many places - set limits for your environment.
constexpr Thresholds THRESHOLDS = {30.0f, 70.0f, 1.0f};

constexpr uint32_t SENSOR_INTERVAL_MS = 2000;       // DHT11 needs >= 1 s between reads
constexpr uint32_t PAGE_INTERVAL_MS = 3000;         // LCD page rotation
constexpr uint32_t BUZZER_ON_MS = 15000;            // buzzer time per alert
constexpr uint32_t ALERT_REPEAT_MS = 5UL * 60000UL; // re-alert while hazard persists
constexpr uint32_t ALERT_RETRY_MS = 10000;          // retry a failed send
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t WIFI_BOOT_WAIT_MS = 10000;
constexpr uint32_t GPS_FIX_MAX_AGE_MS = 10000;      // older than this = no fix
constexpr uint32_t GPS_NO_DATA_AFTER_MS = 5000;     // no NMEA at all = wiring problem
constexpr uint8_t SENSOR_FAIL_LIMIT = 3;            // failed reads before "Sensor Error"

// --------------------------------------------------------------- state ----
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
DHT dht(DHT_PIN, DHT_TYPE);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
HazardTracker tracker(THRESHOLDS);

float tempC = NAN;
float humPct = NAN;
bool sensorOk = false;
uint8_t sensorFails = 0;

bool alertPending = false;
uint32_t lastAlertMs = 0;
uint32_t lastAttemptMs = 0;
uint32_t buzzerUntilMs = 0;  // 0 = off

uint32_t lastSensorMs = 0;
uint32_t lastWifiTryMs = 0;
char lcdCache[2][17] = {"", ""};

// ------------------------------------------------------------- helpers ----
static bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

static void writePin(uint8_t pin, bool on) {
  digitalWrite(pin, (LED_COMMON_ANODE ? !on : on) ? HIGH : LOW);
}

static void setLed(bool r, bool g, bool b) {
  writePin(LED_R_PIN, r);
  writePin(LED_G_PIN, g);
  writePin(LED_B_PIN, b);
}

// Writes a full 16-char line, and only touches the LCD when the text changed
// (the original sketch called lcd.clear() every loop, which flickers).
static void lcdLine(uint8_t row, const char* text) {
  char line[17];
  snprintf(line, sizeof line, "%-16.16s", text);
  if (strcmp(line, lcdCache[row]) == 0) return;
  strcpy(lcdCache[row], line);
  lcd.setCursor(0, row);
  lcd.print(line);
}

static bool gpsHasFix() {
  return gps.location.isValid() && gps.location.age() < GPS_FIX_MAX_AGE_MS;
}

// ------------------------------------------------------------ telegram ----
// Never print `url`: it contains the bot token.
static bool telegramCall(const char* method, const char* query) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // TLS certificate is NOT verified (setInsecure) to keep setup simple. See the
  // README "Security notes" for how to pin Telegram's root CA instead.
  WiFiClientSecure client;
  client.setInsecure();

  char url[1024];
  snprintf(url, sizeof url, "https://api.telegram.org/bot%s/%s?chat_id=%s&%s", BOT_TOKEN,
           method, CHAT_ID, query);

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  http.end();

  Serial.printf("Telegram %s -> HTTP %d\n", method, code);
  return code == 200;
}

static bool sendAlert() {
  const bool fix = gpsHasFix();
  const double lat = fix ? gps.location.lat() : 0.0;
  const double lon = fix ? gps.location.lng() : 0.0;

  char msg[256];
  char enc[768];
  char query[800];

  formatAlert(msg, sizeof msg, tempC, humPct, fix, lat, lon, THRESHOLDS);
  urlEncode(msg, enc, sizeof enc);
  snprintf(query, sizeof query, "text=%s", enc);
  if (!telegramCall("sendMessage", query)) return false;

  if (fix) {  // also drop a map pin; failure here is not worth retrying the text
    snprintf(query, sizeof query, "latitude=%.6f&longitude=%.6f", lat, lon);
    telegramCall("sendLocation", query);
  }
  return true;
}

// ---------------------------------------------------------------- wifi ----
static void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTryMs = millis();
}

static void wifiMaintain(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (!elapsed(now, lastWifiTryMs, WIFI_RETRY_MS)) return;
  Serial.println("Wi-Fi down, retrying");
  WiFi.disconnect();
  wifiBegin();
}

// ------------------------------------------------------------- sensors ----
static void readSensor(uint32_t now) {
  if (!elapsed(now, lastSensorMs, SENSOR_INTERVAL_MS)) return;
  lastSensorMs = now;

  const float h = dht.readHumidity();
  const float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    if (sensorFails < 255) ++sensorFails;
    if (sensorFails >= SENSOR_FAIL_LIMIT) sensorOk = false;
    Serial.println("DHT read failed");
    return;
  }

  sensorFails = 0;
  sensorOk = true;
  tempC = t;
  humPct = h;
  Serial.printf("Temp %.1f C | Humidity %.0f %%\n", t, h);

  if (tracker.update(t, h) && tracker.active()) {  // just entered hazard
    alertPending = true;
    lastAttemptMs = now - ALERT_RETRY_MS;  // send immediately
    buzzerUntilMs = now + BUZZER_ON_MS;
  }
}

// ------------------------------------------------------- hazard outputs ----
static void updateOutputs(uint32_t now) {
  // Re-alert while the hazard persists.
  if (tracker.active() && !alertPending && elapsed(now, lastAlertMs, ALERT_REPEAT_MS)) {
    alertPending = true;
    lastAttemptMs = now - ALERT_RETRY_MS;
    buzzerUntilMs = now + BUZZER_ON_MS;
  }
  if (!tracker.active()) {
    alertPending = false;
    buzzerUntilMs = 0;
  }

  if (alertPending && elapsed(now, lastAttemptMs, ALERT_RETRY_MS)) {
    lastAttemptMs = now;
    if (sendAlert()) {
      alertPending = false;
      lastAlertMs = millis();
    }
  }

  const bool buzz = buzzerUntilMs != 0 && static_cast<int32_t>(buzzerUntilMs - now) > 0;
  if (!buzz) buzzerUntilMs = 0;
  digitalWrite(BUZZER_PIN, buzz ? HIGH : LOW);

  if (tracker.active())
    setLed(true, false, false);  // red   = hazard
  else if (!sensorOk)
    setLed(false, false, true);  // blue  = sensor error
  else
    setLed(false, true, false);  // green = normal
}

// ----------------------------------------------------------------- lcd ----
static void gpsLines(char* l0, size_t n0, char* l1, size_t n1) {
  if (gpsHasFix()) {
    snprintf(l0, n0, "GPS: Fix OK");
    snprintf(l1, n1, "%.4f,%.4f", gps.location.lat(), gps.location.lng());
  } else if (millis() > GPS_NO_DATA_AFTER_MS && gps.charsProcessed() < 10) {
    snprintf(l0, n0, "GPS: No Data");
    snprintf(l1, n1, "Check wiring");
  } else {
    snprintf(l0, n0, "GPS: No Signal");
    snprintf(l1, n1, "Sats:%u Wi-Fi:%s", static_cast<unsigned>(gps.satellites.value()),
             WiFi.status() == WL_CONNECTED ? "OK" : "--");
  }
}

static void updateLcd(uint32_t now) {
  char l0[24];
  char l1[24];

  if (tracker.active()) {
    snprintf(l0, sizeof l0, "!! HAZARD !!");
    snprintf(l1, sizeof l1, "T:%.1fC H:%.0f%%", tempC, humPct);
  } else if (!sensorOk) {
    snprintf(l0, sizeof l0, "Sensor Error");
    snprintf(l1, sizeof l1, "Check DHT wiring");
  } else if ((now / PAGE_INTERVAL_MS) % 2 == 0) {
    snprintf(l0, sizeof l0, "Temp: %.1f C", tempC);
    snprintf(l1, sizeof l1, "Humi: %.0f %%", humPct);
  } else {
    gpsLines(l0, sizeof l0, l1, sizeof l1);
  }
  lcdLine(0, l0);
  lcdLine(1, l1);
}

// ------------------------------------------------------- setup and loop ----
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  setLed(false, false, false);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdLine(0, "LOCO Helmet");
  lcdLine(1, "Starting...");

  dht.begin();
  gpsSerial.setRxBufferSize(1024);  // must be set before begin()
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Quick self-test so wiring problems show up at power-on.
  setLed(true, false, false);
  delay(250);
  setLed(false, true, false);
  delay(250);
  setLed(false, false, true);
  delay(250);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  setLed(false, false, false);

  // Connect to Wi-Fi, but never block forever: local alarms must work offline.
  wifiBegin();
  lcdLine(1, "Wi-Fi...");
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && !elapsed(millis(), start, WIFI_BOOT_WAIT_MS)) {
    delay(250);
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "Wi-Fi connected" : "Wi-Fi not connected yet, continuing offline");
}

void loop() {
  // Feed the GPS parser on every pass; nothing below blocks for long.
  while (gpsSerial.available() > 0) gps.encode(gpsSerial.read());

  const uint32_t now = millis();
  wifiMaintain(now);
  readSensor(now);
  updateOutputs(now);
  updateLcd(now);
}
