#pragma once
// 板级抽象：M5StickS3（BtnA/BtnB）↔ M5Cardputer ADV（物理键盘）
// 由 platformio.ini 的 -DBOARD_CARDPUTER=1 切换，源码其余部分不需要关心板型。
// 屏幕同为 240×135（Cardputer 横装 rotation(1)），UI/渲染层零改动。

#include <M5Unified.h>

#ifdef BOARD_CARDPUTER
#include <M5Cardputer.h>
#endif

// ---------------------------------------------------------------- 初始化
inline void boardBegin() {
  auto cfg = M5.config();
#ifdef BOARD_CARDPUTER
  // ⚠️ 必须走 M5Cardputer.begin()：M5.begin() + M5Cardputer.begin() 会让 I2C 键盘静默死
  cfg.fallback_board = m5::board_t::board_M5Cardputer;
  M5Cardputer.begin(cfg);
#else
  M5.begin(cfg);
#endif
}

inline void boardUpdate() {
#ifdef BOARD_CARDPUTER
  M5Cardputer.update();   // 每轮 loop 都要调，否则键盘扫描/事件丢失
#else
  M5.update();
#endif
}

inline void boardScreenSetup() {
  M5.Display.setRotation(1);
}

inline void boardSpeakerSetup() {
#ifdef BOARD_CARDPUTER
  M5.Speaker.setVolume(80);   // 小喇叭腔体，同档音量明显低于 StickS3
#else
  M5.Speaker.setVolume(48);
#endif
}

inline void boardPowerSetup() {
#ifndef BOARD_CARDPUTER
  M5.Power.setExtOutput(false);   // StickS3 的 Grove 供电脚；Cardputer 无此语义
#endif
}

// ---------------------------------------------------------------- 输入
enum BoardKey { BK_NONE = 0, BK_NEXT, BK_PREV, BK_RECONFIG };

#ifdef BOARD_CARDPUTER

static uint32_t s_keyPrevMask = 0;
static uint32_t s_keyHoldStart = 0;
static bool s_keyReconfigFired = false;

// 位定义：a-z = 0..25，符号键单独占高位（⚠️ 符号键不能参与 k-'a'，那是负位移 UB）
#define BK_BIT_SLASH (1UL << 26)
#define BK_BIT_COMMA (1UL << 27)
#define BK_BIT_DOT   (1UL << 28)

inline uint32_t boardKeyMask(const char* word, size_t n) {
  uint32_t m = 0;
  for (size_t i = 0; i < n; i++) {
    char k = word[i];
    if (k >= 'A' && k <= 'Z') k += 32;
    if (k >= 'a' && k <= 'z') m |= (1UL << (k - 'a'));
    else if (k == '/') m |= BK_BIT_SLASH;
    else if (k == ',') m |= BK_BIT_COMMA;
    else if (k == '.') m |= BK_BIT_DOT;
  }
  return m;
}

// 返回本轮的“边沿”事件（翻页）；长按 r 单独返回 BK_RECONFIG
inline BoardKey boardPollKey() {
  auto ks = M5Cardputer.Keyboard.keysState();
  size_t n = ks.word.size();
  // 把被按住的键拼成掩码（word 是 std::vector<char>，不能直接当数组传）
  char buf[32];
  size_t m = 0;
  for (size_t i = 0; i < n && m < sizeof(buf); i++) buf[m++] = ks.word[i];
  uint32_t mask = boardKeyMask(buf, m);
  uint32_t edge = mask & ~s_keyPrevMask;
  s_keyPrevMask = mask;

  // 长按 r（1.6s）= 重新配网
  bool rHeld = (mask & (1UL << ('r' - 'a'))) != 0;
  if (rHeld) {
    if (!s_keyHoldStart) s_keyHoldStart = millis();
    if (!s_keyReconfigFired && millis() - s_keyHoldStart > 1600) {
      s_keyReconfigFired = true;
      return BK_RECONFIG;
    }
  } else {
    s_keyHoldStart = 0;
    s_keyReconfigFired = false;
  }

  // 下一页：/ 或 . 或 d ；上一页：, 或 a
  if (edge & (BK_BIT_SLASH | BK_BIT_DOT | (1UL << ('d' - 'a')))) return BK_NEXT;
  if (edge & (BK_BIT_COMMA | (1UL << ('a' - 'a')))) return BK_PREV;
  return BK_NONE;
}

inline bool boardAnyKeyDown() {
  auto ks = M5Cardputer.Keyboard.keysState();
  return ks.word.size() > 0 || ks.enter || ks.del;
}

#else   // ------------------------------------------------------------ StickS3

inline BoardKey boardPollKey() {
  if (millis() < 2500) return BK_NONE;   // 防止开机误触
  if (M5.BtnB.pressedFor(1600)) return BK_RECONFIG;
  if (M5.BtnA.wasClicked()) return BK_NEXT;
  if (M5.BtnB.wasClicked()) return BK_PREV;
  return BK_NONE;
}

inline bool boardAnyKeyDown() { return M5.BtnA.isPressed() || M5.BtnB.isPressed(); }

#endif

// 配网向导里“跳过/取消”的按键判定（长按 B / 按任意键）
inline bool boardPortalWantsCancel() {
#ifdef BOARD_CARDPUTER
  return boardAnyKeyDown();
#else
  return M5.BtnB.pressedFor(1500);
#endif
}

// UI 底部导航提示 / 配网页提示（跟按键映射同步）
#ifdef BOARD_CARDPUTER
#define BOARD_NAV_LEFT   ", <"
#define BOARD_NAV_RIGHT  "/ >"
#define BOARD_KEY_HINT_RSET  "长按 R 重新配网"
#else
#define BOARD_NAV_LEFT   "A >"
#define BOARD_NAV_RIGHT  "< B"
#define BOARD_KEY_HINT_RSET  "长按 B 重新配网"
#endif
