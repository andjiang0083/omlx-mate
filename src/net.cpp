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
static bool s_enabled = true;   // false = 射频已关（熄屏），netPoll 直接返回

void netBegin(const Config& cfg) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(WIFI_MODEM_SLEEP);   // StickS3: 射频按 DTIM 睡（推 5s 一次，延迟无感）
  WiFi.setHostname(g_hostname);
  if (cfg.ssid[0]) {
    WiFi.begin(cfg.ssid, cfg.pass);
    s_startedAt = millis();
    Serial.printf("[NET] connecting %s\n", cfg.ssid);
  }
}

// 熄屏 = 主动下线：射频关掉后连 WiFi.status() 都不再轮询，避免任何唤醒电流
void netOff() {
  if (!s_enabled) return;
  s_enabled = false;
  s_up = false;
  s_justUp = false;
  s_startedAt = 0;
  s_retryAt = 0;
  if (s_mdns) {
    MDNS.end();
    s_mdns = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[NET] radio off");
}

void netOn() {
  if (s_enabled) return;
  s_enabled = true;
  s_up = false;
  s_retryAt = 0;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(g_hostname);
  if (g_cfg.ssid[0]) {
    WiFi.begin(g_cfg.ssid, g_cfg.pass);
    s_startedAt = millis();
    Serial.printf("[NET] radio on, connecting %s\n", g_cfg.ssid);
  }
}

void netPoll() {
  if (!s_enabled) return;
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
