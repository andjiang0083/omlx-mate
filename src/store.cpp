#include "store.h"

#include <Preferences.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

#include "config.h"

static const char* NVS_NAMESPACE = "omate";

void configLoad(Config& cfg) {
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.brightness = DEFAULT_BRIGHTNESS;
  cfg.lang = 0;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return;
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  String token = p.getString("token", "");
  cfg.brightness = p.getUChar("bright", DEFAULT_BRIGHTNESS);
  cfg.lang = p.getUChar("lang", 0);
  p.end();

  std::strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
  std::strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
  std::strncpy(cfg.token, token.c_str(), sizeof(cfg.token) - 1);
  if (cfg.brightness < 10) cfg.brightness = DEFAULT_BRIGHTNESS;
  if (cfg.lang > 1) cfg.lang = 0;
}

bool configSave(const Config& cfg) {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return false;
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.pass);
  p.putString("token", cfg.token);
  p.putUChar("bright", cfg.brightness);
  p.putUChar("lang", cfg.lang);
  p.end();
  return true;
}

void configClear() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.clear();
    p.end();
  }
}

void configEnsureToken(Config& cfg) {
  if (cfg.token[0]) return;
  char buf[17];
  for (int i = 0; i < 3; i++) {
    std::snprintf(buf + i * 4, 5, "%04x", static_cast<unsigned>(esp_random() & 0xFFFF));
  }
  buf[12] = '\0';
  std::strncpy(cfg.token, buf, sizeof(cfg.token) - 1);
  configSave(cfg);
}
