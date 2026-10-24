#pragma once
#include "store.h"

void netBegin(const Config& cfg);
void netPoll();
void netOff();                  // 熄屏：射频全关
void netOn();                   // 唤醒：恢复 STA 并重连
bool netIsUp();
int netRssi();
const char* netIp();
bool netTakeConnectedEvent();
