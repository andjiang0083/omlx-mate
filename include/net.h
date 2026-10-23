#pragma once
#include "store.h"

void netBegin(const Config& cfg);
void netPoll();
bool netIsUp();
int netRssi();
const char* netIp();
bool netTakeConnectedEvent();
