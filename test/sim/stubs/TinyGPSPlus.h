#pragma once
#include <Arduino.h>
extern bool g_fix;
struct TinyGPSLocation { bool isValid() const{return g_fix;} uint32_t age() const{return 100;} double lat(){return 12.9716;} double lng(){return 77.5946;} };
struct TinyGPSInteger { uint32_t value(){return g_fix?7:0;} };
struct TinyGPSPlus { TinyGPSLocation location; TinyGPSInteger satellites; bool encode(char){return true;} uint32_t charsProcessed() const{return g_fix?500:0;} };
