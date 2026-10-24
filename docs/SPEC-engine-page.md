# SPEC（讨论稿）— oMLX Mate 加「引擎」页：把 omlx 看板的运行指标搬到设备上

状态：**讨论中**，未动代码。
起因：用户 2026-10-24 提出「app 缺少 omlx 看板的内容，把关键指标筛进来」。

---

## 1. 数据源实测（这是讨论的地基，先钉死）

### 1.1 `GET http://127.0.0.1:8000/api/status`（无鉴权可读，omlx 0.7.2）

```json
{"status":"ok","version":"0.7.2","uptime_seconds":165920.5,
 "models_discovered":12,"models_loaded":1,"models_loading":0,
 "default_model":"Qwen3.6-35B-A3B-8bit",
 "loaded_models":["Qwen3.8-Flash-Next-oQ4e-mtp"],
 "total_requests":583,"active_requests":0,"waiting_requests":0,
 "total_prompt_tokens":73558435,"total_completion_tokens":479496,
 "total_cached_tokens":72138932,
 "cache_efficiency":98.1,
 "avg_prefill_tps":771.8,"avg_generation_tps":47.1,
 "model_memory_used":78009236596,"model_memory_max":131845326144,
 "model_memory_used_formatted":"72.65GB","model_memory_max_formatted":"122.79GB",
 "custom_kernels":{...},"ane_prefill":{...}}
```

agent 现在只取了 4 个：`version` / `loaded_models` / `default_model` / `active_requests`。

### 1.2 没有更细的接口

`/metrics`、`/api/stats`、`/api/usage`、`/api/metrics`、`/api/models`、`/api/v1/models` **全部 404**。

### 1.3 用量库只有小时粒度

`~/.omlx/usage.sqlite3` 单表 `model_usage_hourly`：

```
timestamp_hour, model_id, requests, prompt_tokens, completion_tokens,
cached_tokens, prefill_seconds, generation_seconds, request_seconds, timed_requests
```

**结论：原方案里"最近 5 分钟实时 tok/s"做不到**（没有逐请求记录）。能用的口径只有两个：

| 口径 | 算法 | 特性 |
|---|---|---|
| 本小时 | `completion_tokens / generation_seconds`（当小时） | 有当小时请求才有值；omlx 重启不清零；比累计平均"新鲜" |
| omlx 累计 | `/api/status` 的 `avg_generation_tps` | 简单；omlx 重启后清零重算 |

---

## 2. 候选指标（小屏适配度筛选）

| 指标 | 实测值 | 适配 | 理由 |
|---|---|---|---|
| 生成速度 tok/s | 47.1 | ★★★ | 一个数说清"引擎跑多快"，有生命感 |
| 缓存效率 | 98.1% | ★★★ | 直接解释省的钱从哪来（98% 命中≈不重算） |
| 内存占用 | 72.65 / 122.79 GB | ★★★ | 两段数字 + 一条 bar，密度刚好 |
| 活跃 / 排队 | 0 / 0 | ★★ | 状态胶囊：空闲 / 生成中 / 排队 n |
| 已加载模型 | Qwen3.8-Flash-Next-oQ4e-mtp | ★★ | 显示"此刻在跑哪个"，模型名要截断到 26 字符 |
| 预填充速度 | 771.8 tok/s | ⚠️ | 技术味重，占一行但只有自己看得懂 |
| 累计请求 / 运行时长 | 583 / 46h | ⚠️ | 适合小字，不适合大数字 |
| 自定义 kernel / ANE 预填充 | - | ❌ | 配置项不是运行状态 |
| API 端点 / 集成 / Claude Code | - | ❌ | 小屏放不下，也没有看的意义 |

---

## 3. 三个版式（待选）

### A. 新增一页「引擎」（推荐）

```
┌──────────────────────────┐
│ 引擎                 46h │
│                          │
│      47.1                │
│      tok/s               │
│  ▓▓▓▓▓▓▓▓▓░  98.1% 缓存  │
│  模型  Qwen3.8-Flash-…   │
│  状态  空闲 · 排队 0      │
│  内存  72.65 / 122.79GB  │
│              , <    / >  │
└──────────────────────────┘
```

- 大数字：生成速度
- bar：缓存命中率（与省钱页同色系）
- 三行小字：模型 / 状态 / 内存
- 页脚沿用现有导航

### B. 拆两页：性能页 + 内存页

性能页（速度 + 缓存 + 状态）、内存页（占用 bar + 已加载模型列表）。信息更宽裕，但翻页变多，且内存页内容单薄。

### C. 不加页，只在省钱页加一行

省钱页底部加 `缓存命中 98.1%`，解释"省下的钱主要来自缓存"。改动最小，但"引擎视角"仍然没有。

---

## 4. 传输与体积

- snapshot payload 现在多大：待测（<2KB 是设计约束）
- 新增字段：`gen_tps` `prefill_tps` `cache_eff` `mem_used` `mem_max` `active` `waiting` `total_req` `uptime` `loading` ≈ 10 个数值/短串，估算 +120 字节，不破坏约束
- agent 侧：`omlx_status()` 扩展字段（已有 20 秒缓存，不增加探测频率）
- 固件侧：`Snapshot` 结构 + `PT_ENGINE` 渲染函数

---

## 5. 待用户定

1. 版式 A / B / C
2. 大数字用哪个：生成速度 / 缓存效率 / 内存占用
3. 口径：本小时 / omlx 累计
4. 是否顺便在省钱页加一行"缓存命中 xx%"

---

## 6. 风险

- `avg_generation_tps` 是**累计平均**，跑久了会越来越"钝"（重启清零后又要重新累积）。若用户以为它是实时值，会误判"怎么一直 47.1"。
- 缓存效率同理是累计值。
- 内存占用是真实瞬时值，可信。
- 熄屏断网（0.2.3）后，引擎页在熄屏期间不刷新 —— 唤醒拉一次即最新，符合设计。
