#pragma once
#include <stdint.h>

// ---------------------------------------------------------------- 产品身份
#define FW_NAME "oMLX Mate"
#define FW_VERSION "0.2.4"
#define FW_BUILD __DATE__ " " __TIME__

// ---------------------------------------------------------------- 屏幕
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

// ---------------------------------------------------------------- 调色板
// 与网页看板 (omlx-savings) 同一套 Product Teal token, 换算成 RGB565.
#define C_BG 0x08A2      // #0E1614 页面底色
#define C_SURF 0x10E3    // #131D1B 卡片
#define C_SURF2 0x1924   // #1A2523 次级面
#define C_BORDER 0x2185  // #25332F 分隔线
#define C_TEXT 0xEF9D    // #E8F1EE 主文字
#define C_TEXT2 0xBE58   // #BAC9C5 次文字
#define C_MUTED 0x8D14   // #8FA3A0 弱文字
#define C_TEAL 0x0D2F    // #0CA678 品牌主色
#define C_TEAL2 0x15D0   // #12B886 亮青
#define C_TEAL_DIM 0x0A8C // 约 #0A4A38 柱状图底色
#define C_SOFT 0xDFBD    // #D9F5EC 亮青底
#define C_AMBER 0xF4E0   // #F59F00 峰时
#define C_RED 0xE186     // #E03131 错误/离线
#define C_GREEN 0x3589   // #37B24D 正常
#define C_WHITE 0xFFFF
#define C_DIM 0x6BEF // #6C7F7D

// ---------------------------------------------------------------- 行为参数
static constexpr uint32_t FETCH_INTERVAL_MS = 5000;      // 醒着时轮询
static constexpr uint32_t DIM_AFTER_MS = 60000;           // 变暗
static constexpr uint32_t OFF_AFTER_MS = 120000;          // 熄屏 = 下线（StickS3 插线时豁免）
static constexpr uint8_t DEFAULT_BRIGHTNESS = 75;
static constexpr uint8_t DIM_BRIGHTNESS = 35;
static constexpr uint32_t SLEEP_CPU_MHZ = 80;             // 熄屏后降频
static constexpr uint32_t SLEEP_POLL_MS = 100;            // 熄屏主循环间隔（Cardputer 轮询档）

// ---------------------------------------------------------------- 省电口径（仅 StickS3，见 docs/SPEC-0.2.4-power.md）
// 共享 src 被两个 env 编译；新行为一律只作用在 StickS3 分支，Cardputer 保持 0.2.3 原值。
#ifdef BOARD_CARDPUTER
static constexpr uint32_t AWAKE_CPU_MHZ = 240;
static constexpr uint32_t FORCED_RENDER_MS = 1000;   // 每 1s 兜底整屏重绘
static constexpr bool WIFI_MODEM_SLEEP = false;      // 射频常亮接收
static constexpr bool USB_KEEP_AWAKE = false;        // 插线豁免熄屏（Cardputer 充电弱，不启用）
#else
static constexpr uint32_t AWAKE_CPU_MHZ = 80;        // 重绘瓶颈是 SPI，240MHz 只是空转
static constexpr uint32_t FORCED_RENDER_MS = 60000;  // 内容只在快照变化/翻页时变
static constexpr bool WIFI_MODEM_SLEEP = true;       // 射频按 DTIM 睡，推 5s 一次无感
static constexpr bool USB_KEEP_AWAKE = true;         // 插线（VBUS>1V）= 不熄屏，只变暗
#endif
static constexpr uint32_t VBUS_POLL_MS = 2000;       // VBUS 是 I2C 读，必须节流

static constexpr uint32_t AGENT_PORT = 8788;              // agent 监听端口（Host 头缺失时兜底）
static constexpr uint32_t HTTP_TIMEOUT_MS = 6000;
static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
static constexpr uint16_t HTTP_PORT = 80;
