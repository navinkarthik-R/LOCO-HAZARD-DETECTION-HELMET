// Pure logic for the LOCO hazard helmet: no Arduino dependencies, so it is
// unit-tested on a normal PC (see test/test_logic.cpp) and compiled into the
// firmware unchanged.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------ heat/humidity --

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

  // Feed one valid reading. Returns true if the state changed.
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

// ------------------------------------------------------------------ gas (MQ-2)

// All values are raw ADC counts (0-4095). An MQ-2 is non-selective and its
// absolute reading drifts with temperature, humidity and age, so the main test is
// "how far above the clean-air baseline measured at start-up", plus an absolute
// ceiling in case the baseline itself was taken in contaminated air.
struct GasLimits {
  int deltaRaw;       // alarm when reading exceeds baseline + deltaRaw
  int absLimitRaw;    // alarm when reading reaches this, whatever the baseline
  int hysteresisRaw;  // reading must fall this far below the trip point to clear
};

inline int gasTripLevel(int baseline, const GasLimits& l) {
  const int rel = baseline + l.deltaRaw;
  return rel < l.absLimitRaw ? rel : l.absLimitRaw;
}

class GasTracker {
 public:
  explicit GasTracker(const GasLimits& l) : l_(l), active_(false) {}

  bool update(int raw, int baseline) {
    const int trip = gasTripLevel(baseline, l_);
    bool next = active_;
    if (!active_) {
      if (raw >= l_.absLimitRaw || raw > baseline + l_.deltaRaw) next = true;
    } else if (raw < trip - l_.hysteresisRaw) {
      next = false;
    }
    const bool changed = (next != active_);
    active_ = next;
    return changed;
  }

  bool active() const { return active_; }
  void reset() { active_ = false; }

 private:
  GasLimits l_;
  bool active_;
};

// ------------------------------------------------ vitals (MAX30102 SpO2 / HR) --

struct VitalsLimits {
  int spo2Min;       // alarm when SpO2 (%) is below this
  int hrMin;         // alarm when heart rate (bpm) is below this
  int hrMax;         // alarm when heart rate (bpm) is above this
  int hysteresis;    // readings must be this far inside the limits to clear
  uint8_t confirm;   // consecutive out-of-range (or in-range) readings required
};

// Optical SpO2 on a moving person is noisy, so a single bad reading must not
// raise the alarm: `confirm` bad readings in a row are required, and the same
// number of good ones to clear. Only feed it readings the sensor marked valid.
class VitalsTracker {
 public:
  explicit VitalsTracker(const VitalsLimits& l)
      : l_(l), active_(false), badStreak_(0), goodStreak_(0) {}

  bool update(int spo2, int hr) {
    const bool bad = spo2 < l_.spo2Min || hr < l_.hrMin || hr > l_.hrMax;
    const bool good = spo2 >= l_.spo2Min + l_.hysteresis &&
                      hr >= l_.hrMin + l_.hysteresis && hr <= l_.hrMax - l_.hysteresis;
    if (bad) {
      if (badStreak_ < 255) ++badStreak_;
      goodStreak_ = 0;
    } else if (good) {
      if (goodStreak_ < 255) ++goodStreak_;
      badStreak_ = 0;
    } else {  // inside the hysteresis band: neither confirms nor clears
      badStreak_ = 0;
      goodStreak_ = 0;
    }

    bool next = active_;
    if (!active_ && badStreak_ >= l_.confirm) next = true;
    else if (active_ && goodStreak_ >= l_.confirm) next = false;
    const bool changed = (next != active_);
    active_ = next;
    return changed;
  }

  // No valid vitals for a while (sensor removed): we cannot assert a hazard.
  bool reset() {
    const bool changed = active_;
    active_ = false;
    badStreak_ = 0;
    goodStreak_ = 0;
    return changed;
  }

  bool active() const { return active_; }

 private:
  VitalsLimits l_;
  bool active_;
  uint8_t badStreak_;
  uint8_t goodStreak_;
};

// ------------------------------------------------------------------ alerting --

enum : uint8_t { CAUSE_ENV = 1, CAUSE_GAS = 2, CAUSE_VITALS = 4 };

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

// Everything the alert text needs, captured at the moment of sending.
struct AlertContext {
  uint8_t causes;  // CAUSE_* bitmask of the alarms that are active
  float tempC;
  float humPct;
  Thresholds th;
  int gasRaw;
  int gasBaseline;
  GasLimits gas;
  int spo2;
  int hr;
  VitalsLimits vitals;
  bool hasFix;
  double lat;
  double lon;
};

// snprintf into buf at `off`, never overrunning `cap`; advances `off`.
inline void appendf(char* buf, size_t cap, size_t& off, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
inline void appendf(char* buf, size_t cap, size_t& off, const char* fmt, ...) {
  if (off + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + off, cap - off, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  off += (static_cast<size_t>(n) < cap - off) ? static_cast<size_t>(n) : cap - off - 1;
}

// Builds the Telegram alert text. Always NUL-terminated, truncated to `cap`.
inline void formatAlert(char* buf, size_t cap, const AlertContext& c) {
  if (cap == 0) return;
  buf[0] = '\0';
  size_t off = 0;
  appendf(buf, cap, off, "HAZARD DETECTED\n");

  if (c.causes & CAUSE_ENV) {
    appendf(buf, cap, off, "Heat/humidity: temperature %.1f C (limit %.1f), humidity %.0f %% (limit %.0f)\n",
            c.tempC, c.th.tempMaxC, c.humPct, c.th.humMaxPct);
  }
  if (c.causes & CAUSE_GAS) {
    appendf(buf, cap, off, "Gas/smoke: sensor reading %d (clean-air baseline %d, alarm level %d)\n",
            c.gasRaw, c.gasBaseline, gasTripLevel(c.gasBaseline, c.gas));
  }
  if (c.causes & CAUSE_VITALS) {
    appendf(buf, cap, off, "Wearer vitals: SpO2 %d %% (min %d), heart rate %d bpm (range %d-%d). Indicative only.\n",
            c.spo2, c.vitals.spo2Min, c.hr, c.vitals.hrMin, c.vitals.hrMax);
  }

  if (c.hasFix) {
    appendf(buf, cap, off, "Location: https://maps.google.com/?q=%.6f,%.6f", c.lat, c.lon);
  } else {
    appendf(buf, cap, off, "Location: no GPS fix yet");
  }
}
