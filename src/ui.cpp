#include "ui.h"

#include <M5Unified.h>
#include <qrcode.h>

#include "board.h"   // 底部导航提示随板型切换

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- 基础
static M5Canvas canvas(&M5.Display);
static bool s_ready = false;
static bool EN = false;

static const lgfx::IFont* F12 = &fonts::efontCN_12;
static const lgfx::IFont* F12B = &fonts::efontCN_12_b;
static const lgfx::IFont* F16B = &fonts::efontCN_16_b;
static const lgfx::IFont* HERO = &fonts::FreeSansBold24pt7b;
static const lgfx::IFont* HERO_S = &fonts::FreeSansBold18pt7b;

#define T(zh, en) (EN ? (en) : (zh))

static void tl(int x, int y, const char* s, uint16_t c, const lgfx::IFont* f) {
  canvas.setFont(f);
  canvas.setTextColor(c);
  canvas.setTextDatum(top_left);
  canvas.drawString(s, x, y);
}

static void tr(int x, int y, const char* s, uint16_t c, const lgfx::IFont* f) {
  canvas.setFont(f);
  canvas.setTextColor(c);
  canvas.setTextDatum(top_right);
  canvas.drawString(s, x, y);
}

static void tc(int x, int y, const char* s, uint16_t c, const lgfx::IFont* f) {
  canvas.setFont(f);
  canvas.setTextColor(c);
  canvas.setTextDatum(middle_center);
  canvas.drawString(s, x, y);
}

static void fitText(char* out, size_t n, const char* in, int maxW, const lgfx::IFont* f) {
  canvas.setFont(f);
  if (canvas.textWidth(in) <= maxW) {
    std::strncpy(out, in, n - 1);
    out[n - 1] = '\0';
    return;
  }
  std::strncpy(out, in, n - 1);
  out[n - 1] = '\0';
  size_t len = std::strlen(out);
  while (len > 2) {
    out[len - 1] = '\0';
    len--;
    if (canvas.textWidth(out) <= maxW - 10) break;
  }
  std::strncat(out, "..", n - std::strlen(out) - 1);
}

static void fmtMoney(char* out, size_t n, float v) {
  float a = v < 0 ? -v : v;
  if (a >= 10000.0f) {
    std::snprintf(out, n, "%.1fk", v / 1000.0f);
  } else {
    std::snprintf(out, n, "%.2f", v);
  }
}

static void fmtTokens(char* out, size_t n, uint64_t t) {
  if (t >= 1000000000ULL) {
    std::snprintf(out, n, "%.2fB", static_cast<double>(t) / 1e9);
  } else if (t >= 1000000ULL) {
    std::snprintf(out, n, "%.1fM", static_cast<double>(t) / 1e6);
  } else if (t >= 1000ULL) {
    std::snprintf(out, n, "%.0fK", static_cast<double>(t) / 1e3);
  } else {
    std::snprintf(out, n, "%llu", static_cast<unsigned long long>(t));
  }
}

// ---------------------------------------------------------------- 组件
static void drawWifi(int x, int y, bool up, int rssi) {
  int q = 0;
  if (up) {
    if (rssi > -55) q = 4;
    else if (rssi > -65) q = 3;
    else if (rssi > -75) q = 2;
    else q = 1;
  }
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    canvas.fillRect(x + i * 4, y + 11 - h, 3, h, (i < q) ? C_TEAL2 : C_BORDER);
  }
}

static void drawBattery(int x, int y, int pct, bool charging) {
  uint16_t c = charging ? C_AMBER : (pct <= 20 ? C_RED : C_TEXT2);
  canvas.drawRoundRect(x, y, 16, 9, 2, c);
  canvas.fillRect(x + 16, y + 3, 2, 3, c);
  int w = (14 * (pct < 0 ? 0 : (pct > 100 ? 100 : pct))) / 100;
  if (w > 0) canvas.fillRect(x + 1, y + 1, w, 7, c);
}

static void drawBars(int x, int y, int w, int h, const float* v, int n, uint16_t color) {
  float mx = 0;
  for (int i = 0; i < n; i++)
    if (v[i] > mx) mx = v[i];
  if (mx <= 0) mx = 1;
  int gap = n > 15 ? 1 : 4;
  int bw = (w - gap * (n - 1)) / n;
  if (bw < 1) bw = 1;
  for (int i = 0; i < n; i++) {
    int bh = static_cast<int>(v[i] / mx * (h - 1));
    if (v[i] > 0 && bh < 2) bh = 2;
    if (bh <= 0) continue;
    canvas.fillRect(x + i * (bw + gap), y + h - bh, bw, bh, (i == n - 1) ? color : C_TEAL_DIM);
  }
  canvas.drawFastHLine(x, y + h, w, C_BORDER);
}

static void drawChip(int x, int y, const char* text, uint16_t bg, uint16_t fg) {
  canvas.setFont(F12B);
  int w = canvas.textWidth(text) + 18;
  canvas.fillRoundRect(x, y, w, 17, 8, bg);
  tc(x + w / 2, y + 9, text, fg, F12B);
}

static void drawChart(const Snapshot& s, uint8_t chart, int y, int h) {
  const float* arr = nullptr;
  int n = 0;
  if (chart == CS_D7) {
    arr = s.series7;
    n = 7;
  } else if (chart == CS_D30) {
    arr = s.series30;
    n = 30;
  } else if (chart == CS_HOURLY) {
    arr = s.hourly;
    n = 24;
  }
  if (!arr) return;
  drawBars(10, y, 220, h, arr, n, C_TEAL2);
}

// efont 里没有 ¥ 字形, 用线条画一个, 比豆腐块可靠
static void drawYen(int x, int y, int h, uint16_t color) {
  int w = h * 5 / 8;
  int mid = x + w / 2;
  canvas.drawLine(x, y, mid, y + h / 2, color);
  canvas.drawLine(x + w, y, mid, y + h / 2, color);
  canvas.drawLine(mid, y + h / 2, mid, y + h, color);
  canvas.drawLine(x, y + h / 2 + 1, x + w, y + h / 2 + 1, color);
  canvas.drawLine(x, y + h - 3, x + w, y + h - 3, color);
}

static void drawHeroMoney(const char* amount, uint16_t color, bool dense) {
  const lgfx::IFont* f = dense ? HERO_S : HERO;
  canvas.setFont(f);
  int aw = canvas.textWidth(amount);
  if (aw > 200) {
    f = HERO_S;
    canvas.setFont(f);
    aw = canvas.textWidth(amount);
  }
  const int yenW = 10, gap = 4;
  int total = yenW + gap + aw;
  int x = (SCREEN_W - total) / 2;
  if (x < 8) x = 8;
  int top = dense ? 40 : 46;
  drawYen(x, top, 16, color);
  canvas.setFont(f);
  canvas.setTextColor(color);
  canvas.setTextDatum(top_left);
  canvas.drawString(amount, x + yenW + gap, 30);
}

static void drawHeroText(const char* text, uint16_t color) {
  canvas.setFont(HERO);
  const lgfx::IFont* f = HERO;
  int w = canvas.textWidth(text);
  if (w > 220) {
    f = HERO_S;
    canvas.setFont(f);
    w = canvas.textWidth(text);
  }
  int x = (SCREEN_W - w) / 2;
  if (x < 6) x = 6;
  canvas.setFont(f);
  canvas.setTextColor(color);
  canvas.setTextDatum(top_left);
  canvas.drawString(text, x, 30);
}

// ---------------------------------------------------------------- 框架
static void drawChrome(const Snapshot& s, const UiState& st) {
  canvas.fillRect(0, 0, SCREEN_W, 18, C_SURF);
  canvas.drawFastHLine(0, 17, SCREEN_W, C_BORDER);

  uint16_t dot = !st.wifiUp ? C_RED : (st.stale ? C_AMBER : C_TEAL2);
  canvas.fillCircle(9, 9, 3.5f, dot);
  tl(16, 2, "oMLX Mate", C_TEXT2, F12B);

  const char* chip;
  uint16_t bg, fg = C_BG;
  if (!st.wifiUp) {
    chip = T("离线", "OFFLINE");
    bg = C_RED;
    fg = C_WHITE;
  } else if (s.receivedAtMs == 0) {
    chip = T("等待", "WAIT");
    bg = C_AMBER;
  } else if (st.stale) {
    chip = T("延迟", "LAG");
    bg = C_AMBER;
  } else if (s.peak) {
    chip = T("峰时", "PEAK");
    bg = C_AMBER;
  } else {
    chip = T("谷时", "OFF");
    bg = C_TEAL;
  }
  drawChip(94, 0, chip, bg, fg);

  char clock[6];
  std::strncpy(clock, s.srv[0] ? s.srv : "--:--", 5);
  clock[5] = '\0';
  tr(194, 3, clock, C_TEXT2, F12);
  drawWifi(200, 4, st.wifiUp, st.rssi);
  drawBattery(220, 5, st.battery, st.charging);
}

static void drawFooter(const DisplayConfig& dc, const UiState& st) {
  canvas.drawFastHLine(0, 121, SCREEN_W, C_BORDER);
  uint8_t n = dc.count ? dc.count : 1;
  for (uint8_t i = 0; i < n; i++) {
    int cx = SCREEN_W / 2 + (i - (n - 1) / 2) * 9;
    if (i == st.page) canvas.fillCircle(cx, 128, 2.5f, C_TEAL2);
    else canvas.drawCircle(cx, 128, 2, C_BORDER);
  }
  tl(6, 123, BOARD_NAV_LEFT, C_DIM, F12);
  tr(234, 123, BOARD_NAV_RIGHT, C_DIM, F12);
}

// ---------------------------------------------------------------- 页面
static const char* rangeTitleSavings(uint8_t r) {
  switch (r) {
    case RS_YESTERDAY: return T("昨日已省", "Saved yesterday");
    case RS_D7: return T("近 7 天已省", "Saved · 7d");
    case RS_D30: return T("近 30 天已省", "Saved · 30d");
    default: return T("今日已省", "Saved today");
  }
}

static void pageSavings(const Snapshot& s, const DisplayPage& p) {
  const RangeData& r = snapshotRange(s, p.range);
  tl(10, 20, rangeTitleSavings(p.range), C_TEXT2, F12B);

  char amount[24];
  if (r.valid) fmtMoney(amount, sizeof(amount), r.net);
  else std::snprintf(amount, sizeof(amount), "--");
  drawHeroMoney(amount, (r.valid && r.net < 0) ? C_RED : C_TEAL2, p.dense);

  int div = p.dense ? 64 : 70;
  canvas.drawFastHLine(10, div, 220, C_BORDER);

  if (!r.valid) {
    tl(10, div + 5, T("等待 Mac 推送…", "Waiting for Mac…"), C_MUTED, F12);
    return;
  }
  char line[96];
  int y = p.dense ? 67 : 74;
  std::snprintf(line, sizeof(line), T("API %.2f   ·   电费 %.2f", "API %.2f   ·   power %.2f"),
                static_cast<double>(r.api), static_cast<double>(r.elec));
  tl(10, y, line, C_TEXT2, F12);

  char tok[16];
  fmtTokens(tok, sizeof(tok), r.tok);
  std::snprintf(line, sizeof(line), T("%s tok · %u 次 · 命中 %.1f%%", "%s tok · %u req · hit %.1f%%"),
                tok, r.req, static_cast<double>(r.hit * 100.0f));
  tl(10, y + 15, line, C_MUTED, F12);

  if (p.dense) {
    std::snprintf(line, sizeof(line), T("缓存省 %.2f", "cache saved %.2f"),
                  static_cast<double>(r.cs));
    tl(10, y + 30, line, C_DIM, F12);
  }
  drawChart(s, p.chart, p.dense ? 110 : 103, p.dense ? 8 : 15);
}

static void pageUsage(const Snapshot& s, const DisplayPage& p) {
  const RangeData& r = snapshotRange(s, p.range);
  const char* title = p.range == RS_D30 ? T("近 30 天用量", "Usage · 30d")
                    : p.range == RS_D7 ? T("近 7 天用量", "Usage · 7d")
                    : p.range == RS_YESTERDAY ? T("昨日用量", "Usage yesterday")
                    : T("今日用量", "Usage today");
  tl(10, 20, title, C_TEXT2, F12B);

  char buf[24];
  if (r.valid) fmtTokens(buf, sizeof(buf), r.tok);
  else std::snprintf(buf, sizeof(buf), "--");
  drawHeroText(buf, C_TEAL2);

  int div = p.dense ? 64 : 70;
  canvas.drawFastHLine(10, div, 220, C_BORDER);
  if (!r.valid) {
    tl(10, div + 5, T("等待 Mac 推送…", "Waiting for Mac…"), C_MUTED, F12);
    return;
  }
  int y = p.dense ? 67 : 74;
  char line[96];
  std::snprintf(line, sizeof(line), T("%u 次请求  ·  命中 %.1f%%", "%u req  ·  hit %.1f%%"),
                r.req, static_cast<double>(r.hit * 100.0f));
  tl(10, y, line, C_TEXT2, F12);

  char model[30];
  fitText(model, sizeof(model), s.omlxModel[0] ? s.omlxModel : T("未加载模型", "no model"), 220, F12);
  tl(10, y + 14, model, C_MUTED, F12);

  if (p.dense) {
    std::snprintf(line, sizeof(line), T("缓存省 %.2f", "cache saved %.2f"),
                  static_cast<double>(r.cs));
    tl(10, y + 30, line, C_DIM, F12);
  }
  drawChart(s, p.chart, p.dense ? 110 : 103, p.dense ? 8 : 15);
}

static void pageTrend(const Snapshot& s, const DisplayPage& p) {
  const RangeData& r = snapshotRange(s, p.range);
  tl(10, 20, rangeTitleSavings(p.range), C_TEXT2, F12B);

  char amount[24];
  if (r.valid) fmtMoney(amount, sizeof(amount), r.net);
  else std::snprintf(amount, sizeof(amount), "--");
  drawHeroMoney(amount, (r.valid && r.net < 0) ? C_RED : C_TEAL2, p.dense);

  int div = p.dense ? 64 : 70;
  canvas.drawFastHLine(10, div, 220, C_BORDER);
  if (!r.valid) {
    tl(10, div + 5, T("等待 Mac 推送…", "Waiting for Mac…"), C_MUTED, F12);
    return;
  }
  int y = p.dense ? 67 : 74;
  float days = p.range == RS_D30 ? 30.0f : (p.range == RS_D7 ? 7.0f : 1.0f);
  char line[96];
  std::snprintf(line, sizeof(line), T("日均 %.2f  ·  %u 次", "avg %.2f  ·  %u req"),
                static_cast<double>(r.net / days), r.req);
  tl(10, y, line, C_TEXT2, F12);

  int peakPct = r.api > 0 ? static_cast<int>(r.pk / r.api * 100.0f) : 0;
  std::snprintf(line, sizeof(line), T("峰时 %d%%  ·  命中 %.1f%%", "peak %d%%  ·  hit %.1f%%"),
                peakPct, static_cast<double>(r.hit * 100.0f));
  tl(10, y + 15, line, C_MUTED, F12);

  if (p.dense) {
    char tok[16];
    fmtTokens(tok, sizeof(tok), r.tok);
    std::snprintf(line, sizeof(line), "%s tok", tok);
    tl(10, y + 30, line, C_DIM, F12);
  }
  drawChart(s, p.chart, p.dense ? 110 : 103, p.dense ? 8 : 15);
}

static void pageModels(const Snapshot& s, const DisplayPage& p) {
  tl(10, 20, T("模型用量 · 近 7 天", "Models · 7d"), C_TEXT2, F12B);
  if (s.modelCount == 0) {
    tc(SCREEN_W / 2, 72, T("暂无模型数据", "No model data"), C_MUTED, F16B);
    return;
  }
  float maxCost = s.models[0].cost > 0 ? s.models[0].cost : 1.0f;
  int rows = s.modelCount > 4 ? 4 : s.modelCount;
  for (int i = 0; i < rows; i++) {
    const ModelRow& m = s.models[i];
    int y = 38 + i * 20;
    char name[30];
    fitText(name, sizeof(name), m.name, 148, F12B);
    tl(10, y, name, C_TEXT, F12B);
    char m2[16];
    fmtMoney(m2, sizeof(m2), m.cost);
    canvas.setFont(F12B);
    int mw = canvas.textWidth(m2);
    drawYen(230 - mw - 10, y + 2, 10, C_TEAL2);
    tr(230, y, m2, C_TEAL2, F12B);

    canvas.fillRoundRect(10, y + 15, 220, 3, 1, C_SURF2);
    int w = static_cast<int>(220.0f * (m.cost / maxCost));
    if (w < 2 && m.cost > 0) w = 2;
    if (w > 0) canvas.fillRoundRect(10, y + 15, w, 3, 1, C_TEAL);
  }
  (void)p;
}

static void pageCost(const Snapshot& s, const DisplayPage& p) {
  const RangeData& r = snapshotRange(s, p.range);
  const char* title = p.range == RS_D30 ? T("成本构成 · 近 30 天", "Cost · 30d")
                    : p.range == RS_D7 ? T("成本构成 · 近 7 天", "Cost · 7d")
                    : p.range == RS_YESTERDAY ? T("成本构成 · 昨日", "Cost · yesterday")
                    : T("成本构成 · 今日", "Cost · today");
  tl(10, 20, title, C_TEXT2, F12B);

  const char* labels[4] = {T("云账单", "API bill"), T("电费", "Power"),
                           T("缓存省", "Cache"), T("净省", "Net")};
  float vals[4] = {r.api, r.elec, r.cs, r.net};
  for (int i = 0; i < 4; i++) {
    int cx = 10 + (i % 2) * 116;
    int cy = 38 + (i / 2) * 36;
    canvas.fillRoundRect(cx, cy, 106, 32, 6, C_SURF);
    canvas.drawRoundRect(cx, cy, 106, 32, 6, C_BORDER);
    tl(cx + 8, cy + 3, labels[i], C_MUTED, F12);
    char m[16];
    fmtMoney(m, sizeof(m), vals[i]);
    uint16_t vc = i == 3 ? C_TEAL2 : C_TEXT;
    drawYen(cx + 8, cy + 17, 12, vc);
    tl(cx + 22, cy + 14, m, vc, F16B);
  }
  int peakPct = r.api > 0 ? static_cast<int>(r.pk / r.api * 100.0f) : 0;
  char line[80];
  std::snprintf(line, sizeof(line), T("峰时占比 %d%%  ·  命中 %.1f%%", "peak %d%%  ·  hit %.1f%%"),
                peakPct, static_cast<double>(r.hit * 100.0f));
  tl(10, 108, line, C_MUTED, F12);
}

static void pageDevice(const Snapshot& s, const UiState& st) {
  tl(10, 20, T("设备与链路", "Device & link"), C_TEXT2, F12B);

  auto row = [](int y, const char* label, const char* value, uint16_t vc) {
    tl(10, y, label, C_MUTED, F12);
    char v[44];
    fitText(v, sizeof(v), value, 150, F12);
    tr(230, y, v, vc, F12);
  };

  char ssid[34];
  fitText(ssid, sizeof(ssid), st.ssid[0] ? st.ssid : "(unset)", 150, F12);
  row(38, "WiFi", ssid, C_TEXT);

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s  %d dBm", st.ip, st.rssi);
  row(54, T("地址", "IP"), buf, st.wifiUp ? C_TEXT : C_RED);

  std::snprintf(buf, sizeof(buf), "v%s · %s", s.omlxVer[0] ? s.omlxVer : "?",
                s.omlxUp ? T("在线", "online") : T("离线", "offline"));
  row(70, "oMLX", buf, s.omlxUp ? C_TEXT : C_RED);

  char model[30];
  fitText(model, sizeof(model), s.omlxModel[0] ? s.omlxModel : T("未加载", "none"), 150, F12);
  row(86, T("模型", "Model"), model, C_TEXT2);

  std::snprintf(buf, sizeof(buf), "%d%%%s · v%s", st.battery,
                st.charging ? T(" 充电", " chg") : "", FW_VERSION);
  row(102, T("电量 / 固件", "Battery / FW"), buf, C_TEXT2);
}

static void pageSetup(const Snapshot& s, const UiState& st) {
  tl(10, 20, T("连接 Mac", "Connect Mac"), C_TEXT2, F12B);

  if (!st.wifiUp) {
    tl(10, 42, T("设备还没有连上 Wi-Fi", "Device is offline"), C_TEXT, F12);
    tl(10, 60, T("长按 B 打开配网页面", "hold B to set up Wi-Fi"), C_MUTED, F12);
    (void)s;
    return;
  }

  tl(10, 40, T("在 Mac 上打开这个地址：", "Open this on your Mac:"), C_MUTED, F12);
  char url[44];
  std::snprintf(url, sizeof(url), "http://%s.local", st.host);
  canvas.setFont(F16B);
  if (canvas.textWidth(url) > 220) {
    tl(10, 56, url, C_TEAL2, F12B);
  } else {
    tl(10, 56, url, C_TEAL2, F16B);
  }
  tl(10, 82, T("页面里复制安装命令，粘贴到终端", "copy the install command, paste in Terminal"),
     C_MUTED, F12);
  tl(10, 98, T("装好后几秒内自动出数", "data appears within seconds"), C_DIM, F12);

  char buf[48];
  std::snprintf(buf, sizeof(buf), "%s · %d dBm", st.ip, st.rssi);
  tl(10, 112, buf, C_TEXT2, F12);
  (void)s;
}

// ---------------------------------------------------------------- 对外接口
void uiBegin() {
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("[UI] PSRAM sprite failed, fallback to heap");
    canvas.setPsram(false);
    canvas.createSprite(SCREEN_W, SCREEN_H);
  }
  canvas.setTextWrap(false);
  canvas.fillScreen(C_BG);
  canvas.pushSprite(0, 0);
  s_ready = true;
}

void uiRender(const Snapshot& s, const DisplayConfig& dc, const UiState& st,
              const Config& cfg) {
  if (!s_ready) return;
  EN = cfg.lang == 1;

  canvas.fillScreen(C_BG);
  drawChrome(s, st);

  if (!s.ok) {
    pageSetup(s, st);
  } else {
    uint8_t idx = st.page;
    if (idx >= dc.count) idx = 0;
    const DisplayPage& p = dc.pages[idx];
    switch (p.type) {
      case PT_SAVINGS: pageSavings(s, p); break;
      case PT_USAGE: pageUsage(s, p); break;
      case PT_TREND: pageTrend(s, p); break;
      case PT_MODELS: pageModels(s, p); break;
      case PT_COST: pageCost(s, p); break;
      case PT_DEVICE: pageDevice(s, st); break;
      default: pageSavings(s, p); break;
    }
  }

  drawFooter(dc, st);
  canvas.pushSprite(0, 0);
}

void uiSplash(const char* title, const char* sub, const char* foot) {
  if (!s_ready) return;
  canvas.fillScreen(C_BG);
  canvas.fillRect(0, 0, SCREEN_W, 18, C_SURF);
  canvas.drawFastHLine(0, 17, SCREEN_W, C_BORDER);
  canvas.fillCircle(9, 9, 3.5f, C_TEAL2);
  tl(16, 2, "oMLX Mate", C_TEXT2, F12B);
  tr(232, 2, "v" FW_VERSION, C_DIM, F12);

  canvas.setFont(HERO);
  canvas.setTextColor(C_TEAL2);
  canvas.setTextDatum(top_left);
  canvas.drawString("oMLX", 14, 34);
  tl(14, 70, title, C_TEXT, F16B);
  tl(14, 92, sub, C_MUTED, F12);
  tl(14, 110, foot, C_DIM, F12);
  canvas.pushSprite(0, 0);
}

void uiPortalScreen(const char* ssid, const char* pass, const char* url) {
  if (!s_ready) return;
  canvas.fillScreen(C_BG);
  canvas.fillRect(0, 0, SCREEN_W, 18, C_SURF);
  canvas.drawFastHLine(0, 17, SCREEN_W, C_BORDER);
  canvas.fillCircle(9, 9, 3.5f, C_AMBER);
  tl(16, 2, "oMLX Mate", C_TEXT2, F12B);
  tr(232, 2, T("配网", "SETUP"), C_AMBER, F12B);

  tl(10, 22, T("1  手机连接 Wi-Fi", "1  Join this Wi-Fi"), C_MUTED, F12);
  char shortSsid[20];
  fitText(shortSsid, sizeof(shortSsid), ssid, 130, F16B);
  tl(10, 36, shortSsid, C_TEAL2, F16B);
  tl(10, 56, T("密码", "pass"), C_MUTED, F12);
  tl(48, 55, pass, C_TEXT, F12B);

  tl(10, 74, T("2  浏览器打开", "2  Open browser"), C_MUTED, F12);
  tl(10, 88, url, C_TEXT, F12B);
  tl(10, 110, T("5 分钟内有效", "valid 5 min"), C_DIM, F12);

  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(3)];
  if (qrcode_initText(&qr, buf, 3, ECC_LOW, url) == 0) {
    int scale = 3;
    int px = 150, py = 24;
    int side = qr.size * scale;
    canvas.fillRoundRect(px - 4, py - 4, side + 8, side + 8, 4, C_WHITE);
    for (int my = 0; my < qr.size; my++) {
      for (int mx = 0; mx < qr.size; mx++) {
        if (qrcode_getModule(&qr, mx, my)) {
          canvas.fillRect(px + mx * scale, py + my * scale, scale, scale, C_BG);
        }
      }
    }
  }
  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- 调试截屏
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void uiDumpFrame() {
  if (!s_ready) return;
  const uint8_t* p = static_cast<const uint8_t*>(canvas.getBuffer());
  size_t n = static_cast<size_t>(SCREEN_W) * SCREEN_H * 2;
  Serial.printf("SHOT %d %d %u\n", SCREEN_W, SCREEN_H, static_cast<unsigned>(n));
  char line[100];
  int li = 0;
  for (size_t i = 0; i < n; i += 3) {
    size_t rem = n - i;
    uint32_t v = static_cast<uint32_t>(p[i]) << 16;
    if (rem > 1) v |= static_cast<uint32_t>(p[i + 1]) << 8;
    if (rem > 2) v |= p[i + 2];
    char q[4];
    q[0] = B64[(v >> 18) & 0x3F];
    q[1] = B64[(v >> 12) & 0x3F];
    q[2] = rem > 1 ? B64[(v >> 6) & 0x3F] : '=';
    q[3] = rem > 2 ? B64[v & 0x3F] : '=';
    for (int k = 0; k < 4; k++) {
      line[li++] = q[k];
      if (li >= 96) {
        line[li] = '\0';
        Serial.println(line);
        li = 0;
      }
    }
  }
  if (li) {
    line[li] = '\0';
    Serial.println(line);
  }
  Serial.println("SHOT-END");
}
