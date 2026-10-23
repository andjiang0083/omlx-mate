#pragma once
#include "config.h"
#include "display.h"
#include "snapshot.h"
#include "store.h"

struct UiState {
  uint8_t page = 0;
  bool wifiUp = false;
  bool stale = true;
  bool charging = false;
  int rssi = 0;
  int battery = 100;
  uint32_t ageSec = 0;
  char ip[16] = "--";
  char ssid[33] = "";
  char host[28] = "";
};

void uiBegin();
void uiRender(const Snapshot& s, const DisplayConfig& dc, const UiState& st,
              const Config& cfg);
void uiSplash(const char* title, const char* sub, const char* foot);
void uiPortalScreen(const char* ssid, const char* pass, const char* url);
void uiDumpFrame();
