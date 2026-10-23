#pragma once
#include <stdint.h>

// 一次推送里单个区间的数字 (agent 的 metrics.{today,y,d7,d30})
struct RangeData {
  bool valid = false;
  float net = 0;   // 净省 = API 账单 - 电费
  float api = 0;   // 折算的 DeepSeek 账单
  float elec = 0;  // 本地电费
  float cs = 0;    // 缓存命中省下的钱
  float pk = 0;    // 其中峰时段账单
  float hit = 0;   // 缓存命中率 0..1
  uint64_t tok = 0;
  uint32_t req = 0;
};

struct ModelRow {
  char name[28] = "";
  float cost = 0;
  uint64_t tok = 0;
  uint32_t req = 0;
};

struct Snapshot {
  bool ok = false;
  uint32_t ts = 0;
  char srv[10] = "";
  bool peak = false;
  RangeData today, y, d7, d30;
  float series7[7] = {};
  float series30[30] = {};
  float hourly[24] = {};
  ModelRow models[5];
  uint8_t modelCount = 0;
  bool omlxUp = false;
  char omlxVer[12] = "";
  char omlxModel[28] = "";
  char err[72] = "";
  uint32_t receivedAtMs = 0;  // 本机 millis(), 用于判断数据新鲜度
};

// 按配置取区间
const RangeData& snapshotRange(const Snapshot& s, uint8_t range);
