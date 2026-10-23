#pragma once
#include <stdint.h>

#include "display.h"
#include "snapshot.h"
#include "store.h"

// 全局共享状态 (main 定义, web/ui 只读或用 setter)
extern Config g_cfg;
extern Snapshot g_snap;
extern DisplayConfig g_display;
extern uint8_t g_page;
extern volatile bool g_newSnapshot;
extern char g_hostname[24];
extern bool g_needRender;
extern int g_battery;
extern uint32_t g_lastCmdSeq;
extern uint32_t g_rebootAt;
extern bool g_charging;

// 命令执行 (由 /api/snapshot 的 cmd 字段触发)
void appApplyCommand(const char* type, int value);
uint8_t appPageCount();
