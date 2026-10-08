#pragma once
#include <Arduino.h>
#include <Wire.h>
#define I2C_SPEED_STANDARD 100000
extern bool g_maxPresent; extern uint32_t g_ir, g_red;
// Delivers 25 samples per simulated second, like the real sensor at 100 Hz / avg 4.
struct MAX30105 {
  uint32_t last = 0; int pending = 0;
  bool begin(TwoWire&, uint32_t){ last = g_now; return g_maxPresent; }
  void setup(uint8_t, uint8_t, uint8_t, int, int, int){}
  uint16_t check(){
    uint32_t dt = g_now - last; int n = (int)(dt * 25 / 1000);
    if (n > 0) { pending += n; last += (uint32_t)n * 40; }
    if (pending > 32) pending = 32;   // the real FIFO holds 32 samples
    return (uint16_t)pending;
  }
  uint8_t available(){ return (uint8_t)pending; }
  uint32_t getFIFORed(){ return g_red; }
  uint32_t getFIFOIR(){ return g_ir; }
  void nextSample(){ if (pending > 0) --pending; }
};
