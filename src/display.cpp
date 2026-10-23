#include "display.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <cstring>

static const char* TYPE_KEYS[] = {"savings", "usage", "trend", "models", "cost", "device"};
static const char* RANGE_KEYS[] = {"today", "yesterday", "d7", "d30"};
static const char* CHART_KEYS[] = {"d7", "d30", "hourly", "none"};

const char* pageTypeKey(uint8_t t) { return t < 6 ? TYPE_KEYS[t] : "savings"; }
uint8_t pageTypeFromKey(const char* s) {
  for (uint8_t i = 0; i < 6; i++)
    if (!std::strcmp(s, TYPE_KEYS[i])) return i;
  return PT_SAVINGS;
}
const char* rangeKey(uint8_t r) { return r < 4 ? RANGE_KEYS[r] : "today"; }
uint8_t rangeFromKey(const char* s) {
  for (uint8_t i = 0; i < 4; i++)
    if (!std::strcmp(s, RANGE_KEYS[i])) return i;
  return RS_TODAY;
}
const char* chartKey(uint8_t c) { return c < 4 ? CHART_KEYS[c] : "none"; }
uint8_t chartFromKey(const char* s) {
  for (uint8_t i = 0; i < 4; i++)
    if (!std::strcmp(s, CHART_KEYS[i])) return i;
  return CS_NONE;
}

DisplayConfig displayDefault() {
  DisplayConfig c{};
  c.version = 1;
  c.count = 4;
  c.pages[0] = {PT_SAVINGS, RS_TODAY, CS_D7, false};
  c.pages[1] = {PT_USAGE, RS_TODAY, CS_HOURLY, false};
  c.pages[2] = {PT_TREND, RS_D7, CS_D30, false};
  c.pages[3] = {PT_MODELS, RS_D7, CS_NONE, false};
  return c;
}

bool displayFromDoc(JsonDocument& doc, DisplayConfig& out) {
  JsonVariantConst root = doc["display"].isNull() ? doc.as<JsonVariantConst>()
                                                 : doc["display"].as<JsonVariantConst>();
  JsonArrayConst arr = root["pages"].as<JsonArrayConst>();
  if (arr.isNull()) return false;
  DisplayConfig c{};
  c.version = root["version"] | 1;
  for (JsonVariantConst p : arr) {
    if (c.count >= 8) break;
    const char* t = p["type"] | "savings";
    DisplayPage& pg = c.pages[c.count++];
    pg.type = pageTypeFromKey(t);
    pg.range = rangeFromKey(p["range"] | "today");
    pg.chart = chartFromKey(p["chart"] | "none");
    pg.dense = p["dense"] | false;
  }
  if (c.count == 0) return false;
  out = c;
  return true;
}

bool displayLoad(DisplayConfig& cfg) {
  Preferences p;
  if (!p.begin("omate", true)) return false;
  String s = p.getString("display", "");
  p.end();
  if (!s.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, s)) return false;
  return displayFromDoc(doc, cfg);
}

void displaySave(const DisplayConfig& cfg) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["version"] = cfg.version;
  JsonArray pages = root["pages"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg.count; i++) {
    JsonObject p = pages.add<JsonObject>();
    p["type"] = pageTypeKey(cfg.pages[i].type);
    p["range"] = rangeKey(cfg.pages[i].range);
    p["chart"] = chartKey(cfg.pages[i].chart);
    p["dense"] = cfg.pages[i].dense;
  }
  String s;
  serializeJson(doc, s);
  Preferences p;
  if (p.begin("omate", false)) {
    p.putString("display", s);
    p.end();
  }
}

bool displayEquals(const DisplayConfig& a, const DisplayConfig& b) {
  if (a.count != b.count || a.version != b.version) return false;
  for (uint8_t i = 0; i < a.count; i++) {
    const DisplayPage& x = a.pages[i];
    const DisplayPage& y = b.pages[i];
    if (x.type != y.type || x.range != y.range || x.chart != y.chart || x.dense != y.dense)
      return false;
  }
  return true;
}
