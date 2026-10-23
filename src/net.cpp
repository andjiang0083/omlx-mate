#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include <cstring>

#include "app.h"
#include "config.h"

static bool s_up = false;
static bool s_justUp = false;
static uint32_t s_startedAt = 0;
static uint32_t s_retryAt = 0;
static char s_ip[16] = "--";
static bool s_mdns = false;

void netBegin(const Config& cfg) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.setHostname(g_hostname);
  if (cfg.ssid[0]) {
    WiFi.begin(cfg.ssid, cfg.pass);
    s_startedAt = millis();
    Serial.printf("[NET] connecting %s\n", cfg.ssid);
  }
}

void netPoll() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_up) {
      s_up = true;
      s_justUp = true;
      String ip = WiFi.localIP().toString();
      std::strncpy(s_ip, ip.c_str(), sizeof(s_ip) - 1);
      s_ip[sizeof(s_ip) - 1] = '\0';
      Serial.printf("[NET] up ip=%s host=%s.local rssi=%d\n", s_ip, g_hostname, WiFi.RSSI());
      if (!s_mdns) {
        s_mdns = MDNS.begin(g_hostname);
        if (s_mdns) MDNS.addService("http", "_tcp", 80);
        Serial.printf("[NET] mdns=%d\n", s_mdns);
      }
    }
    return;
  }

  if (s_up) {
    s_up = false;
    s_retryAt = millis() + 3000;
    Serial.println("[NET] link lost");
  }
  if (s_startedAt && millis() - s_startedAt > WIFI_TIMEOUT_MS) {
    s_startedAt = 0;
    s_retryAt = millis() + 8000;
  }
  if (s_retryAt && millis() >= s_retryAt) {
    s_retryAt = 0;
    WiFi.disconnect();
    WiFi.begin();
    s_startedAt = millis();
  }
}

bool netIsUp() { return s_up; }
int netRssi() { return s_up ? WiFi.RSSI() : 0; }
const char* netIp() { return s_ip; }

bool netTakeConnectedEvent() {
  bool v = s_justUp;
  s_justUp = false;
  return v;
}
