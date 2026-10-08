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

static void testFormatAlert() {
  char buf[256];
  formatAlert(buf, sizeof buf, 35.0f, 50.0f, true, 12.971600, 77.594600, TH);
  CHECK(std::strstr(buf, "high temperature") != nullptr);
  CHECK(std::strstr(buf, "high humidity") == nullptr);
  CHECK(std::strstr(buf, "https://maps.google.com/?q=12.971600,77.594600") != nullptr);

  formatAlert(buf, sizeof buf, 35.0f, 80.0f, false, 0, 0, TH);
  CHECK(std::strstr(buf, "high temperature and humidity") != nullptr);
  CHECK(std::strstr(buf, "no GPS fix") != nullptr);

  formatAlert(buf, sizeof buf, 29.5f, 50.0f, false, 0, 0, TH);
  CHECK(std::strstr(buf, "hazard still active") != nullptr);

  // Tiny buffers must stay terminated and in bounds.
  for (size_t cap = 1; cap < 40; ++cap) {
    char tiny[64];
    std::memset(tiny, 'X', sizeof tiny);
    formatAlert(tiny, cap, 35.0f, 80.0f, true, 1.0, 2.0, TH);
    CHECK(std::strlen(tiny) < cap);
  }
}

int main() {
  testTracker();
  testUrlEncode();
  testFormatAlert();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("all tests passed");
  return 0;
}
