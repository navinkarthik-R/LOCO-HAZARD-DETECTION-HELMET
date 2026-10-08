/*
 * LOCO Hazard Detection Helmet - ESP32 firmware
 *
 * Environment : DHT11 temperature/humidity, MQ-2 combustible gas / smoke
 * Wearer      : MAX30102 heart rate and SpO2
 * Position    : GPS (NEO-6M class, UART)
 * Outputs     : 16x2 I2C LCD, buzzer, RGB LED, Telegram alert with map link
 *
 * Local alarms never depend on Wi-Fi: if the network is down the helmet still
 * beeps and lights up, and keeps retrying the Telegram alert in the background.
 *
 * Prototype / educational project. It is NOT a certified safety device and NOT a
 * medical device.
 */

#include <Arduino.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <MAX30105.h>        // SparkFun MAX3010x library (also drives the MAX30102)
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <spo2_algorithm.h>  // from the same SparkFun library

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h - copy secrets.h.example to secrets.h and fill in your Wi-Fi and Telegram details."
#endif

#include "hazard_logic.h"

// ---------------------------------------------------------------- pins ----
// See docs/WIRING.md. The LCD and MAX30102 share the I2C bus (different
// addresses: 0x27 and 0x57). The RGB LED must NOT use GPIO 21/22 - those are I2C.
constexpr uint8_t GPS_RX_PIN = 16;  // ESP32 RX  <- GPS TX
constexpr uint8_t GPS_TX_PIN = 17;  // ESP32 TX  -> GPS RX
constexpr uint8_t DHT_PIN = 4;
constexpr uint8_t GAS_PIN = 34;     // MQ-2 AO through a voltage divider; ADC1 only
constexpr uint8_t BUZZER_PIN = 19;  // active buzzer
constexpr uint8_t LED_R_PIN = 25;
constexpr uint8_t LED_G_PIN = 26;
constexpr uint8_t LED_B_PIN = 27;
constexpr uint8_t I2C_SDA_PIN = 21;
constexpr uint8_t I2C_SCL_PIN = 22;

constexpr bool LED_COMMON_ANODE = false;  // true if your RGB LED is common-anode
constexpr uint8_t LCD_ADDR = 0x27;        // try 0x3F if the LCD stays blank
#define DHT_TYPE DHT11

// -------------------------------------------------------------- limits ----
// Heat/humidity: defaults carried over from the original prototype. 30 C is
// below normal summer temperature in many places - set limits for your site.
constexpr Thresholds THRESHOLDS = {30.0f, 70.0f, 1.0f};

// Gas, in raw ADC counts (0-4095). PLACEHOLDERS: an MQ-2 is non-selective and
// uncalibrated. Calibrate against your own sensor, divider and environment
// (see README "Calibrating the gas sensor").
//   alarm when reading > clean-air baseline + deltaRaw, or >= absLimitRaw
constexpr GasLimits GAS_LIMITS = {500, 3000, 100};

// Vitals (MAX30102). PLACEHOLDERS, not medical advice. {SpO2 min %, HR min bpm,
// HR max bpm, hysteresis, consecutive readings needed to trigger/clear}.
constexpr VitalsLimits VITALS_LIMITS = {90, 45, 140, 2, 3};

// -------------------------------------------------------------- timing ----
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;        // DHT11 needs >= 1 s between reads
constexpr uint32_t PAGE_INTERVAL_MS = 3000;          // LCD page rotation
constexpr uint32_t BUZZER_ON_MS = 15000;             // buzzer time per alert
constexpr uint32_t ALERT_REPEAT_MS = 5UL * 60000UL;  // re-alert while a hazard persists
constexpr uint32_t ALERT_RETRY_MS = 10000;           // retry a failed send
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t WIFI_BOOT_WAIT_MS = 10000;
constexpr uint32_t GPS_FIX_MAX_AGE_MS = 10000;       // older than this = no fix
constexpr uint32_t GPS_NO_DATA_AFTER_MS = 5000;      // no NMEA at all = wiring problem
constexpr uint8_t SENSOR_FAIL_LIMIT = 3;             // failed reads before "Sensor Error"

// MQ-2: the heater needs time to stabilise. No gas alarm during warm-up; the
// last GAS_BASELINE_WINDOW_MS of warm-up set the clean-air baseline, so power the
// helmet on in clean air.
constexpr uint32_t GAS_WARMUP_MS = 120000;
constexpr uint32_t GAS_BASELINE_WINDOW_MS = 10000;
constexpr uint32_t GAS_INTERVAL_MS = 500;
constexpr uint8_t GAS_SAMPLES = 8;                   // ADC readings averaged per sample

// MAX30102. Window/step follow SparkFun's SpO2 example: 100 samples at 25 sps
// (sampleRate 100 / average 4) = 4 s of data, recomputed every 25 samples = 1 s.
constexpr int SPO2_WINDOW = 100;
constexpr int SPO2_STEP = 25;
constexpr uint8_t MAX_LED_POWER = 60;                // raise for forehead/temple use
constexpr uint32_t CONTACT_IR_MIN = 50000;           // IR below this = no skin contact
constexpr uint32_t VITALS_STALE_MS = 30000;          // no valid vitals this long = drop the alarm

// --------------------------------------------------------------- state ----
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
DHT dht(DHT_PIN, DHT_TYPE);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
MAX30105 particleSensor;

HazardTracker tracker(THRESHOLDS);
GasTracker gasTracker(GAS_LIMITS);
VitalsTracker vitalsTracker(VITALS_LIMITS);

// DHT11
float tempC = NAN;
float humPct = NAN;
bool sensorOk = false;      // have a valid reading
bool sensorFailed = false;  // too many failed reads in a row
uint8_t sensorFails = 0;
uint32_t lastSensorMs = 0;

// MQ-2
int gasRaw = 0;
int gasBaseline = 0;
bool gasReady = false;  // warm-up finished and baseline taken
long gasBaseSum = 0;
int gasBaseN = 0;
uint32_t lastGasMs = 0;

// MAX30102
bool maxOk = false;
uint32_t irBuf[SPO2_WINDOW];
uint32_t redBuf[SPO2_WINDOW];
int filled = 0;
int newSamples = 0;
bool contact = false;
bool vitalsValid = false;
int vSpo2 = 0;
int vHr = 0;
uint32_t lastVitalsMs = 0;

// Alerting
uint8_t prevMask = 0;
bool alertPending = false;
uint32_t lastAlertMs = 0;
uint32_t lastAttemptMs = 0;
uint32_t buzzerUntilMs = 0;  // 0 = off
uint32_t lastWifiTryMs = 0;
char lcdCache[2][17] = {"", ""};
char alertMsg[512];   // static: keeps big buffers off the task stack
char alertBody[1700];

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

static uint8_t activeMask() {
  return (tracker.active() ? CAUSE_ENV : 0) | (gasTracker.active() ? CAUSE_GAS : 0) |
         (vitalsTracker.active() ? CAUSE_VITALS : 0);
}

// Blocking calls (Telegram) leave a gap in the sensor streams; start the
// pulse-oximeter window over so it never mixes samples across the gap.
static void resetVitalsWindow() {
  filled = 0;
  newSamples = 0;
}

// ------------------------------------------------------------ telegram ----
// Never print the URL: it contains the bot token.
static bool telegramPost(const char* method, const char* body) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // TLS certificate is NOT verified (setInsecure) to keep setup simple. See the
  // README "Security notes" for how to pin Telegram's root CA instead.
  WiFiClientSecure client;
  client.setInsecure();

  char url[160];
  snprintf(url, sizeof url, "https://api.telegram.org/bot%s/%s", BOT_TOKEN, method);

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int code = http.POST(String(body));
  http.end();

  Serial.printf("Telegram %s -> HTTP %d\n", method, code);
  return code == 200;
}

// Builds "chat_id=<id>&" into alertBody and returns the offset to append at.
static size_t startBody() {
  char chat[48];
  urlEncode(CHAT_ID, chat, sizeof chat);
  return static_cast<size_t>(snprintf(alertBody, sizeof alertBody, "chat_id=%s&", chat));
}

static bool sendAlert() {
  const bool fix = gpsHasFix();
  const double lat = fix ? gps.location.lat() : 0.0;
  const double lon = fix ? gps.location.lng() : 0.0;

  AlertContext c;
  memset(&c, 0, sizeof c);
  c.causes = activeMask();
  c.tempC = tempC;
  c.humPct = humPct;
  c.th = THRESHOLDS;
  c.gasRaw = gasRaw;
  c.gasBaseline = gasBaseline;
  c.gas = GAS_LIMITS;
  c.spo2 = vSpo2;
  c.hr = vHr;
  c.vitals = VITALS_LIMITS;
  c.hasFix = fix;
  c.lat = lat;
  c.lon = lon;
  formatAlert(alertMsg, sizeof alertMsg, c);

  size_t off = startBody();
  off += snprintf(alertBody + off, sizeof alertBody - off, "text=");
  urlEncode(alertMsg, alertBody + off, sizeof alertBody - off);
  const bool ok = telegramPost("sendMessage", alertBody);

  if (ok && fix) {  // also drop a map pin; failure here is not worth retrying the text
    off = startBody();
    snprintf(alertBody + off, sizeof alertBody - off, "latitude=%.6f&longitude=%.6f", lat, lon);
    telegramPost("sendLocation", alertBody);
  }
  resetVitalsWindow();
  return ok;
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

// ------------------------------------------------- sensor: DHT11 temp/hum ----
static void readEnvironment(uint32_t now) {
  if (!elapsed(now, lastSensorMs, SENSOR_INTERVAL_MS)) return;
  lastSensorMs = now;

  const float h = dht.readHumidity();
  const float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    if (sensorFails < 255) ++sensorFails;
    if (sensorFails >= SENSOR_FAIL_LIMIT) {
      sensorOk = false;
      sensorFailed = true;
    }
    Serial.println("DHT read failed");
    return;
  }

  sensorFails = 0;
  sensorFailed = false;
  sensorOk = true;
  tempC = t;
  humPct = h;
  Serial.printf("Temp %.1f C | Humidity %.0f %%\n", t, h);
  tracker.update(t, h);
}

// -------------------------------------------------------- sensor: MQ-2 gas ----
static void readGas(uint32_t now) {
  if (!elapsed(now, lastGasMs, GAS_INTERVAL_MS)) return;
  lastGasMs = now;

  long sum = 0;
  for (uint8_t i = 0; i < GAS_SAMPLES; ++i) sum += analogRead(GAS_PIN);
  gasRaw = static_cast<int>(sum / GAS_SAMPLES);

  if (now < GAS_WARMUP_MS) {  // warming up: collect the baseline, never alarm
    if (now + GAS_BASELINE_WINDOW_MS >= GAS_WARMUP_MS) {
      gasBaseSum += gasRaw;
      ++gasBaseN;
    }
    return;
  }
  if (!gasReady) {
    gasBaseline = gasBaseN > 0 ? static_cast<int>(gasBaseSum / gasBaseN) : gasRaw;
    gasReady = true;
    Serial.printf("Gas baseline %d, alarm level %d\n", gasBaseline, gasTripLevel(gasBaseline, GAS_LIMITS));
  }
  gasTracker.update(gasRaw, gasBaseline);
}

// ----------------------------------------------- sensor: MAX30102 SpO2 / HR ----
static void computeVitals(uint32_t now) {
  int32_t spo2 = 0;
  int32_t hr = 0;
  int8_t spo2Valid = 0;
  int8_t hrValid = 0;
  maxim_heart_rate_and_oxygen_saturation(irBuf, SPO2_WINDOW, redBuf, &spo2, &spo2Valid, &hr, &hrValid);

  // Reject anything the algorithm flags invalid or that is physiologically absurd.
  if (!spo2Valid || !hrValid || spo2 < 70 || spo2 > 100 || hr < 30 || hr > 220) return;

  vSpo2 = static_cast<int>(spo2);
  vHr = static_cast<int>(hr);
  vitalsValid = true;
  lastVitalsMs = now;
  Serial.printf("SpO2 %d %% | HR %d bpm\n", vSpo2, vHr);
  vitalsTracker.update(vSpo2, vHr);
}

static void readVitals(uint32_t now) {
  if (!maxOk) return;

  particleSensor.check();  // pull new samples out of the sensor FIFO
  while (particleSensor.available()) {
    const uint32_t red = particleSensor.getFIFORed();
    const uint32_t ir = particleSensor.getFIFOIR();
    particleSensor.nextSample();

    if (ir < CONTACT_IR_MIN) {  // not touching skin: discard the window
      contact = false;
      resetVitalsWindow();
      continue;
    }
    contact = true;

    if (filled < SPO2_WINDOW) {
      irBuf[filled] = ir;
      redBuf[filled] = red;
      ++filled;
      newSamples = 0;
      if (filled < SPO2_WINDOW) continue;  // window full: compute right away
    } else {                               // slide the window by one sample
      memmove(irBuf, irBuf + 1, (SPO2_WINDOW - 1) * sizeof irBuf[0]);
      memmove(redBuf, redBuf + 1, (SPO2_WINDOW - 1) * sizeof redBuf[0]);
      irBuf[SPO2_WINDOW - 1] = ir;
      redBuf[SPO2_WINDOW - 1] = red;
      if (++newSamples < SPO2_STEP) continue;
      newSamples = 0;
    }
    computeVitals(now);
  }

  // Sensor removed or signal lost for a while: we can't assert a vitals hazard.
  if (vitalsValid && elapsed(now, lastVitalsMs, VITALS_STALE_MS)) {
    vitalsValid = false;
    vitalsTracker.reset();
  }
}

// ------------------------------------------------------- hazard outputs ----
static void updateOutputs(uint32_t now) {
  const uint8_t mask = activeMask();

  if ((mask & ~prevMask) != 0 ||  // a new hazard appeared (even if another is active)
      (mask != 0 && !alertPending && elapsed(now, lastAlertMs, ALERT_REPEAT_MS))) {
    alertPending = true;
    lastAttemptMs = now - ALERT_RETRY_MS;  // send immediately
    buzzerUntilMs = now + BUZZER_ON_MS;
  }
  if (mask == 0) {
    alertPending = false;
    buzzerUntilMs = 0;
  }
  prevMask = mask;

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

  if (mask != 0)
    setLed(true, false, false);  // red   = hazard
  else if (sensorFailed)
    setLed(false, false, true);  // blue  = DHT sensor error
  else
    setLed(false, true, false);  // green = normal
}

// ----------------------------------------------------------------- lcd ----
static void pageEnv(char* l0, size_t n0, char* l1, size_t n1) {
  if (sensorFailed) {
    snprintf(l0, n0, "Sensor Error");
    snprintf(l1, n1, "Check DHT wiring");
  } else if (!sensorOk) {
    snprintf(l0, n0, "Reading...");
    l1[0] = '\0';
  } else {
    snprintf(l0, n0, "Temp: %.1f C", tempC);
    snprintf(l1, n1, "Humi: %.0f %%", humPct);
  }
}

static void pageGas(uint32_t now, char* l0, size_t n0, char* l1, size_t n1) {
  if (!gasReady) {
    snprintf(l0, n0, "Gas: warming up");
    snprintf(l1, n1, "%us left", static_cast<unsigned>((GAS_WARMUP_MS - (now < GAS_WARMUP_MS ? now : GAS_WARMUP_MS)) / 1000));
  } else {
    snprintf(l0, n0, gasTracker.active() ? "Gas: %d ALERT" : "Gas: %d", gasRaw);
    snprintf(l1, n1, "Alarm at: %d", gasTripLevel(gasBaseline, GAS_LIMITS));
  }
}

static void pageVitals(char* l0, size_t n0, char* l1, size_t n1) {
  if (!maxOk) {
    snprintf(l0, n0, "SpO2: no sensor");
    snprintf(l1, n1, "Check I2C wiring");
  } else if (!contact) {
    snprintf(l0, n0, "SpO2/HR: --");
    snprintf(l1, n1, "No contact");
  } else if (!vitalsValid) {
    snprintf(l0, n0, "SpO2/HR: ...");
    snprintf(l1, n1, "Measuring...");
  } else {
    snprintf(l0, n0, "SpO2: %d %%", vSpo2);
    snprintf(l1, n1, "HR: %d bpm", vHr);
  }
}

static void pageGps(char* l0, size_t n0, char* l1, size_t n1) {
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
  const uint32_t slot = now / PAGE_INTERVAL_MS;

  const uint8_t mask = activeMask();
  if (mask != 0) {
    // Cycle through the active causes on line 2.
    uint8_t list[3];
    uint8_t n = 0;
    if (mask & CAUSE_ENV) list[n++] = CAUSE_ENV;
    if (mask & CAUSE_GAS) list[n++] = CAUSE_GAS;
    if (mask & CAUSE_VITALS) list[n++] = CAUSE_VITALS;
    snprintf(l0, sizeof l0, "!! HAZARD !!");
    switch (list[slot % n]) {
      case CAUSE_ENV:
        snprintf(l1, sizeof l1, "T:%.1fC H:%.0f%%", tempC, humPct);
        break;
      case CAUSE_GAS:
        snprintf(l1, sizeof l1, "GAS:%d >%d", gasRaw, gasTripLevel(gasBaseline, GAS_LIMITS));
        break;
      default:
        snprintf(l1, sizeof l1, "SpO2:%d HR:%d", vSpo2, vHr);
        break;
    }
  } else {
    switch (slot % 4) {
      case 0:
        pageEnv(l0, sizeof l0, l1, sizeof l1);
        break;
      case 1:
        pageGas(now, l0, sizeof l0, l1, sizeof l1);
        break;
      case 2:
        pageVitals(l0, sizeof l0, l1, sizeof l1);
        break;
      default:
        pageGps(l0, sizeof l0, l1, sizeof l1);
        break;
    }
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
  pinMode(GAS_PIN, INPUT);
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

  // MAX30102: red + IR LEDs (mode 2), 100 Hz with 4x averaging = 25 samples/s.
  maxOk = particleSensor.begin(Wire, I2C_SPEED_STANDARD);
  if (maxOk) {
    particleSensor.setup(MAX_LED_POWER, 4, 2, 100, 411, 4096);
  } else {
    Serial.println("MAX30102 not found - check wiring (I2C address 0x57). Vitals disabled.");
  }

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
  readEnvironment(now);
  readGas(now);
  readVitals(now);
  updateOutputs(now);
  updateLcd(now);
}
