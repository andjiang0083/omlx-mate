#include <ArduinoOTA.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "board.h"   // 板级抽象（StickS3 按键 ↔ Cardputer 键盘）
#include "config.h"
#include "display.h"
#include "net.h"
#include "portal.h"
#include "snapshot.h"
#include "store.h"
#include "ui.h"
#include "web.h"

Config g_cfg;
Snapshot g_snap;
DisplayConfig g_display;
uint8_t g_page = 0;
volatile bool g_newSnapshot = false;
char g_hostname[24] = "omlx-mate";
bool g_needRender = true;
int g_battery = 100;
bool g_charging = false;
uint32_t g_lastCmdSeq = 0;
uint32_t g_rebootAt = 0;

static bool screenOn = true;
static uint8_t bright = DEFAULT_BRIGHTNESS;
static uint32_t lastActivityMs = 0;
static uint32_t lastRenderMs = 0;
static uint32_t lastBatteryMs = 0;
static bool otaStarted = false;
static bool webStarted = false;
static bool s_radioOff = false;   // 射频已关（熄屏）
static bool s_pulled = false;     // 本轮已主动拉过快照
static bool s_plugged = false;    // 最近一次 VBUS 采样：插着线
static char lineBuf[160];
static size_t lineLen = 0;

static void beep(int freq = 1800) { M5.Speaker.tone(freq, 12); }
static void wakeScreen();
static void enterSleep();

// ---------------------------------------------------------------- 命令
void appApplyCommand(const char* type, int value) {
  if (!type || !type[0]) return;
  if (!std::strcmp(type, "page")) {
    if (value >= 0 && value < g_display.count) g_page = static_cast<uint8_t>(value);
  } else if (!std::strcmp(type, "next")) {
    g_page = (g_page + 1) % (g_display.count ? g_display.count : 1);
  } else if (!std::strcmp(type, "prev")) {
    g_page = (g_page + (g_display.count ? g_display.count : 1) - 1) %
             (g_display.count ? g_display.count : 1);
  } else if (!std::strcmp(type, "brightness")) {
    g_cfg.brightness = static_cast<uint8_t>(value < 10 ? 10 : (value > 255 ? 255 : value));
    bright = g_cfg.brightness;
    M5.Display.setBrightness(bright);
    configSave(g_cfg);
  } else if (!std::strcmp(type, "lang")) {
    g_cfg.lang = value == 1 ? 1 : 0;
    configSave(g_cfg);
  } else if (!std::strcmp(type, "identify")) {
    beep(2400);
  } else if (!std::strcmp(type, "reboot")) {
    g_rebootAt = millis() + 900;  // 先让 HTTP 响应把 ack 发回去
  } else if (!std::strcmp(type, "factory")) {
    configClear();
    g_rebootAt = millis() + 900;
  }
  g_needRender = true;
}

uint8_t appPageCount() { return g_display.count ? g_display.count : 1; }

// ---------------------------------------------------------------- 串口
static void printStatus() {
  Serial.printf("[STATUS] fw=%s host=%s.local ip=%s rssi=%d wifi=%d data=%d page=%u/%u\n",
                FW_VERSION, g_hostname, netIp(), netRssi(), netIsUp(), g_snap.ok, g_page,
                g_display.count);
  Serial.printf("[STATUS] today net=%.2f tok=%llu req=%u | d7 net=%.2f d30 net=%.2f\n",
                g_snap.today.net, static_cast<unsigned long long>(g_snap.today.tok),
                g_snap.today.req, g_snap.d7.net, g_snap.d30.net);
  Serial.printf("[STATUS] heap=%u psram=%u token=%s\n", ESP.getFreeHeap(),
                static_cast<unsigned>(ESP.getFreePsram()), g_cfg.token);
}

static void handleSerialLine(char* line) {
  wakeScreen();
  while (*line == ' ') line++;
  String cmd(line);
  cmd.trim();
  if (!cmd.length()) return;
  if (cmd.equalsIgnoreCase("WAKE")) {
    g_needRender = true;
  } else if (cmd.equalsIgnoreCase("SHOT")) {
    uiDumpFrame();
  } else if (cmd.equalsIgnoreCase("STATUS")) {
    printStatus();
  } else if (cmd.equalsIgnoreCase("PAGE")) {
    g_needRender = true;
  } else if (cmd.startsWith("PAGE ")) {
    int p = cmd.substring(5).toInt();
    if (p >= 0 && p < g_display.count) {
      g_page = static_cast<uint8_t>(p);
      g_needRender = true;
    }
  } else if (cmd.startsWith("LANG ")) {
    g_cfg.lang = cmd.substring(5).toInt() == 1 ? 1 : 0;
    configSave(g_cfg);
    g_needRender = true;
  } else if (cmd.startsWith("BRIGHT ")) {
    int b = cmd.substring(7).toInt();
    if (b >= 10 && b <= 255) {
      g_cfg.brightness = static_cast<uint8_t>(b);
      bright = g_cfg.brightness;
      M5.Display.setBrightness(bright);
      configSave(g_cfg);
    }
  } else if (cmd.startsWith("SET ")) {
    String rest = cmd.substring(4);
    int bar = rest.indexOf('|');
    if (bar < 0) {
      Serial.println("[CMD] usage: SET ssid|pass");
      return;
    }
    std::strncpy(g_cfg.ssid, rest.substring(0, bar).c_str(), sizeof(g_cfg.ssid) - 1);
    std::strncpy(g_cfg.pass, rest.substring(bar + 1).c_str(), sizeof(g_cfg.pass) - 1);
    configSave(g_cfg);
    Serial.printf("[CMD] wifi saved ssid=%s, rebooting\n", g_cfg.ssid);
    delay(300);
    ESP.restart();
  } else if (cmd.equalsIgnoreCase("RESET")) {
    configClear();
    delay(200);
    ESP.restart();
  } else {
    Serial.println("[CMD] SHOT | STATUS | PAGE n | LANG n | BRIGHT n | SET ssid|pass | RESET");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      handleSerialLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---------------------------------------------------------------- 交互
static void wakeScreen() {
  lastActivityMs = millis();
  if (!screenOn) {
    screenOn = true;
    setCpuFrequencyMhz(AWAKE_CPU_MHZ);
    M5.Display.wakeup();
    boardAudioOn();         // 熄屏时功放已断电，唤醒后重建（Cardputer 上是空操作）
    bright = g_cfg.brightness;
    M5.Display.setBrightness(bright);
    lastBatteryMs = 0;      // 立刻采一次电量
    s_pulled = false;       // 重连后主动拉一次快照
    g_needRender = true;
    if (s_radioOff) {
      s_radioOff = false;
      netOn();
    }
    Serial.println("[PWR] on (panel+radio up)");
  } else if (bright != g_cfg.brightness) {
    bright = g_cfg.brightness;
    M5.Display.setBrightness(bright);
  }
}

// 熄屏 = 主动下线 + CPU 真睡：面板停扫描、服务器/OTA 停、射频关、音频硬件断电
static void enterSleep() {
  if (!screenOn) return;
  screenOn = false;
  g_newSnapshot = false;
  M5.Display.sleep();               // 关背光 + 面板停扫描（比只置亮度 0 再省一档）
  if (webStarted) {
    webStop();
    webStarted = false;
  }
  if (otaStarted) {
    ArduinoOTA.end();
    otaStarted = false;
  }
  s_radioOff = true;
  netOff();
  boardAudioOff();                  // stop() 只停播放，amp 仍通电（睡醒边缘会放大 I2S 残余）
  setCpuFrequencyMhz(SLEEP_CPU_MHZ);
  Serial.printf("[PWR] off (heap=%u)\n", static_cast<unsigned>(ESP.getFreeHeap()));
}

static void enterPortal() {
  Serial.println("[PORTAL] enter");
  uiSplash("配置网络", "手机连接 oMLX-Mate-XXXX", BOARD_KEY_HINT_RSET);
  PortalResult r = portalRun(g_cfg, 300000);
  if (r == PORTAL_SAVED) {
    uiSplash("配置已保存", "正在重启…", "");
    delay(800);
    ESP.restart();
  }
  if (g_cfg.configured()) netBegin(g_cfg);
  lastActivityMs = millis();
  g_needRender = true;
}

// 返回 true = 需要进入配网页（长按重配网）
static bool handleButtons() {
  // 熄屏时第一下只负责亮屏：吃掉该次按下沿，第二下才翻页
  if (!screenOn) {
    if (boardAnyKeyDown()) {
      wakeScreen();
      boardPollKey();
    }
    return false;
  }
  BoardKey k = boardPollKey();
  if (k == BK_RECONFIG) return true;
  if (k == BK_NEXT) {
    g_page = (g_page + 1) % appPageCount();
    beep(2000);
    g_needRender = true;
  } else if (k == BK_PREV) {
    g_page = (g_page + appPageCount() - 1) % appPageCount();
    beep(1500);
    g_needRender = true;
  }
  return false;
}

static void startServices() {
  if (!webStarted) {
    webBegin();
    webStarted = true;
  }
  if (!otaStarted) {
    ArduinoOTA.setHostname(g_hostname);
    ArduinoOTA.setPassword("omlxmate");
    ArduinoOTA.onStart([]() { uiSplash("OTA 升级", "请勿断电", ""); });
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.printf("[OTA] %s.local  pass=omlxmate\n", g_hostname);
  }
}

// ---------------------------------------------------------------- 生命周期
void setup() {
  boardBegin();
  Serial.begin(115200);
  delay(250);

  boardPowerSetup();
  boardScreenSetup();
  M5.Display.setBrightness(DEFAULT_BRIGHTNESS);
  boardSpeakerSetup();
#ifndef BOARD_CARDPUTER
  setCpuFrequencyMhz(AWAKE_CPU_MHZ);   // 醒着档也不需要 240MHz（Cardputer 保持默认 240）
#endif

  uiBegin();
  uiSplash("正在启动", "读取配置…", "v" FW_VERSION);
  delay(400);

  configLoad(g_cfg);
  configEnsureToken(g_cfg);
  if (!displayLoad(g_display)) g_display = displayDefault();
  bright = g_cfg.brightness;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  std::snprintf(g_hostname, sizeof(g_hostname), "omlx-mate-%02x%02x", mac[4], mac[5]);

  Serial.printf("\n==== %s v%s ====\n", FW_NAME, FW_VERSION);
  Serial.printf("[BOOT] host=%s.local configured=%d ssid=%s token=%s pages=%u\n",
                g_hostname, g_cfg.configured(), g_cfg.ssid, g_cfg.token, g_display.count);

  if (!g_cfg.configured()) {
    uiSplash("首次配置", "用手机扫码连接 Wi-Fi", "正在打开热点…");
    delay(900);
    PortalResult r = portalRun(g_cfg, 300000);
    if (r == PORTAL_SAVED) {
      uiSplash("配置已保存", "正在重启…", "");
      delay(700);
      ESP.restart();
    }
  } else {
    netBegin(g_cfg);
  }

  lastActivityMs = millis();
  lastRenderMs = 0;
  g_needRender = true;
}

void loop() {
  if (g_rebootAt && millis() > g_rebootAt) ESP.restart();
  boardUpdate();
  pollSerial();
  if (handleButtons()) {
    enterPortal();
    return;
  }

  netPoll();
  if (netTakeConnectedEvent()) {
    startServices();
    if (!s_pulled) {
      s_pulled = true;
      agentPullSnapshot();
    }
  }
  if (webStarted) webLoop();
  if (otaStarted && netIsUp()) ArduinoOTA.handle();

  uint32_t now = millis();
  if (screenOn && now - lastBatteryMs > 5000) {
    lastBatteryMs = now;
    g_battery = M5.Power.getBatteryLevel();
    g_charging = M5.Power.isCharging();
  }

  // 插线（VBUS>1V）= 桌面上有人在用：豁免熄屏，但仍然变暗（避免整夜满背光）
  // ⚠️ 不能用 isCharging()：满电时它返回 false，会让插着线的设备在充满那刻熄屏。
  if (screenOn && USB_KEEP_AWAKE) {
    static uint32_t vbusAt = 0;
    if (now - vbusAt >= VBUS_POLL_MS) {
      vbusAt = now;
      s_plugged = M5.Power.getVBUSVoltage() > 1000;
    }
  }

  uint32_t idle = now - lastActivityMs;
  if (screenOn && idle > OFF_AFTER_MS && !(USB_KEEP_AWAKE && s_plugged)) {
    enterSleep();          // 熄屏 = 下线 + CPU 真睡
  } else if (screenOn && idle > DIM_AFTER_MS && bright != DIM_BRIGHTNESS) {
    bright = DIM_BRIGHTNESS;
    M5.Display.setBrightness(bright);
  }

  if (screenOn && (g_needRender || g_newSnapshot || now - lastRenderMs > FORCED_RENDER_MS)) {
    g_needRender = false;
    g_newSnapshot = false;
    UiState st;
    st.page = g_page;
    st.wifiUp = netIsUp();
    st.rssi = netRssi();
    st.battery = g_battery;
    st.charging = g_charging;
    st.stale = g_snap.receivedAtMs && (now - g_snap.receivedAtMs > 15000);
    st.ageSec = g_snap.receivedAtMs ? (now - g_snap.receivedAtMs) / 1000 : 0;
    std::strncpy(st.ip, netIp(), sizeof(st.ip) - 1);
    std::strncpy(st.host, g_hostname, sizeof(st.host) - 1);
    std::strncpy(st.ssid, g_cfg.ssid, sizeof(st.ssid) - 1);
    uiRender(g_snap, g_display, st, g_cfg);
    lastRenderMs = now;
  }

#ifdef BOARD_CARDPUTER
  delay(screenOn ? 15 : SLEEP_POLL_MS);   // 保持 0.2.3 代码形状（该板不做 light sleep）
#else
  // 熄屏档：直接睡到按键（RAM 保留，唤醒即续跑），不再 100ms 空转
  if (screenOn) delay(15);
  else boardSleepTier();
#endif
}
