#pragma once
#include <Arduino.h>
extern int g_spo2, g_hr, g_valid, g_algoCalls;
inline void maxim_heart_rate_and_oxygen_saturation(uint32_t*, int32_t, uint32_t*, int32_t* s, int8_t* sv,
                                                   int32_t* h, int8_t* hv) {
  ++g_algoCalls; *s = g_spo2; *sv = (int8_t)g_valid; *h = g_hr; *hv = (int8_t)g_valid;
}
