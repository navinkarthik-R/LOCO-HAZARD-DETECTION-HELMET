#pragma once
#include <Arduino.h>
#define DHT11 11
extern float g_temp, g_hum;
struct DHT { DHT(uint8_t,uint8_t){} void begin(){} float readTemperature(){return g_temp;} float readHumidity(){return g_hum;} };
