#pragma once
#include <Arduino.h>
extern std::string g_lcd[2];
struct LiquidCrystal_I2C { uint8_t r=0; LiquidCrystal_I2C(uint8_t,uint8_t,uint8_t){} void init(){} void backlight(){} void clear(){} void setCursor(uint8_t,uint8_t row){r=row;} size_t print(const char* t){g_lcd[r]=t;return strlen(t);} };
