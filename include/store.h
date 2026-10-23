#pragma once
#include <Arduino.h>

// 设备端持久化配置: 只保留 WiFi、亮度、语言、配对 token.
// 计价参数与主机地址都在 Mac 端, 设备不需要知道.
struct Config {
  char ssid[33];
  char pass[65];
  uint8_t brightness;
  uint8_t lang;    // 0=中文 1=English
  char token[17];  // 配对 token, 首次启动生成

  bool configured() const { return ssid[0] != '\0'; }
};

void configLoad(Config& cfg);
bool configSave(const Config& cfg);
void configClear();
void configEnsureToken(Config& cfg);
