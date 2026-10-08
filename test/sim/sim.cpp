// Runs the real firmware sketch on a PC against fake Arduino libraries (see stubs/)
// with a fake clock, sensors and network, and checks the alert/buzzer/LED behaviour.
//   g++ -std=gnu++17 -Wall -Wextra -x c++ -I test/sim/stubs -I firmware/loco_hazard_helmet test/sim/sim.cpp -o /tmp/simbin
//   /tmp/simbin          full scenario run
//   /tmp/simbin nomax    MAX30102 missing at boot
#include <Arduino.h>

uint32_t g_now = 0;
int g_pin[64];
bool g_quiet = true;
float g_temp = 25, g_hum = 50;
int g_wifi = 3, g_http = 200;
bool g_fix = false;
int g_gas = 800;                                      // MQ-2 raw ADC
bool g_maxPresent = true;                             // MAX30102 on the bus
uint32_t g_ir = 1000, g_red = 1000;                   // IR below 50000 = no skin contact
int g_spo2 = 97, g_hr = 72, g_valid = 1, g_algoCalls = 0;
std::vector<std::string> g_calls, g_bodies;
std::string g_lcd[2];

#include "../../firmware/loco_hazard_helmet/loco_hazard_helmet.ino"

void tick(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 100) {
    g_now += 100;
    loop();
  }
}

static int fails = 0;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL line %d: %s\n", __LINE__, #c);         \
      ++fails;                                            \
    }                                                     \
  } while (0)

static int count(const char* m) {
  int n = 0;
  for (auto& c : g_calls)
    if (c.find(m) != std::string::npos) ++n;
  return n;
}
static bool bodyHas(const char* s) { return !g_bodies.empty() && g_bodies.back().find(s) != std::string::npos; }
static bool isRed() { return g_pin[25] == HIGH && g_pin[26] == LOW && g_pin[27] == LOW; }
static bool isGreen() { return g_pin[25] == LOW && g_pin[26] == HIGH && g_pin[27] == LOW; }
static bool isBlue() { return g_pin[25] == LOW && g_pin[26] == LOW && g_pin[27] == HIGH; }
static bool lcdSeen(const char* text, uint32_t forMs) {  // does line 0 show `text` at any point?
  for (uint32_t t = 0; t < forMs; t += 100) {
    tick(100);
    if (g_lcd[0].find(text) != std::string::npos) return true;
  }
  return false;
}

static void scenarioNoMax() {
  g_maxPresent = false;
  setup();
  tick(3000);
  CHECK(!maxOk);
  CHECK(lcdSeen("SpO2: no sensor", 15000));  // vitals page tells the user
  g_ir = 90000; tick(10000);                  // nothing to read, must not crash or alarm
  CHECK(g_calls.empty());
  CHECK(!vitalsTracker.active());
}

static void scenarioFull() {
  setup();
  CHECK(maxOk);

  // --- Gas warm-up: high readings during warm-up must not alarm -----------------
  g_gas = 3500;
  tick(60000);
  CHECK(g_calls.empty() && isGreen());
  CHECK(lcdSeen("Gas: warming up", 12000));
  g_gas = 800;                    // clean air for the baseline window (last 10 s)
  tick(58000);
  CHECK(gasReady && gasBaseline == 800);
  CHECK(g_calls.empty() && isGreen() && g_pin[19] == LOW);

  // --- Heat/humidity (DHT11) ----------------------------------------------------
  g_temp = 31;
  tick(3000);
  CHECK(isRed());
  CHECK(count("sendMessage") == 1 && count("sendLocation") == 0);  // no GPS fix: text only
  CHECK(bodyHas("Heat%2Fhumidity") && bodyHas("chat_id=1&text="));
  CHECK(g_pin[19] == HIGH);                                        // buzzer on
  CHECK(g_lcd[1].find("T:31.0C") != std::string::npos);
  tick(14000);
  CHECK(g_pin[19] == LOW && isRed());
  tick(200000);
  CHECK(count("sendMessage") == 1);                                // no spam while it persists
  tick(95000);
  CHECK(count("sendMessage") == 2 && g_pin[19] == HIGH);           // repeat after 5 min
  g_temp = 29.5; tick(4000); CHECK(isRed());                       // hysteresis keeps it latched
  g_temp = 28.9; tick(4000); CHECK(isGreen() && g_pin[19] == LOW);

  // --- Failing network, then success; location pin only with a fix --------------
  g_calls.clear(); g_http = -1; g_fix = true; g_temp = 33; tick(35000);
  int tries = count("sendMessage");
  CHECK(tries >= 3 && tries <= 5);                                  // retried every ~10 s
  g_http = 200; tick(12000);
  int after = count("sendMessage");
  tick(30000);
  CHECK(count("sendMessage") == after);                             // stops once delivered
  CHECK(count("sendLocation") == 1);
  g_fix = false;

  // --- Wi-Fi down: local alarm works, alert queued, delivered on reconnect ------
  g_temp = 20; tick(4000); g_calls.clear();
  g_wifi = 6; g_temp = 40; tick(4000);
  CHECK(isRed() && g_pin[19] == HIGH && g_calls.empty());
  g_wifi = 3; tick(12000);
  CHECK(count("sendMessage") == 1);

  // --- DHT failure: blue LED, no alert; recovers --------------------------------
  g_temp = 20; tick(4000); g_calls.clear();
  g_temp = NAN; tick(10000);
  CHECK(isBlue() && g_calls.empty());
  CHECK(lcdSeen("Sensor Error", 15000));
  g_temp = 22; tick(4000);
  CHECK(isGreen());

  // --- Gas (MQ-2) ---------------------------------------------------------------
  g_calls.clear();
  g_gas = 1300; tick(3000);                  // equal to baseline + delta: not yet
  CHECK(g_calls.empty() && isGreen());
  g_gas = 1400; tick(2000);
  CHECK(isRed() && g_pin[19] == HIGH);
  CHECK(count("sendMessage") == 1 && bodyHas("Gas%2Fsmoke") && !bodyHas("Heat%2Fhumidity"));
  CHECK(g_lcd[0].find("HAZARD") != std::string::npos && g_lcd[1].find("GAS:1400") != std::string::npos);
  g_gas = 1250; tick(3000); CHECK(isRed());                          // within hysteresis
  g_gas = 1150; tick(3000); CHECK(isGreen() && g_pin[19] == LOW);

  // --- Vitals (MAX30102) --------------------------------------------------------
  g_calls.clear();
  g_ir = 1000; tick(3000);
  CHECK(!contact && isGreen());
  g_ir = 80000; g_red = 70000; g_algoCalls = 0;
  tick(2000);
  CHECK(contact && g_algoCalls == 0);                                // window still filling (4 s)
  tick(4000);
  CHECK(g_algoCalls >= 1 && vitalsValid && vSpo2 == 97 && vHr == 72);
  CHECK(g_calls.empty() && isGreen());
  CHECK(lcdSeen("SpO2: 97", 15000));
  g_valid = 0; tick(5000);                                           // algorithm says "invalid"
  CHECK(g_calls.empty());                                            // invalid readings are ignored
  g_valid = 1; g_spo2 = 85;
  tick(2000); CHECK(g_calls.empty());                                // 1-2 bad readings: ignored
  tick(2500);
  CHECK(isRed() && count("sendMessage") == 1);                       // 3 in a row: alarm
  CHECK(bodyHas("Wearer%20vitals") && bodyHas("SpO2%2085"));
  g_spo2 = 96; tick(5000);
  CHECK(isGreen() && g_pin[19] == LOW);                              // recovers
  g_spo2 = 85; g_calls.clear(); tick(5000); CHECK(isRed());
  g_ir = 1000;                                                       // sensor taken off
  tick(10000); CHECK(isRed());                                       // still latched for a while...
  tick(25000); CHECK(isGreen());                                     // ...then dropped: can't assert
  g_spo2 = 97; g_ir = 80000; tick(12000);
  CHECK(vitalsValid && isGreen());

  // --- A blocking send leaves a gap in the sensor stream: the pulse-oximeter ----
  // --- window must restart rather than mix samples across it --------------------
  CHECK(filled == SPO2_WINDOW);                                      // full window before the alert
  g_calls.clear(); g_temp = 35;
  for (int i = 0; i < 100 && count("sendMessage") == 0; ++i) tick(100);
  CHECK(count("sendMessage") == 1);
  CHECK(filled < 10);                                                // restarted right after the send

  // --- Several causes at once: a NEW cause alerts immediately -------------------
  g_gas = 1500; tick(2000);
  CHECK(count("sendMessage") == 2 && bodyHas("Heat%2Fhumidity") && bodyHas("Gas%2Fsmoke"));
  g_temp = 22; tick(4000);
  CHECK(isRed());                                                    // gas still active
  g_gas = 800; tick(3000);
  CHECK(isGreen() && g_pin[19] == LOW);
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "nomax")
    scenarioNoMax();
  else
    scenarioFull();
  puts(fails ? "SIMULATION FAILED" : "simulation passed");
  return fails ? 1 : 0;
}
