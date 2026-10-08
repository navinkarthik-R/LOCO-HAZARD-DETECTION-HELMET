#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
extern std::vector<std::string> g_calls; extern int g_http;
struct String { std::string s; String(const char* c):s(c){} };
struct HTTPClient { std::string u; void setTimeout(uint16_t){} bool begin(WiFiClient&, String s){u=s.s;return true;}
  int GET(){ size_t b=u.find('?'); size_t c=u.rfind('/',b); g_calls.push_back(std::to_string(g_now)+" "+u.substr(c+1,b-c-1)+" "+std::to_string(g_http)); return g_http; } void end(){} };
