// Runs the real firmware sketch on a PC against fake Arduino libraries (see stubs/)
// with a fake clock, sensor and network, and checks the alert/buzzer/LED behaviour.
//   g++ -std=gnu++17 -Wall -Wextra -x c++ -I test/sim/stubs -I firmware/loco_hazard_helmet test/sim/sim.cpp -o /tmp/sim && /tmp/sim
#include <Arduino.h>
uint32_t g_now=0; int g_pin[64]; bool g_quiet=true; float g_temp=25,g_hum=50; int g_wifi=3,g_http=200; bool g_fix=false;
std::vector<std::string> g_calls; std::string g_lcd[2];
#include "../../firmware/loco_hazard_helmet/loco_hazard_helmet.ino"
void tick(uint32_t ms){ for(uint32_t t=0;t<ms;t+=100){ g_now+=100; loop(); } }
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); ++fails; } }while(0)
static int count(const char* m){int n=0; for(auto&c:g_calls) if(c.find(m)!=std::string::npos) ++n; return n;}
int main(){
  setup();
  // 1. normal
  tick(5000);
  CHECK(g_pin[26]==HIGH && g_pin[25]==LOW);            // green
  CHECK(g_pin[19]==LOW); CHECK(g_calls.empty());
  // 2. hazard
  g_temp=31; uint32_t t0=g_now; tick(3000);
  CHECK(g_pin[25]==HIGH && g_pin[26]==LOW);            // red
  CHECK(count("sendMessage")==1); CHECK(count("sendLocation")==0);  // no fix -> text only
  CHECK(g_pin[19]==HIGH);                              // buzzer on
  CHECK(g_lcd[0].find("HAZARD")!=std::string::npos);
  tick(14000); CHECK(g_pin[19]==LOW);                  // ~15 s later buzzer off
  CHECK(g_pin[25]==HIGH);                              // still red
  // 3. no spam while hazard persists, repeat after 5 min
  tick(200000); CHECK(count("sendMessage")==1);
  tick(95000); CHECK(count("sendMessage")==2); CHECK(g_pin[19]==HIGH);
  // 4. hysteresis then clear
  g_temp=29.5; tick(4000); CHECK(g_pin[25]==HIGH);     // still latched
  g_temp=28.9; tick(4000); CHECK(g_pin[26]==HIGH && g_pin[25]==LOW && g_pin[19]==LOW);
  // 5. failing network: retries every 10 s, stops after success; with GPS fix also sends location
  g_calls.clear(); g_http=-1; g_fix=true; g_temp=33; tick(35000);
  int tries=count("sendMessage"); CHECK(tries>=3 && tries<=5);
  g_http=200; tick(12000); int after=count("sendMessage"); tick(30000); CHECK(count("sendMessage")==after);
  CHECK(count("sendLocation")==1);
  // 6. Wi-Fi down: local alarm still works, alert queued then delivered on reconnect
  g_temp=20; tick(4000); g_calls.clear(); g_wifi=6; g_temp=40; tick(4000);
  CHECK(g_pin[25]==HIGH && g_pin[19]==HIGH); CHECK(g_calls.empty());
  g_wifi=3; tick(12000); CHECK(count("sendMessage")==1);
  // 7. sensor failure -> blue, no new alert
  g_temp=20; tick(4000); g_calls.clear(); g_temp=NAN; tick(10000);
  CHECK(g_pin[27]==HIGH && g_pin[25]==LOW); CHECK(g_calls.empty());
  CHECK(g_lcd[0].find("Sensor Error")!=std::string::npos);
  g_temp=22; tick(4000); CHECK(g_pin[26]==HIGH);       // recovers
  (void)t0;
  puts(fails? "SIMULATION FAILED":"simulation passed"); return fails?1:0;
}
