#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
#define WIFI_STA 1
extern int g_wifi;
struct WiFiStub { int status(){return g_wifi;} void mode(int){} void begin(const char*, const char*){} void disconnect(){} };
inline WiFiStub WiFi;
