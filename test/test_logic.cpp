// Host-side tests for hazard_logic.h.
//   g++ -std=c++17 -Wall -Wextra -Werror test/test_logic.cpp -o /tmp/test_logic && /tmp/test_logic
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../firmware/loco_hazard_helmet/hazard_logic.h"

static int failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

static const Thresholds TH = {30.0f, 70.0f, 1.0f};
static const GasLimits GAS = {500, 3000, 100};
static const VitalsLimits VIT = {90, 45, 140, 2, 3};

static void testTracker() {
  HazardTracker t(TH);
  CHECK(!t.update(30.0f, 70.0f));  // limits are strict: equal is not a hazard
  CHECK(!t.active());

  CHECK(t.update(30.1f, 50.0f));   // temperature trips it
  CHECK(t.active());
  CHECK(!t.update(29.5f, 50.0f));  // inside the hysteresis band: stays latched
  CHECK(t.active());
  CHECK(t.update(29.0f, 50.0f));   // 1.0 below the limit: clears
  CHECK(!t.active());

  CHECK(t.update(20.0f, 70.5f));   // humidity trips it
  CHECK(t.active());
  CHECK(!t.update(20.0f, 69.5f));  // humidity still inside the band
  CHECK(t.active());
  CHECK(t.update(20.0f, 69.0f));
  CHECK(!t.active());

  CHECK(t.update(35.0f, 80.0f));   // both high, then only one recovers
  CHECK(!t.update(25.0f, 80.0f));  // humidity still high: stays latched
  CHECK(t.active());
  CHECK(t.update(25.0f, 60.0f));
  CHECK(!t.active());
}

static void testGas() {
  GasTracker g(GAS);
  const int base = 800;  // trip level = min(800+500, 3000) = 1300
  CHECK(gasTripLevel(base, GAS) == 1300);

  CHECK(!g.update(1300, base));  // relative limit is strict: equal does not trip
  CHECK(g.update(1301, base));
  CHECK(g.active());
  CHECK(!g.update(1250, base));  // inside the hysteresis band: stays latched
  CHECK(g.active());
  CHECK(g.update(1199, base));   // below trip - hysteresis: clears
  CHECK(!g.active());

  // Baseline taken in smoky air: the absolute ceiling still trips.
  GasTracker g2(GAS);
  const int badBase = 2900;
  CHECK(gasTripLevel(badBase, GAS) == 3000);
  CHECK(!g2.update(2999, badBase));
  CHECK(g2.update(3000, badBase));
  CHECK(!g2.update(2950, badBase));  // within hysteresis of 3000
  CHECK(g2.active());
  CHECK(g2.update(2899, badBase));
  CHECK(!g2.active());

  g2.update(3500, badBase);
  g2.reset();
  CHECK(!g2.active());
}

static void testVitals() {
  VitalsTracker v(VIT);  // confirm = 3
  CHECK(!v.update(97, 72));
  CHECK(!v.active());

  // One or two bad readings (motion artefact) must not alarm...
  CHECK(!v.update(85, 72));
  CHECK(!v.update(85, 72));
  CHECK(!v.update(97, 72));          // ...and a good one resets the count
  CHECK(!v.update(85, 72));
  CHECK(!v.update(85, 72));
  CHECK(v.update(85, 72));           // third in a row trips it
  CHECK(v.active());

  // Clearing needs 3 readings clear of the limit by the hysteresis (90 + 2).
  CHECK(!v.update(91, 72));          // in the band: neither confirms nor clears
  CHECK(!v.update(95, 72));
  CHECK(!v.update(95, 72));
  CHECK(v.active());
  CHECK(v.update(95, 72));
  CHECK(!v.active());

  // Heart rate limits, high and low.
  VitalsTracker h(VIT);
  h.update(97, 141); h.update(97, 141);
  CHECK(h.update(97, 141) && h.active());
  VitalsTracker l(VIT);
  l.update(97, 44); l.update(97, 44);
  CHECK(l.update(97, 44) && l.active());

  // Sensor removed: reset() clears the alarm, and says so only if it was active.
  CHECK(l.reset());
  CHECK(!l.active());
  CHECK(!l.reset());
}

static void testUrlEncode() {
  char out[64];
  size_t n = urlEncode("a b\nc?=&é", out, sizeof out);
  CHECK(std::strcmp(out, "a%20b%0Ac%3F%3D%26%C3%A9") == 0);
  CHECK(n == std::strlen(out));

  CHECK(urlEncode("Az09-_.~", out, sizeof out) == 8);
  CHECK(std::strcmp(out, "Az09-_.~") == 0);

  // Truncation: always terminated, never half a %XX triplet.
  for (size_t cap = 1; cap < 12; ++cap) {
    char small[16];
    std::memset(small, 'X', sizeof small);
    size_t w = urlEncode("ab cd ef", small, cap);
    CHECK(w < cap);
    CHECK(small[w] == '\0');
    CHECK(std::strlen(small) == w);
    // A '%' in the last two slots would mean a cut-off triplet.
    CHECK(w < 1 || small[w - 1] != '%');
    CHECK(w < 2 || small[w - 2] != '%');
  }
  CHECK(urlEncode("abc", out, 0) == 0);
}

static AlertContext ctx(uint8_t causes, bool fix) {
  AlertContext c;
  std::memset(&c, 0, sizeof c);
  c.causes = causes;
  c.tempC = 35.0f;
  c.humPct = 50.0f;
  c.th = TH;
  c.gasRaw = 2100;
  c.gasBaseline = 800;
  c.gas = GAS;
  c.spo2 = 88;
  c.hr = 72;
  c.vitals = VIT;
  c.hasFix = fix;
  c.lat = 12.971600;
  c.lon = 77.594600;
  return c;
}

static void testFormatAlert() {
  char buf[512];

  formatAlert(buf, sizeof buf, ctx(CAUSE_ENV, true));
  CHECK(std::strstr(buf, "HAZARD DETECTED") == buf);
  CHECK(std::strstr(buf, "Heat/humidity: temperature 35.0 C (limit 30.0), humidity 50 % (limit 70)") != nullptr);
  CHECK(std::strstr(buf, "Gas/smoke") == nullptr);
  CHECK(std::strstr(buf, "Wearer vitals") == nullptr);
  CHECK(std::strstr(buf, "https://maps.google.com/?q=12.971600,77.594600") != nullptr);

  formatAlert(buf, sizeof buf, ctx(CAUSE_GAS, false));
  CHECK(std::strstr(buf, "sensor reading 2100 (clean-air baseline 800, alarm level 1300)") != nullptr);
  CHECK(std::strstr(buf, "Heat/humidity") == nullptr);
  CHECK(std::strstr(buf, "no GPS fix") != nullptr);

  formatAlert(buf, sizeof buf, ctx(CAUSE_VITALS, false));
  CHECK(std::strstr(buf, "SpO2 88 % (min 90), heart rate 72 bpm (range 45-140)") != nullptr);
  CHECK(std::strstr(buf, "Indicative only") != nullptr);

  formatAlert(buf, sizeof buf, ctx(CAUSE_ENV | CAUSE_GAS | CAUSE_VITALS, true));
  CHECK(std::strstr(buf, "Heat/humidity") != nullptr);
  CHECK(std::strstr(buf, "Gas/smoke") != nullptr);
  CHECK(std::strstr(buf, "Wearer vitals") != nullptr);

  // Tiny buffers must stay terminated and in bounds, for every combination.
  for (uint8_t causes = 1; causes < 8; ++causes) {
    for (size_t cap = 1; cap < 330; ++cap) {
      char tiny[400];
      std::memset(tiny, 'X', sizeof tiny);
      formatAlert(tiny, cap, ctx(causes, true));
      CHECK(std::strlen(tiny) < cap);
      CHECK(tiny[cap] == 'X' || cap >= sizeof tiny);  // nothing written past cap
    }
  }
}

int main() {
  testTracker();
  testGas();
  testVitals();
  testUrlEncode();
  testFormatAlert();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("all tests passed");
  return 0;
}
