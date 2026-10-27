# SPEC — oMLX Mate 0.2.4「熄屏真睡」省电

状态：**已批准**（用户 2026-10-27 拍板两选项：① 熄屏档走 light sleep；② StickS3 插电常亮，Cardputer 保持 0.2.3 行为）
目标设备：M5StickS3（250mAh，受电池约束的主目标）；M5Cardputer ADV **零改动**

---

## 1. 问题：0.2.3 只关了外设，CPU 一直醒着

0.2.3「熄屏 = 主动下线」关掉了射频、面板、HTTP/OTA，把 CPU 降到 80MHz，然后主循环
`delay(100)` 每 100ms 醒一次扫按键（`src/main.cpp:352`）。

| 档位 | 0.2.3 组成 | 估电流 | 每天占比 |
|---|---|---|---|
| AWAKE | 240MHz + `WiFi.setSleep(false)` + 每 1s 整屏重绘 | 90–110mA | 少数分钟 |
| DIM | 同上，背光 35 | 75–90mA | 1 分钟/次 |
| **OFF** | 射频关、面板睡，**CPU 80MHz 每 100ms 空转** | **15–20mA** | **90%+** |

250mAh 按 17mA 只能撑 ~15h —— 熄屏档这一项就把整机压到不足一天，其余优化加起来都没它大。
其他漏点：`net.cpp:23` 射频常亮接收（关掉 WiFi 省电）、`main.cpp:334` 每秒强制整屏重绘、
功放 `beep()` 后 amp 一直通电、BMI270 IMU 全程采样（本产品根本不用 IMU）。

> 以上除熄屏档外均为估数，见 §6 的实测口径。

---

## 2. 状态机（0.2.4，仅 StickS3 分支）

| 状态 | 进入 | 亮度 | CPU | WiFi | 渲染 | 主循环 |
|---|---|---|---|---|---|---|
| `AWAKE` | 任意键 / 上电 | 75 | **80MHz** | on（**modem sleep**） | 变化时 + 60s 兜底 | `delay(15)` |
| `DIM` | 无操作 60s | 35 | 80MHz | on | 同上 | 同上 |
| `OFF` | 无操作 120s **且未插线** | 0 + 面板 sleep | 80MHz | **off** | 停 | **`esp_light_sleep_start()`，无定时器** |
| `OFF`(插线) | VBUS > 1V | 保持 75 或 35 | 80MHz | on | 同上 | 同上（**不熄屏**） |

- 插线判定用 `M5.Power.getVBUSVoltage() > 1000`（mV），**不用 `isCharging()`** —— 满电时它返回 false，
  会让"插着线的桌面设备"在电量满的那一刻熄屏。
- 插线只豁免熄屏，**不豁免变暗**：桌上过夜不会烧一整夜满背光。
- `OFF → AWAKE`：light sleep 唤醒后 RAM 全在 → 缓存的快照直接重绘（**首帧 ~0.2s**，比 0.2.3 的 3s 更好），
  随后射频重连 + 主动 GET 刷新（2–3s）。首帧可能带 `stale` 标记，属正常（数据确实是旧的，不谎报）。
- 熄屏时第一次按键仍只负责亮屏（`handleButtons` 既有逻辑，唤醒键在该分支被消费）。

---

## 3. 熄屏档实现（核心改动）

```cpp
// 睡前（enterSleep）：外设全部硬件级关断，不是逻辑停止
M5.Display.sleep();      // 已有
netOff();                // 已有：WiFi.mode(WIFI_OFF)
M5.Speaker.end();        // 新增：stop() 只停播放，amp 仍通电（睡醒边缘会放大 I2S 残余）
setCpuFrequencyMhz(80);  // 已有

// 主循环尾部（熄屏时）
gpio_config_t gc = {};
gc.pin_bit_mask = (1ULL << 11) | (1ULL << 12);   // BtnA/BtnB，已核 M5Unified.inl:2503
gc.mode = GPIO_MODE_INPUT;
gc.pull_up_en = GPIO_PULLUP_ENABLE;              // 低有效，悬浮高电平唤不醒
gc.intr_type = GPIO_INTR_LOW_LEVEL;              // ⚠️ 枚举是 LOW_LEVEL，不是 LOWLEVEL
gpio_config(&gc);
gpio_wakeup_enable(GPIO_NUM_11, GPIO_INTR_LOW_LEVEL);
gpio_wakeup_enable(GPIO_NUM_12, GPIO_INTR_LOW_LEVEL);
esp_sleep_enable_gpio_wakeup();
if (esp_light_sleep_start() != ESP_OK) delay(100);  // 睡不进去就退轮询，绝不留热循环
gpio_wakeup_disable(GPIO_NUM_11);
gpio_wakeup_disable(GPIO_NUM_12);
```

**为什么不是 deep sleep**：deep sleep 每次唤醒 = 重启 + 启动页 + WiFi 重连（3s，且 RAM 全丢），
而 light sleep 已是 ~1mA 量级、唤醒即续跑。日常档位不值得为一个数量级牺牲手感。
deep sleep 只在电量危急时做保险（本版不做，见 §7）。

**为什么不用 IMU 唤醒（拿起即亮）**：StickS3 的 IMU 中断接在 PM1 的 GPIO4，不是 SoC 的
RTC/ext 脚 —— `ext0/ext1` 唤不动，只能定时醒来轮询（反而更费电）。硬件路径不通，不做。

---

## 4. 醒着档降耗（决定「看一眼」花多少）

一次 2 分钟满亮 ≈ 3.3mAh；一天摸 20 次就吃掉 1/4 电量。所以醒着档也要压：

| 项 | 0.2.3 | 0.2.4 | 理由 |
|---|---|---|---|
| `WiFi.setSleep` | `false` | `true`（MIN_MODEM） | 射频常亮接收是最大单项；推 5s 一次，+几十 ms 无感 |
| `AWAKE_CPU_MHZ` | 240 | **80** | 重绘瓶颈是 SPI 不是 CPU，240MHz 只是空转 |
| 整屏重绘兜底 | 每 **1s** | 每 **60s** | 内容只在快照变化/翻页时变，1s 一次纯浪费（每秒 64KB SPI + 全画布重绘） |

---

## 5. 共享代码树的隔离口径

`src/` 被两个 env 共用。**本版所有新行为都包在 `#ifndef BOARD_CARDPUTER` 内**，Cardputer 的
`AWAKE_CPU_MHZ` / `WiFi.setSleep` / 重绘周期 / 主循环节拍一律保持 0.2.3 原值 —— 该板键盘在
TCA8418（I2C）上，GPIO 唤不醒，light sleep 本身对它也不适用。改动可用 diff 逐条核对。

---

## 6. 验证

改前基线（0.2.3 Cardputer 实测）：`65% → 64% → 63%`，约 1%/45s（插 USB，净放电）。

1. `pio run -e sticks3` + `pio run -e cardputer` 双绿（Cardputer 只做编译回归，不刷）
2. 真机（用户，自动交接）：拔线 → 等 2 分钟熄屏 → **按键必须一次就亮**（这是 GPIO11/12 唤醒的唯一判据）
3. 电量曲线：满电拔线，记 `%` 与时间到掉 10%，对比 0.2.3 同条件；目标 ≥ 3 倍改善
4. 插线：VBUS 在位 → 不再熄屏（只变暗）；拔线 2 分钟后熄屏
5. 亮屏体验：按键 → 缓存的上一帧立刻出，2–3s 内数字刷新
6. 熄屏期间 USB CDC 掉线属预期（light sleep 的已知行为），不做串口验证
7. **唤醒后看电量格/信号格是否还会变**：light sleep 会门控外设时钟，若 I2C 醒来没恢复，
   表现就是电量数字冻住（屏照常显示、按键也灵，只有数字不动）—— 这是本版唯一没把握的点

---

## 7. 不做

- Cardputer 版 light sleep（键盘 I2C，唤醒路径未验证）
- 日常 deep sleep / PM1 `powerOff()`：留在「电量 <5% 保险」场景，本版不做
- IMU 拿起唤醒（硬件路径不通，见 §3）
- 内置电压斜率电表：本版用「拔线掉 10% 记时间」粗测即可判 3 倍差异；数字若有争议再补 bench 构建

---

## 8. 版本

固件 `0.2.4`（StickS3 行为变更，Cardputer 零改动）；Mac agent 不变。
