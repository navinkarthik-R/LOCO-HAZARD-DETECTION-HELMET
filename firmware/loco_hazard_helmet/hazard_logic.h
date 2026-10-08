// Pure logic for the LOCO hazard helmet: no Arduino dependencies, so it is
// unit-tested on a normal PC (see test/test_logic.cpp) and compiled into the
// firmware unchanged.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct Thresholds {
  float tempMaxC;    // hazard when temperature is strictly above this
  float humMaxPct;   // hazard when relative humidity is strictly above this
  float hysteresis;  // both readings must drop this far below the limit to clear
};

// Latching hazard detector. Without hysteresis a DHT11 reading hovering around
// the limit would flip the alarm on and off every couple of seconds.
class HazardTracker {
 public:
  explicit HazardTracker(const Thresholds& t) : t_(t), active_(false) {}

  // Feed one valid reading. Returns true if the hazard state changed.
  bool update(float tempC, float humPct) {
    bool next = active_;
    if (!active_) {
      if (tempC > t_.tempMaxC || humPct > t_.humMaxPct) next = true;
    } else if (tempC <= t_.tempMaxC - t_.hysteresis &&
               humPct <= t_.humMaxPct - t_.hysteresis) {
      next = false;
    }
    const bool changed = (next != active_);
    active_ = next;
    return changed;
  }

  bool active() const { return active_; }

 private:
  Thresholds t_;
  bool active_;
};

// Percent-encodes `in` into `out` (RFC 3986 unreserved characters pass through).
// Always NUL-terminates when cap > 0 and never cuts a %XX triplet in half.
// Returns the number of characters written, excluding the NUL.
inline size_t urlEncode(const char* in, char* out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  if (cap == 0) return 0;
  size_t n = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p; ++p) {
    const unsigned char c = *p;
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                       c == '.' || c == '~';
    if (plain) {
      if (n + 1 >= cap) break;
      out[n++] = static_cast<char>(c);
    } else {
      if (n + 3 >= cap) break;
      out[n++] = '%';
      out[n++] = hex[c >> 4];
      out[n++] = hex[c & 0x0F];
    }
  }
  out[n] = '\0';
  return n;
}

// Builds the Telegram alert text. Always NUL-terminated, truncated to `cap`.
inline void formatAlert(char* buf, size_t cap, float tempC, float humPct, bool hasFix,
                        double lat, double lon, const Thresholds& th) {
  if (cap == 0) return;
  const bool tHigh = tempC > th.tempMaxC;
  const bool hHigh = humPct > th.humMaxPct;
  const char* why = (tHigh && hHigh) ? "high temperature and humidity"
                    : tHigh          ? "high temperature"
                    : hHigh          ? "high humidity"
                                     : "hazard still active";

  int n = snprintf(buf, cap, "HAZARD DETECTED (%s)\nTemperature: %.1f C (limit %.1f)\nHumidity: %.0f %% (limit %.0f)\n",
                   why, tempC, th.tempMaxC, humPct, th.humMaxPct);
  size_t off = (n < 0) ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);

  if (hasFix) {
    snprintf(buf + off, cap - off, "Location: https://maps.google.com/?q=%.6f,%.6f", lat, lon);
  } else {
    snprintf(buf + off, cap - off, "Location: no GPS fix yet");
  }
}
