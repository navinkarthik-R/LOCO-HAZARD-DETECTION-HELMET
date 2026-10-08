#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <string>
#include <vector>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define SERIAL_8N1 0
extern uint32_t g_now; extern int g_pin[64]; extern bool g_quiet;
inline uint32_t millis(){return g_now;}
void tick(uint32_t ms);                       // advances time, runs loop()
inline void delay(uint32_t ms){g_now+=ms;}
inline void pinMode(uint8_t,uint8_t){}
extern int g_gas;
inline int analogRead(uint8_t){return g_gas;}
inline void digitalWrite(uint8_t p,uint8_t v){g_pin[p]=v;}
struct SerialStub { void begin(unsigned long){} void println(const char*){} int printf(const char*,...) __attribute__((format(printf,2,3))){return 0;} };
inline SerialStub Serial;
struct HardwareSerial { HardwareSerial(int){} void begin(unsigned long,uint32_t,int8_t,int8_t){} void setRxBufferSize(size_t){} int available(){return 0;} int read(){return 0;} };
