#pragma once
#include "store.h"

enum PortalResult { PORTAL_SAVED, PORTAL_CANCEL, PORTAL_TIMEOUT };

// 阻塞式配网门户: 手机连 AP -> 表单 -> 保存. 保存成功返回 PORTAL_SAVED.
PortalResult portalRun(Config& cfg, uint32_t timeoutMs);
