#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
extern std::vector<std::string> g_calls; extern std::vector<std::string> g_bodies; extern int g_http;
struct String { std::string s; String(const char* c):s(c){} };
struct HTTPClient {
  std::string u;
  void setTimeout(uint16_t){}
  void addHeader(const char*, const char*){}
  bool begin(WiFiClient&, String s){u=s.s;return true;}
  int POST(String b){
    size_t c=u.rfind('/');
    g_calls.push_back(std::to_string(g_now)+" "+u.substr(c+1)+" "+std::to_string(g_http));
    g_bodies.push_back(b.s);
    return g_http;
  }
  void end(){}
};
