#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

// 设备端显示配置由 Mac 端下发 (全局生效). 设备只负责按配置渲染.
enum PageType : uint8_t {
  PT_SAVINGS = 0,
  PT_USAGE,
  PT_TREND,
  PT_MODELS,
  PT_COST,
  PT_DEVICE,
};

enum RangeSel : uint8_t { RS_TODAY = 0, RS_YESTERDAY, RS_D7, RS_D30 };
enum ChartSel : uint8_t { CS_D7 = 0, CS_D30, CS_HOURLY, CS_NONE };

struct DisplayPage {
  uint8_t type;
  uint8_t range;
  uint8_t chart;
  bool dense;
};

struct DisplayConfig {
  uint8_t version;
  uint8_t count;
  DisplayPage pages[8];
};

const char* pageTypeKey(uint8_t t);
uint8_t pageTypeFromKey(const char* s);
const char* rangeKey(uint8_t r);
uint8_t rangeFromKey(const char* s);
const char* chartKey(uint8_t c);
uint8_t chartFromKey(const char* s);

DisplayConfig displayDefault();
bool displayFromDoc(JsonDocument& doc, DisplayConfig& out);
bool displayEquals(const DisplayConfig& a, const DisplayConfig& b);

// NVS 持久化 (原始 JSON, 重启后先按上次配置渲染)
bool displayLoad(DisplayConfig& cfg);
void displaySave(const DisplayConfig& cfg);
