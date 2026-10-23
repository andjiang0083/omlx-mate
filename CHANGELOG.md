# Changelog

## 0.2.2 — 2026-10-23

- 新增 M5Cardputer ADV 版：同一份固件源码，实体键盘 `/` 下一页、`,` 上一页，长按 `R` 重新配网
- 板型差异收敛到 `include/board.h`，两种设备上的页面渲染完全一致
- Mac agent 与安装流程不变：Cardputer 版同样一条 curl 装好

## 0.2.1 — 2026-10-23

- 固件内置最新 Mac agent（`/agent.py` 由设备直接提供，安装不依赖 GitHub）
- 修复自定义 header 在 ESP32 WebServer 上取不到导致的 401：token 同时支持 `?k=` 与 `X-Mate-Token`
- 修复常驻 push 循环写回旧设备列表、导致网页命令丢失的问题
- 命令按 seq 去重；重启/恢复出厂延迟执行，避免 ack 丢失造成重启循环
- agent 增加 mDNS 失败后自动解析 IP 重试
- 设备大数字居中、标题回到左上角；状态胶囊加大为“峰时/谷时”
- 成本/模型/设备页不再显示无意义的图表选项；区间标题随配置变化
- “当前模型”优先取 loaded_models，其次最近一次真实请求的模型
- 充电时不熄屏（仅变暗），电池供电 3 分钟熄屏
- 设备首页复制按钮在非 HTTPS 下增加 execCommand 降级

## 0.2.0 — 2026-10-23

- 从“设备轮询看板”改为“Mac agent 推送 + 命令随响应下发”
- 设备配置驱动渲染：省钱/用量/趋势/模型/成本/设备 六种页面，A/B 翻页
- 设备内置 `/setup.sh`、`/agent.py`、`/uninstall.sh`，一条 curl 安装 LaunchAgent
- 本地控制台：用量/省钱、30 天趋势、显示配置 + 设备预览、设备管理、设置、日志
- 配对 token、mDNS、OTA（ArduinoOTA）、中英双语、串口调试与截屏
