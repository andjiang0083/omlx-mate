# SPEC — oMLX Mate 0.2.3「熄屏即下线」省电

状态：**待批准**（用户 2026-10-23 提出：Cardputer 充电弱，插着 USB 也要省；这个 app 不需要实时性）
目标设备：M5Cardputer ADV（主）+ M5StickS3（同源受益）

---

## 1. 问题（实测）

| 漏点 | 证据 | 后果 |
|---|---|---|
| 充电永不熄屏 | `src/main.cpp:282` `if (screenOn && !g_charging && idle > OFF_AFTER_MS)` | 插着线 = 背光 90 全天亮 |
| WiFi 省电关闭 | `src/net.cpp:22` `WiFi.setSleep(false)` | 射频常开接收，最大单项 |
| 熄屏照旧每秒渲染 | `src/main.cpp` `now - lastRenderMs > 1000` 无 `screenOn` 判断 | 背光灭了，CPU 240MHz 仍在画 |
| 熄屏后仍每 30s 推送 | `include/config.h:34` `FETCH_INTERVAL_SLEEP_MS`（服务端推，设备被动收） | 每次射频唤醒 ~200mA 瞬时 |

实测（2026-10-23 23:12–23:13，插着 USB，AXP2101 报 charging=1）：
`65% → 64% → 63%`，约 1%/45s，净放电。即"插着线也在掉电"。

---

## 2. 用户定的原则

> 熄屏后网络活动和推送都可以停；只有亮屏时才联网，一旦联网数据能快速刷新。这个 app 不需要实时性。

因此设计目标不是"降频在线"，而是 **熄屏 = 主动下线**。

---

## 3. 状态机

| 状态 | 进入条件 | 亮度 | CPU | WiFi | HTTP/mDNS/OTA | 渲染 | 推送 |
|---|---|---|---|---|---|---|---|
| `AWAKE` | 任意键 / 上电 | 75 | 240MHz | on | on | 变化时+秒级 | agent 5s 推 |
| `DIM` | 无操作 60s | 35 | 240MHz | on | on | 变化时 | 同上 |
| `OFF` | 无操作 **120s**（充电也一样） | 0 + 面板 sleep | **80MHz** | **off** | **off** | 停 | 停 |

`OFF` 进入顺序（一次性）：
1. `M5.Display.setBrightness(0)` → `M5.Display.sleep()`（P1，失败则退回只关背光）
2. 跳过 `uiRender`，冻结 `UiState`
3. `webStop()`（Arduino WebServer 停 + OTA 停 + `MDNS.end()`）
4. `WiFi.disconnect(true)` → `WiFi.mode(WIFI_OFF)`
5. `setCpuFrequencyMhz(80)`
6. 主循环 `delay(100)`，只轮询键盘矩阵

`OFF → AWAKE` 唤醒顺序：
1. 亮度 75、CPU 240
2. `WiFi.mode(WIFI_STA)` + `WiFi.begin(ssid, pass)`（凭据在 NVS）
3. 连上（1–2s）→ **主动 GET 一次快照**（见 §4）
4. `webStart()` + mDNS 恢复（后台，不阻塞首帧）
5. 回到 5s 推送接收

---

## 4. 亮屏快刷（新增：设备主动拉）

现状是单向推送（agent `push_device()` → 设备 `/api/snapshot`）。熄屏断网后推不动，所以必须补一条**设备侧主动拉**的通道。

**设备端**
- 记录最近一次收到 POST 的客户端 IP（`server.client().remoteIP()`），存 NVS `agent_ip`
- 唤醒重连后：`GET http://<agent_ip>:8789/api/snapshot?k=<token>`，超时 1.5s
- 成功 → 立即渲染（首帧 ≤ 3s，其中 WiFi 连接占大头）
- 失败 → 退回等推送（agent 下个周期会推），屏上显示 `等待 Mac…`

**Mac 端（agent 0.1.2）**
1. 新增 `GET /api/snapshot?k=<token>`：返回内存里的当前快照 + display 配置（与 POST 同一份 payload）
2. 推送目标从 `http://<host>.local` 改为**上次成功的 IP**（缓存），mDNS 只在首次配对时用
   - 现有 `push_device()` 用 `.local`，设备断网时每次要等 DNS 超时（3s），
     `pusher_loop` 的 `th.join(timeout=3.5)` 会被拖住，影响其他设备
3. 连续 3 次失败 → 该设备退避到 30s 重试一次（日志降噪，不再每 5s 刷错）

---

## 5. 参数

| 常量 | 现在 | 0.2.3 |
|---|---|---|
| `DEFAULT_BRIGHTNESS` | 90 | **75** |
| `DIM_BRIGHTNESS` | 35 | 35 |
| `DIM_AFTER_MS` | 60000 | 60000 |
| `OFF_AFTER_MS` | 180000（且充电豁免） | **120000（无豁免）** |
| `FETCH_INTERVAL_MS` | 5000 | 5000（仅 AWAKE/DIM） |
| `FETCH_INTERVAL_SLEEP_MS` | 30000 | 删除（熄屏不推） |
| CPU | 240MHz | 240 / 熄屏 80 |
| `WiFi.setSleep` | false | AWAKE=false（保手感）；熄屏直接关射频，参数无意义 |

---

## 6. 待用户定的两点

1. **唤醒那一下要不要同时翻页？** 建议：只唤醒、不翻页（第二次按才翻），避免按住看时间变成翻页。
2. **熄屏期间不能 OTA**（设备离线）。建议接受：升级前人一定在设备旁边。

---

## 7. 验证（改前/改后同条件对比）

1. `pio run -e cardputer` + `release.py --strict` + `check_merged.py` 全绿
2. 刷 Cardputer，配网，agent 在线
3. **拔电 15 分钟**：改后用 agent 记电量曲线，与改前基线（1%/45s）对比；目标 ≥ 2 倍改善
4. 亮屏体验：按键 → 屏亮 + 数据刷新，人工计时 ≤ 3s
5. agent 日志：确认离线不再每 5s 刷错、亮屏后 1 次 GET 拿到快照
6. 双板回归：StickS3 同源编译通过（本次可不刷，下版一起发）

---

## 8. 不做（留给 0.2.4 或不做）

- `esp_light_sleep_start()` 深睡：Cardputer 键盘走 I2C（TCA8418），light sleep 下按键中断路径需另设计；熄屏关射频后预计已到 20–30mA 级，收益/风险不划算
- 键盘 RGB 背光控制：本版固件未使用，开启会额外耗电，确认默认关即可
- 亮度环境自适应：无光感硬件

---

## 9. 版本

- 固件 `0.2.3`（Cardputer + StickS3 同源）
- Mac agent `0.1.2`（GET 端点 + IP 缓存 + 退避）
- M5Burner：等 0.2.2 过审后再提 0.2.3，避免同一 fid 下两个待审版本
