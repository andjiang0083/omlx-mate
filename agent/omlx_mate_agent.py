#!/usr/bin/env python3
"""oMLX Mate Agent — Mac 侧数据核心 + 本地控制台.

职责
  1. 只读 oMLX 的 ~/.omlx/usage.sqlite3 (小时 x 模型), 按 DeepSeek 峰谷/缓存价折算"省了多少钱"
  2. 每 N 秒把一份 <2KB 的快照 POST 给局域网里的 StickS3
  3. 在 127.0.0.1:<自动端口> 提供本地控制台: 看用量、改显示配置、控设备页面/重启
  4. 设备离线不影响: 数据在 Mac 端持续计算, 设备一上线就收到最新快照

零第三方依赖, 只用标准库. 由设备自带的 /setup.sh 安装为 LaunchAgent.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import date, datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse

APP_NAME = "oMLX Mate Agent"
VERSION = "0.1.1"
LABEL = "com.omlx.mate"

HOME = Path.home()
OMLX_DIR = Path(os.environ.get("OMLX_BASE_PATH", HOME / ".omlx"))
MATE_DIR = Path(os.environ.get("OMLX_MATE_DIR", HOME / ".omlx-mate"))
CFG_PATH = MATE_DIR / "config.json"
LOG_PATH = MATE_DIR / "agent.log"
PLIST_PATH = HOME / "Library" / "LaunchAgents" / f"{LABEL}.plist"

CST = timezone(timedelta(hours=8))

# ---------------------------------------------------------------- 定价 (与看板同源)
# 元 / 百万 tokens, 每项 = (空闲价, 高峰价)
DEEPSEEK_PRICING = {
    "flash": {
        "label": "deepseek-flash",
        "cny": {"input_cache_hit": (0.02, 0.04),
                "input_cache_miss": (1.0, 2.0),
                "output": (4.0, 8.0)},
        "usd": {"input_cache_hit": (0.003, 0.006),
                "input_cache_miss": (0.15, 0.30),
                "output": (0.60, 1.20)},
    },
    "pro": {
        "label": "deepseek-v4-pro",
        "cny": {"input_cache_hit": (0.15, 0.30),
                "input_cache_miss": (4.5, 9.0),
                "output": (13.5, 27.0)},
        "usd": {"input_cache_hit": (0.022, 0.044),
                "input_cache_miss": (0.66, 1.32),
                "output": (1.98, 3.96)},
    },
}

PEAK_RANGES = [(9, 12), (14, 18)]
PEAK_WEEKDAYS = range(0, 5)

CN_HOLIDAYS_2026 = {
    "2026-01-01", "2026-01-02", "2026-01-03",
    "2026-02-15", "2026-02-16", "2026-02-17", "2026-02-18",
    "2026-02-19", "2026-02-20", "2026-02-21", "2026-02-22", "2026-02-23",
    "2026-04-04", "2026-04-05", "2026-04-06",
    "2026-05-01", "2026-05-02", "2026-05-03", "2026-05-04", "2026-05-05",
    "2026-06-19", "2026-06-20", "2026-06-21",
    "2026-09-25", "2026-09-26", "2026-09-27",
    "2026-10-01", "2026-10-02", "2026-10-03", "2026-10-04",
    "2026-10-05", "2026-10-06", "2026-10-07",
}
CN_WORK_WEEKENDS_2026 = {
    "2026-01-04", "2026-02-14", "2026-02-28",
    "2026-05-09", "2026-09-20", "2026-10-10",
}

PRICING_REVISIONS = [
    {"effective_from": None, "peak_mode": "all_days_peak"},
    {"effective_from": "2026-08-23", "peak_mode": "weekdays_peak"},
    {"effective_from": "2026-09-19", "peak_mode": "holidays_offpeak"},
]

PAGE_TYPES = ("savings", "usage", "trend", "models", "cost", "device")
CHART_TYPES = ("d7", "d30", "hourly", "none")
RANGE_TYPES = ("today", "yesterday", "d7", "d30")

DEFAULT_DISPLAY = {
    "version": 1,
    "pages": [
        {"type": "savings", "range": "today", "chart": "d7", "dense": False},
        {"type": "usage", "range": "today", "chart": "hourly", "dense": False},
        {"type": "trend", "range": "d7", "chart": "d30", "dense": False},
        {"type": "models", "range": "d7", "chart": "none", "dense": False},
    ],
}

DEFAULT_CFG = {
    "port": 8788,
    "lan_access": False,
    "lan_code": "",
    "push_seconds": 5,
    "lang": 0,
    "display": DEFAULT_DISPLAY,
    "pricing": {
        "ds_model": "flash",
        "currency": "CNY",
        "peak": True,
        "power_prefill_w": 200.0,
        "power_decode_w": 65.0,
        "elec_price": 0.6,
        "device_cny": 70000.0,
        "depreciate_days": 1095,
    },
    "devices": [],
}

CFG_LOCK = threading.RLock()
CFG = {}
CACHED_SNAP = {}
STARTED_AT = time.time()
ACTUAL_PORT = 0
OMLX_CACHE = {"at": 0.0, "last_ok": 0.0, "value": {"up": False, "ver": "", "model": "", "active": 0}}


# ---------------------------------------------------------------- 基础工具
def log(msg: str) -> None:
    line = f"[{datetime.now(CST):%Y-%m-%d %H:%M:%S}] {msg}"
    print(line, flush=True)
    try:
        MATE_DIR.mkdir(parents=True, exist_ok=True)
        if LOG_PATH.exists() and LOG_PATH.stat().st_size > 1_000_000:
            LOG_PATH.replace(MATE_DIR / "agent.log.1")
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def deep_merge(base, patch):
    out = dict(base)
    for k, v in (patch or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def load_cfg() -> dict:
    cfg = json.loads(json.dumps(DEFAULT_CFG))
    try:
        if CFG_PATH.exists():
            cfg = deep_merge(cfg, json.loads(CFG_PATH.read_text(encoding="utf-8")))
    except Exception as exc:
        log(f"config load failed: {exc}, using defaults")
    cfg["display"] = sanitize_display(cfg.get("display"))
    if cfg["pricing"].get("ds_model") not in DEEPSEEK_PRICING:
        cfg["pricing"]["ds_model"] = "flash"
    cfg["pricing"]["currency"] = "USD" if cfg["pricing"].get("currency") == "USD" else "CNY"
    return cfg


def save_cfg(cfg: dict) -> None:
    MATE_DIR.mkdir(parents=True, exist_ok=True)
    tmp = CFG_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(CFG_PATH)


def sanitize_display(display) -> dict:
    pages = []
    for p in (display or {}).get("pages") or []:
        if not isinstance(p, dict):
            continue
        t = p.get("type")
        if t not in PAGE_TYPES:
            continue
        pages.append({
            "type": t,
            "range": p.get("range") if p.get("range") in RANGE_TYPES else "today",
            "chart": p.get("chart") if p.get("chart") in CHART_TYPES else "none",
            "dense": bool(p.get("dense")),
        })
        if len(pages) >= 8:
            break
    if not pages:
        pages = json.loads(json.dumps(DEFAULT_DISPLAY["pages"]))
    return {"version": int((display or {}).get("version") or 1), "pages": pages}


# ---------------------------------------------------------------- 峰谷规则
def revision_for(ts: float) -> dict:
    day = datetime.fromtimestamp(ts, CST).date().isoformat()
    rev = PRICING_REVISIONS[0]
    for r in PRICING_REVISIONS:
        if r["effective_from"] is None or r["effective_from"] <= day:
            rev = r
    return rev


def in_peak_hours(dt: datetime) -> bool:
    return any(lo <= dt.hour < hi for lo, hi in PEAK_RANGES)


def is_peak(ts: float) -> bool:
    dt = datetime.fromtimestamp(ts, CST)
    day = dt.date().isoformat()
    mode = revision_for(ts)["peak_mode"]
    if mode == "holidays_offpeak":
        if day in CN_HOLIDAYS_2026 or day in CN_WORK_WEEKENDS_2026:
            return False
    if mode in ("weekdays_peak", "holidays_offpeak"):
        if dt.weekday() not in PEAK_WEEKDAYS:
            return False
    return in_peak_hours(dt)


def price_for(model: str, currency: str, peak: bool) -> dict:
    tbl = DEEPSEEK_PRICING[model][currency.lower()]
    idx = 1 if peak else 0
    return {k: v[idx] for k, v in tbl.items()}


# ---------------------------------------------------------------- oMLX 数据
def load_omlx_settings() -> dict:
    try:
        return json.loads((OMLX_DIR / "settings.json").read_text(encoding="utf-8"))
    except Exception:
        return {}


def omlx_origin(settings) -> str:
    host = os.environ.get("OMLX_HOST") or settings.get("server", {}).get("host", "127.0.0.1")
    if host in ("0.0.0.0", "::"):
        host = "127.0.0.1"
    port = os.environ.get("OMLX_PORT") or settings.get("server", {}).get("port", 8000)
    return f"http://{host}:{port}"


def read_rows(days: int = 33):
    db = OMLX_DIR / "usage.sqlite3"
    if not db.exists():
        return None, f"未找到 {db}"
    since = int(datetime.combine(date.today() - timedelta(days=days),
                                 datetime.min.time(), CST).timestamp())
    sql = ("SELECT timestamp_hour, model_id, requests, prompt_tokens, "
           "completion_tokens, cached_tokens, prefill_seconds, generation_seconds "
           "FROM model_usage_hourly WHERE timestamp_hour >= ? ORDER BY timestamp_hour")
    try:
        con = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=2.0)
        rows = con.execute(sql, (since,)).fetchall()
        con.close()
    except Exception:
        try:
            con = sqlite3.connect(str(db), timeout=2.0)
            rows = con.execute(sql, (since,)).fetchall()
            con.close()
        except Exception as exc:
            return None, f"sqlite: {type(exc).__name__}: {exc}"
    out = [(int(r[0]) // 3600 * 3600, str(r[1] or ""), int(r[2] or 0), int(r[3] or 0),
            int(r[4] or 0), int(r[5] or 0), float(r[6] or 0), float(r[7] or 0))
           for r in rows]
    return out, ""


def rows_in_range(rows, rng: str, today: date):
    def d(ts):
        return datetime.fromtimestamp(ts, CST).date()
    if rng == "today":
        return [r for r in rows if d(r[0]) == today]
    if rng == "yesterday":
        y = today - timedelta(days=1)
        return [r for r in rows if d(r[0]) == y]
    n = 7 if rng == "7d" else 30
    start = today - timedelta(days=n - 1)
    return [r for r in rows if start <= d(r[0]) <= today]


def compute_range(rows, rng: str, pricing: dict, today: date, peak_enabled: bool):
    model = pricing["ds_model"]
    currency = pricing["currency"]
    p_off = price_for(model, currency, False)
    p_pk = price_for(model, currency, True)
    sel = rows_in_range(rows, rng, today)

    t = {"api": 0.0, "nocache": 0.0, "pk": 0.0, "pre": 0.0, "gen": 0.0,
         "prompt": 0, "cached": 0, "completion": 0, "req": 0}
    daily = {}
    models = {}
    hourly = [0.0] * 24

    for ts, mid, req, prompt, compl, cached, pre, gen in sel:
        peak = peak_enabled and is_peak(ts)
        p = p_pk if peak else p_off
        uncached = max(prompt - cached, 0)
        cost = (uncached * p["input_cache_miss"] + cached * p["input_cache_hit"]
                + compl * p["output"]) / 1_000_000
        nocache = (prompt * p["input_cache_miss"] + compl * p["output"]) / 1_000_000

        t["api"] += cost
        t["nocache"] += nocache
        t["prompt"] += prompt
        t["cached"] += cached
        t["completion"] += compl
        t["req"] += req
        t["pre"] += pre
        t["gen"] += gen
        if peak:
            t["pk"] += cost

        day = datetime.fromtimestamp(ts, CST).date().isoformat()
        daily[day] = daily.get(day, 0.0) + cost
        if mid:
            m = models.setdefault(mid, {"c": 0.0, "t": 0, "r": 0})
            m["c"] += cost
            m["t"] += prompt + compl
            m["r"] += req
        if rng == "today":
            hour = datetime.fromtimestamp(ts, CST).hour
            hourly[hour] += cost

    elec = (t["pre"] * float(pricing["power_prefill_w"])
            + t["gen"] * float(pricing["power_decode_w"])) / 3600.0 / 1000.0 \
        * float(pricing["elec_price"])
    tok = t["prompt"] + t["completion"]
    api = t["api"]
    return {
        "net": round(api - elec, 2),
        "api": round(api, 2),
        "elec": round(elec, 2),
        "tok": tok,
        "req": t["req"],
        "hit": round(t["cached"] / t["prompt"], 3) if t["prompt"] else 0.0,
        "cs": round(t["nocache"] - api, 2),
        "pk": round(t["pk"], 2),
        "_daily": daily,
        "_models": models,
        "_hourly": hourly,
    }


def build_series(daily: dict, days: int, today: date):
    return [round(daily.get((today - timedelta(days=i)).isoformat(), 0.0), 2)
            for i in range(days - 1, -1, -1)]


def omlx_status(force: bool = False) -> dict:
    now = time.time()
    if not force and now - OMLX_CACHE["at"] < 20:
        return OMLX_CACHE["value"]
    st = {"up": False, "ver": "", "model": "", "active": 0}
    settings = load_omlx_settings()
    origin = omlx_origin(settings)
    key = str(settings.get("auth", {}).get("api_key") or "")
    req = urllib.request.Request(origin + "/api/status")
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    try:
        with urllib.request.urlopen(req, timeout=3.0) as r:
            data = json.loads(r.read().decode("utf-8"))
        loaded = data.get("loaded_models") or []
        model = (loaded[0] if loaded else str(data.get("default_model") or ""))
        st.update({
            "up": True,
            "ver": str(data.get("version") or "")[:11],
            "model": str(model)[:26],
            "active": int(data.get("active_requests") or 0),
        })
        OMLX_CACHE["last_ok"] = now
    except Exception:
        # 单次探测超时不代表 oMLX 掉了, 90s 内沿用上次成功状态, 避免屏幕闪"离线"
        if now - float(OMLX_CACHE.get("last_ok") or 0) < 90:
            prev = dict(OMLX_CACHE["value"])
            prev["up"] = True
            st = prev
    OMLX_CACHE.update({"at": now, "value": st})
    return st


def build_snapshot(cfg: dict) -> dict:
    now = datetime.now(CST)
    today = now.date()
    rows, err = read_rows(33)
    pricing = cfg["pricing"]
    snap = {
        "v": 1,
        "ok": 1 if rows is not None else 0,
        "ts": int(time.time()),
        "srv": now.strftime("%H:%M:%S"),
        "peak": 1 if is_peak(time.time()) else 0,
        "metrics": {},
        "series": {"d7": [0.0] * 7, "d30": [0.0] * 30, "hourly": [0.0] * 24},
        "models": [],
        "omlx": omlx_status(),
        "err": err,
    }
    if rows is None:
        return snap

    today_data = compute_range(rows, "today", pricing, today, pricing["peak"])
    yday = compute_range(rows, "yesterday", pricing, today, pricing["peak"])
    d7 = compute_range(rows, "7d", pricing, today, pricing["peak"])
    d30 = compute_range(rows, "30d", pricing, today, pricing["peak"])

    def pub(d):
        return {k: v for k, v in d.items() if not k.startswith("_")}

    snap["metrics"] = {"today": pub(today_data), "y": pub(yday),
                       "d7": pub(d7), "d30": pub(d30)}
    snap["series"] = {
        "d7": build_series(d7["_daily"], 7, today),
        "d30": build_series(d30["_daily"], 30, today),
        "hourly": [round(v, 2) for v in today_data["_hourly"]],
    }
    # 模型懒加载时 loaded_models 常为空; 用最近一次真实请求的模型补上,
    # 比 default_model 更接近"当前在用"
    latest = ""
    for r in sorted(rows, key=lambda x: x[0]):
        if r[2] > 0 and r[1]:
            latest = r[1]
    if latest:
        snap["omlx"]["model"] = latest[:26]

    top = sorted(d7["_models"].items(), key=lambda kv: -kv[1]["c"])[:5]
    snap["models"] = [{"n": k[:26], "c": round(v["c"], 2), "t": v["t"], "r": v["r"]}
                      for k, v in top]
    return snap


# ---------------------------------------------------------------- 设备推送
def device_public(d: dict) -> dict:
    return {
        "id": d.get("id", ""),
        "name": d.get("name", ""),
        "url": d.get("url", ""),
        "online": bool(d.get("online")),
        "last_seen": d.get("last_seen", 0),
        "state": d.get("state", {}),
        "pending": d.get("pending"),
    }


def _post_snapshot(base: str, body: dict, token: str) -> dict:
    url = base.rstrip("/") + "/api/snapshot"
    if token:
        url += "?k=" + quote(token)
    req = urllib.request.Request(
        url, data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json", "X-Mate-Token": token},
        method="POST")
    with urllib.request.urlopen(req, timeout=3.0) as r:
        return json.loads(r.read().decode("utf-8") or "{}")


def _resolve_ip_base(base: str) -> str:
    """把 http://omlx-mate-xxxx.local 解析成 http://192.168.x.x:port（mDNS 抖动兜底）."""
    try:
        u = urlparse(base)
        host = u.hostname or ""
        if not host or all(c.isdigit() or c == "." for c in host):
            return ""
        infos = socket.getaddrinfo(host, u.port or 80, socket.AF_INET, socket.SOCK_STREAM)
        if not infos:
            return ""
        port = f":{u.port}" if u.port else ""
        return f"http://{infos[0][4][0]}{port}"
    except Exception:
        return ""


def push_device(dev: dict, snap: dict, display: dict, cfg: dict) -> None:
    body = dict(snap)
    body["display"] = display
    body["cmd"] = dev.get("pending")
    token = str(dev.get("token", ""))
    pend0 = dev.get("pending")
    if pend0:
        log(f"push cmd seq={pend0.get('seq')} {pend0.get('type')} -> {dev.get('id')}")

    primary = dev.get("url", "").rstrip("/")
    bases = [primary]
    cached = str(dev.get("ip_url") or "").rstrip("/")
    if cached and cached != primary:
        bases.append(cached)

    def handle(res: dict, base: str) -> None:
        dev["online"] = True
        dev["last_seen"] = int(time.time())
        if base != primary:
            dev["ip_url"] = base
        if isinstance(res.get("state"), dict):
            dev["state"] = res["state"]
        pend = dev.get("pending")
        if pend and (res.get("ack") == pend.get("seq")
                     or pend.get("type") in ("reboot", "factory")):
            dev["pending"] = None
            log(f"ack seq={res.get('ack')} page={dev.get('state', {}).get('page')}")

    last_exc = None
    for base in bases:
        try:
            handle(_post_snapshot(base, body, token), base)
            return
        except Exception as exc:
            last_exc = exc

    alt = _resolve_ip_base(primary)
    if alt and alt not in bases:
        try:
            log(f"mdns fallback {primary} -> {alt}")
            handle(_post_snapshot(alt, body, token), alt)
            return
        except Exception as exc:
            last_exc = exc

    dev["online"] = False
    dev["error"] = f"{type(last_exc).__name__}: {last_exc}" if last_exc else "push failed"


def pusher_loop(cfg_holder: dict) -> None:
    last_save = 0.0
    while True:
        try:
            with CFG_LOCK:
                cfg = json.loads(json.dumps(cfg_holder))
                # 直接拿 live 设备对象的引用: 网页刚下的命令不会被旧拷贝覆盖
                devices = list(cfg_holder.get("devices", []))
            snap = build_snapshot(cfg)
            CACHED_SNAP.clear()
            CACHED_SNAP.update(snap)
            display = cfg["display"]
            if devices:
                threads = []
                for dev in devices:
                    th = threading.Thread(target=push_device, args=(dev, snap, display, cfg),
                                          daemon=True)
                    th.start()
                    threads.append(th)
                for th in threads:
                    th.join(timeout=3.5)
            now = time.time()
            if devices and now - last_save > 10:
                with CFG_LOCK:
                    save_cfg(cfg_holder)
                last_save = now
        except Exception as exc:
            log(f"push loop error: {type(exc).__name__}: {exc}")
        time.sleep(max(2, int(cfg_holder.get("push_seconds", 5))))


# ---------------------------------------------------------------- Web 控制台
INDEX_HTML = r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>oMLX Mate 控制台</title>
<style>
:root{color-scheme:dark;--bg:#0E1614;--surf:#131D1B;--surf2:#1A2523;--bd:#25332F;
--tx:#E8F1EE;--tx2:#BAC9C5;--mut:#8FA3A0;--teal:#0CA678;--teal2:#12B886;--amber:#F59F00;--red:#E03131}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:14px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:1020px;margin:0 auto;padding:22px 18px 60px}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:18px}
.logo{width:34px;height:34px;border-radius:10px;background:linear-gradient(135deg,#12B886,#0CA678);display:grid;place-items:center;font-weight:800;color:#04241C}
h1{font-size:19px;margin:0;font-weight:700}
.sub{color:var(--mut);font-size:12px}
.pills{margin-left:auto;display:flex;gap:8px;flex-wrap:wrap}
.pill{font-size:12px;padding:5px 10px;border-radius:20px;background:var(--surf);border:1px solid var(--bd);color:var(--tx2)}
.pill b{color:var(--teal2)}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--red);margin-right:6px}
.dot.on{background:var(--teal2)}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:16px}
.card{background:var(--surf);border:1px solid var(--bd);border-radius:14px;padding:16px}
.kpi .lab{color:var(--mut);font-size:12px}
.kpi .val{font-size:26px;font-weight:750;color:var(--teal2);margin-top:4px;letter-spacing:-.02em}
.kpi .sm{color:var(--tx2);font-size:12px;margin-top:2px}
section.card{margin-bottom:16px}
h2{font-size:14px;margin:0 0 12px;color:var(--tx2);font-weight:650;letter-spacing:.02em}
canvas#chart{width:100%;height:130px;display:block}
.rows{display:flex;flex-direction:column;gap:8px}
.dev{display:flex;align-items:center;gap:12px;padding:12px;border:1px solid var(--bd);border-radius:12px;background:var(--surf2);flex-wrap:wrap}
.dev .nm{font-weight:650;min-width:110px}
.dev .meta{color:var(--mut);font-size:12px;display:flex;gap:12px;flex-wrap:wrap}
.btn{border:1px solid var(--bd);background:var(--surf);color:var(--tx);padding:7px 12px;border-radius:9px;font-size:12px;cursor:pointer}
.btn:hover{border-color:var(--teal)}
.btn.primary{background:var(--teal);border-color:var(--teal);color:#04241C;font-weight:700}
.btn.danger{color:#ff8f8f}
.page-row{display:grid;grid-template-columns:24px 1.15fr .9fr 1fr 74px auto;gap:8px;align-items:center;padding:8px;border:1px solid var(--bd);border-radius:10px;margin-bottom:8px;background:var(--surf2)}
.page-row.sel{outline:1px solid var(--teal)}
select,input{background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:8px;padding:7px 9px;font-size:12px;width:100%}
input[type=checkbox]{width:auto;margin:0;accent-color:#0CA678;flex:0 0 auto}
.page-row .chk{white-space:nowrap}
label.chk{display:flex;gap:6px;align-items:center;font-size:12px;color:var(--tx2)}
.two{display:grid;grid-template-columns:1.05fr .95fr;gap:18px;align-items:start}
#preview{width:240px;height:135px;image-rendering:pixelated;border-radius:8px;border:1px solid var(--bd);display:block;margin:6px auto 0}
.settings{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}
.settings label{font-size:11px;color:var(--mut);display:block;margin-bottom:4px}
.actions{display:flex;gap:10px;align-items:center;margin-top:12px;flex-wrap:wrap}
.hint{color:var(--mut);font-size:12px}
pre#logs{background:#0A100F;border:1px solid var(--bd);border-radius:10px;padding:12px;height:200px;overflow:auto;font:11px/1.5 ui-monospace,Menlo,monospace;color:#9fb3ad;white-space:pre-wrap}
code{background:var(--surf2);padding:2px 6px;border-radius:5px;font-size:12px}
.toast{position:fixed;right:18px;bottom:18px;background:var(--teal);color:#04241C;padding:10px 16px;border-radius:10px;font-weight:650;opacity:0;transition:.25s;pointer-events:none}
.toast.on{opacity:1}
@media(max-width:760px){.grid{grid-template-columns:repeat(2,1fr)}.two{grid-template-columns:1fr}.settings{grid-template-columns:1fr}}
</style></head><body><div class="wrap">
<header>
 <div class="logo">o</div>
 <div><h1>oMLX Mate</h1><div class="sub">本地推理 · 省了多少 · 设备控制台</div></div>
 <div class="pills"><span class="pill" id="p-omlx"><span class="dot"></span>oMLX</span>
 <span class="pill" id="p-port">:--</span><span class="pill" id="p-dev">0 台设备</span></div>
</header>

<div class="grid">
 <div class="card kpi"><div class="lab">今日已省</div><div class="val" id="k-net">¥--</div><div class="sm" id="k-api"></div></div>
 <div class="card kpi"><div class="lab">今日 Tokens</div><div class="val" id="k-tok">--</div><div class="sm" id="k-req"></div></div>
 <div class="card kpi"><div class="lab">近 7 天已省</div><div class="val" id="k-7">¥--</div><div class="sm" id="k-hit"></div></div>
 <div class="card kpi"><div class="lab">当前模型</div><div class="val" id="k-model" style="font-size:15px;line-height:1.35;padding-top:8px">--</div><div class="sm" id="k-peak"></div></div>
</div>

<section class="card"><h2>近 30 天每日净省</h2><canvas id="chart"></canvas></section>

<section class="card"><h2>显示配置 <span class="hint">（全局生效，设备 A/B 翻页）</span></h2>
<div class="two">
<div><div id="pages"></div>
<div class="actions"><button class="btn" id="addPage">+ 添加页面</button>
<button class="btn primary" id="saveDisplay">保存并下发</button>
<span class="hint" id="dirty"></span></div></div>
<div><div class="hint" style="text-align:center">设备预览</div><canvas id="preview" width="240" height="135"></canvas>
<div class="hint" style="text-align:center;margin-top:8px">点左侧页面行可预览</div></div>
</div></section>

<section class="card"><h2>设备</h2><div class="rows" id="devices"></div>
<div class="hint" style="margin-top:10px">添加设备：在 StickS3 屏幕显示的地址上执行
<code>curl -sL http://&lt;设备&gt;/setup.sh | bash</code></div></section>

<section class="card"><h2>设置</h2><div class="settings" id="settings"></div>
<div class="actions"><button class="btn primary" id="saveSettings">保存设置</button>
<span class="hint" id="setNote"></span></div></section>

<section class="card"><h2>日志</h2><div class="actions"><button class="btn" id="loadLogs">刷新日志</button></div><pre id="logs">点“刷新日志”查看</pre></section>
</div><div class="toast" id="toast"></div>

<script>
const CODE = new URLSearchParams(location.search).get('code') || '';
let OV = null, DRAFT = null, PREVIEW = 0, PAGES_INIT = false, SETTINGS_INIT = false;
const TYPES = {savings:'省钱', usage:'用量', trend:'趋势', models:'模型', cost:'成本', device:'设备'};
const CHART = {d7:'7 日柱', d30:'30 日柱', hourly:'今日小时', none:'无'};
const RANGE = {today:'今日', yesterday:'昨日', d7:'近 7 天', d30:'近 30 天'};

function toast(t){const e=document.getElementById('toast');e.textContent=t;e.classList.add('on');setTimeout(()=>e.classList.remove('on'),1800)}
async function api(path, body){
  const opt = {headers:{'X-Mate-Code':CODE}};
  if(body!==undefined){opt.method='POST';opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(body)}
  const r = await fetch(path+ (CODE?('?code='+encodeURIComponent(CODE)):''), opt);
  if(!r.ok) throw new Error('HTTP '+r.status);
  return await r.json();
}
function money(v){ if(v===undefined||v===null) return '--'; return '¥'+(Math.abs(v)>=1000? (v/1000).toFixed(1)+'k' : v.toFixed(2)); }
function toks(v){ if(!v) return '0'; if(v>=1e9) return (v/1e9).toFixed(2)+'B'; if(v>=1e6) return (v/1e6).toFixed(1)+'M'; if(v>=1e3) return (v/1e3).toFixed(0)+'K'; return String(v); }

async function refresh(){
  try{
    OV = await api('/api/overview');
    if(!DRAFT){ DRAFT = JSON.parse(JSON.stringify(OV.display)); PAGES_INIT = false; }
    renderTop(); renderChart(); renderDevices();
    if(!PAGES_INIT){ renderPages(); PAGES_INIT = true; }
    if(!SETTINGS_INIT){ renderSettings(); SETTINGS_INIT = true; }
    document.getElementById('p-port').textContent = ':'+OV.agent.port;
    document.getElementById('p-dev').textContent = OV.devices.length+' 台设备';
    const on = OV.snapshot.omlx && OV.snapshot.omlx.up;
    const pd = document.getElementById('p-omlx'); pd.innerHTML = '<span class="dot'+(on?' on':'')+'"></span>oMLX '+(on?'在线':'离线');
  }catch(e){ document.getElementById('setNote').textContent = '控制台连接失败: '+e; }
}

function renderTop(){
  const m = OV.snapshot.metrics || {}; const t = m.today||{}, d7=m.d7||{};
  document.getElementById('k-net').textContent = money(t.net);
  document.getElementById('k-api').textContent = 'API '+money(t.api)+' · 电费 '+money(t.elec);
  document.getElementById('k-tok').textContent = toks(t.tok);
  document.getElementById('k-req').textContent = (t.req||0)+' 次 · 命中 '+((t.hit||0)*100).toFixed(1)+'%';
  document.getElementById('k-7').textContent = money(d7.net);
  document.getElementById('k-hit').textContent = '近 7 天 '+toks(d7.tok)+' tokens';
  const om = OV.snapshot.omlx||{};
  document.getElementById('k-model').textContent = om.model || '未加载';
  document.getElementById('k-peak').textContent = OV.snapshot.peak? '当前峰时价':'当前空闲价';
}

function renderChart(){
  const c=document.getElementById('chart'), r=c.getBoundingClientRect();
  c.width=Math.max(320,r.width*devicePixelRatio); c.height=130*devicePixelRatio;
  const x=c.getContext('2d'), W=c.width, H=c.height, pad=18*devicePixelRatio;
  x.clearRect(0,0,W,H);
  const s=(OV.snapshot.series&&OV.snapshot.series.d30)||[]; const mx=Math.max(...s,0.01);
  const bw=(W-pad*2)/30;
  x.fillStyle='#12B886';
  s.forEach((v,i)=>{const h=(v/mx)*(H-pad*2); x.fillRect(pad+i*bw+bw*0.18, H-pad-h, bw*0.64, h)});
  x.strokeStyle='#25332F'; x.beginPath(); x.moveTo(pad,H-pad); x.lineTo(W-pad,H-pad); x.stroke();
  x.fillStyle='#8FA3A0'; x.font=(10*devicePixelRatio)+'px system-ui';
  x.fillText('30 天前', pad, H-4*devicePixelRatio);
  x.fillText('今天', W-pad-30*devicePixelRatio, H-4*devicePixelRatio);
}

function renderDevices(){
  const box=document.getElementById('devices');
  if(!OV.devices.length){ box.innerHTML='<div class="hint">还没有设备。在设备屏幕显示的地址上执行安装命令即可加入。</div>'; return; }
  box.innerHTML = OV.devices.map(d=>{
    const st=d.state||{}; const on=d.online&&(Date.now()/1000-(d.last_seen||0)<20);
    return `<div class="dev"><span class="dot ${on?'on':''}"></span>
      <span class="nm">${d.name||d.id}</span>
      <span class="meta"><span>${on?'在线':'离线'}</span><span>电量 ${st.battery??'--'}%</span>
      <span>WiFi ${st.rssi??'--'}dBm</span><span>页面 ${st.page??'--'}</span><span>fw ${st.fw||'--'}</span></span>
      <span style="margin-left:auto;display:flex;gap:6px">
      <button class="btn" onclick="cmd('${d.id}','prev')">‹</button>
      <button class="btn" onclick="cmd('${d.id}','next')">›</button>
      <button class="btn" onclick="cmd('${d.id}','identify')">闪烁</button>
      <button class="btn" onclick="cmd('${d.id}','reboot')">重启</button>
      <button class="btn danger" onclick="removeDev('${d.id}')">移除</button></span></div>`}).join('');
}

function renderPages(){
  const box=document.getElementById('pages');
  const CHART_OK=['savings','usage','trend'];
  const RANGE_OK=['savings','usage','trend','cost'];
  box.innerHTML = DRAFT.pages.map((p,i)=>`
    <div class="page-row ${i===PREVIEW?'sel':''}" onclick="selectRow(${i})">
      <div class="hint">${i+1}</div>
      <select onchange="edit(${i},'type',this.value)">${Object.entries(TYPES).map(([k,v])=>`<option value="${k}" ${p.type===k?'selected':''}>${v}</option>`).join('')}</select>
      ${RANGE_OK.includes(p.type)?`<select onchange="edit(${i},'range',this.value)">${Object.entries(RANGE).map(([k,v])=>`<option value="${k}" ${p.range===k?'selected':''}>${v}</option>`).join('')}</select>`:`<div class="hint">—</div>`}
      ${CHART_OK.includes(p.type)?`<select onchange="edit(${i},'chart',this.value)">${Object.entries(CHART).map(([k,v])=>`<option value="${k}" ${p.chart===k?'selected':''}>${v}</option>`).join('')}</select>`:`<div class="hint">—</div>`}
      <label class="chk"><input type="checkbox" ${p.dense?'checked':''} onchange="edit(${i},'dense',this.checked)">紧凑</label>
      <span><button class="btn" onclick="event.stopPropagation();move(${i},-1)">↑</button>
      <button class="btn" onclick="event.stopPropagation();move(${i},1)">↓</button>
      <button class="btn danger" onclick="event.stopPropagation();del(${i})">×</button></span>
    </div>`).join('');
  drawPreview();
}
function edit(i,k,v){DRAFT.pages[i][k]=v;if(k==='type'&&!['savings','usage','trend'].includes(v))DRAFT.pages[i].chart='none';document.getElementById('dirty').textContent='未保存';if(k==='type')renderPages();else drawPreview()}
function selectRow(i){PREVIEW=i;document.querySelectorAll('.page-row').forEach((e,k)=>e.classList.toggle('sel',k===i));drawPreview()}
function move(i,d){const j=i+d;if(j<0||j>=DRAFT.pages.length)return;[DRAFT.pages[i],DRAFT.pages[j]]=[DRAFT.pages[j],DRAFT.pages[i]];PREVIEW=j;renderPages()}
function del(i){if(DRAFT.pages.length<=1)return;DRAFT.pages.splice(i,1);PREVIEW=0;renderPages()}

function drawPreview(){
  const c=document.getElementById('preview'), x=c.getContext('2d');
  const p=DRAFT.pages[PREVIEW]||{type:'savings',range:'today',chart:'d7'};
  const m=(OV.snapshot.metrics||{})[p.range==='yesterday'?'y':p.range]||{};
  const s=OV.snapshot.series||{};
  x.fillStyle='#0E1614';x.fillRect(0,0,240,135);
  x.fillStyle='#131D1B';x.fillRect(0,0,240,18);
  x.fillStyle='#12B886';x.beginPath();x.arc(9,9,3,0,7);x.fill();
  x.font='bold 10px -apple-system,PingFang SC';x.fillStyle='#BAC9C5';x.fillText('oMLX Mate',15,12);
  x.fillStyle='#0E1614';x.beginPath();x.roundRect(104,3,34,13,6);x.fillStyle=OV.snapshot.peak?'#F59F00':'#0CA678';x.fill();
  x.fillStyle='#0E1614';x.font='bold 9px system-ui';x.fillText(OV.snapshot.peak?'PEAK':'OFF',110,12);
  const title={savings:p.range==='yesterday'?'昨日已省':'今日已省',usage:'今日用量',trend:p.range==='d30'?'近 30 天已省':'近 7 天已省',models:'模型用量 · 近 7 天',cost:'成本构成',device:'设备与链路'}[p.type]||'';
  x.fillStyle='#BAC9C5';x.font='bold 12px -apple-system,PingFang SC';x.fillText(title,10,32);
  let hero='';
  if(p.type==='savings'||p.type==='trend') hero=money(m.net);
  else if(p.type==='usage') hero=toks(m.tok);
  if(hero){
    x.fillStyle='#12B886';x.font='bold 30px -apple-system';x.fillText(hero,9,68);
  }
  x.strokeStyle='#25332F';x.beginPath();x.moveTo(10,73);x.lineTo(230,73);x.stroke();
  x.fillStyle='#BAC9C5';x.font='11px -apple-system,PingFang SC';
  if(p.type==='savings'||p.type==='trend'||p.type==='usage'){
    x.fillText(p.type==='usage'?('请求 '+m.req):('API '+money(m.api)+'  ·  电费 '+money(m.elec)),10,86);
    x.fillStyle='#8FA3A0';
    x.fillText(p.type==='usage'?('命中 '+((m.hit||0)*100).toFixed(1)+'%  ·  '+toks(m.tok)+' tok'):((m.req||0)+' 次  ·  命中 '+((m.hit||0)*100).toFixed(1)+'%'),10,101);
    if(p.chart!=='none'){
      const arr=p.chart==='hourly'?s.hourly:p.chart==='d30'?s.d30:s.d7; const n=arr.length;
      const mx=Math.max(...arr,0.01); const bw=220/n;
      arr.forEach((v,i)=>{const h=(v/mx)*14;x.fillStyle=i===n-1?'#12B886':'#0A8C';x.fillRect(10+i*bw+bw*0.12,120-h,bw*0.7,h)});
    }
  } else if(p.type==='models'){
    const ms=OV.snapshot.models||[];
    ms.slice(0,4).forEach((m,i)=>{const y=46+i*20;x.fillStyle='#E8F1EE';x.font='11px -apple-system';x.fillText((m.n||'').slice(0,22),10,y);x.fillStyle='#12B886';x.fillText(money(m.c),196,y);x.fillStyle='#1A2523';x.fillRect(10,y+4,220,3);x.fillStyle='#0CA678';x.fillRect(10,y+4,220*(m.c/(ms[0].c||1)),3)});
  } else if(p.type==='cost'){
    const L=['云账单','电费','缓存省','净省'],V=[m.api,m.elec,m.cs,m.net];
    V.forEach((v,i)=>{const cx=10+(i%2)*116, cy=42+Math.floor(i/2)*34;x.fillStyle='#131D1B';x.beginPath();x.roundRect(cx,cy,106,30,6);x.fill();x.fillStyle='#8FA3A0';x.font='10px -apple-system,PingFang SC';x.fillText(L[i],cx+7,cy+11);x.fillStyle=i===3?'#12B886':'#E8F1EE';x.font='bold 14px -apple-system';x.fillText(money(v),cx+7,cy+26)});
  } else if(p.type==='device'){
    x.fillStyle='#8FA3A0';x.font='11px -apple-system,PingFang SC';
    const rows=[['WiFi',OV.devices[0]?.state?.ssid||'--'],['桥接',OV.devices[0]?.online?'在线':'离线'],['oMLX',(OV.snapshot.omlx||{}).ver||'--'],['模型',(OV.snapshot.omlx||{}).model||'--']];
    rows.forEach((r,i)=>x.fillText(r[0]+'   '+r[1],10,48+i*18));
  }
  x.fillStyle='#25332F';x.fillRect(0,121,240,1);
  x.fillStyle='#6C7F7D';x.font='10px -apple-system,PingFang SC';x.fillText('A 换页',6,133);
}

function renderSettings(){
  const s=OV.settings, p=s.pricing;
  document.getElementById('settings').innerHTML=`
   <div><label>对标模型</label><select id="s-model"><option value="flash" ${p.ds_model==='flash'?'selected':''}>deepseek-flash</option><option value="pro" ${p.ds_model==='pro'?'selected':''}>deepseek-v4-pro</option></select></div>
   <div><label>币种</label><select id="s-cur"><option value="CNY" ${p.currency==='CNY'?'selected':''}>CNY ¥</option><option value="USD" ${p.currency==='USD'?'selected':''}>USD $</option></select></div>
   <div><label>电价 ¥/kWh</label><input id="s-elec" type="number" step="0.01" value="${p.elec_price}"></div>
   <div><label>设备折旧价 ¥</label><input id="s-dev" type="number" step="500" value="${p.device_cny}"></div>
   <div><label>预填充功率 W</label><input id="s-pre" type="number" value="${p.power_prefill_w}"></div>
   <div><label>生成功率 W</label><input id="s-gen" type="number" value="${p.power_decode_w}"></div>
   <div><label>推送间隔 秒</label><input id="s-push" type="number" min="2" max="60" value="${s.push_seconds}"></div>
   <div><label>设备语言</label><select id="s-lang"><option value="0" ${s.lang==0?'selected':''}>中文</option><option value="1" ${s.lang==1?'selected':''}>English</option></select></div>
   <div><label class="chk"><input type="checkbox" id="s-peak" ${p.peak?'checked':''}>启用峰谷计价</label></div>
   <div><label class="chk"><input type="checkbox" id="s-lan" ${s.lan_access?'checked':''}>允许局域网打开控制台</label></div>
   <div><label>局域网访问码</label><input id="s-code" value="${s.lan_code||''}" placeholder="留空自动生成"></div>
   <div><label>控制台端口</label><input id="s-port" type="number" value="${s.port}"></div>`;
}

document.getElementById('saveDisplay').onclick=async()=>{
  DRAFT.version=(DRAFT.version||1)+1;
  await api('/api/display',{display:DRAFT}); document.getElementById('dirty').textContent='已保存'; toast('显示配置已下发');
};
document.getElementById('addPage').onclick=()=>{DRAFT.pages.push({type:'savings',range:'today',chart:'d7',dense:false});PREVIEW=DRAFT.pages.length-1;renderPages()};
document.getElementById('saveSettings').onclick=async()=>{
  const s={pricing:{ds_model:v('s-model'),currency:v('s-cur'),peak:document.getElementById('s-peak').checked,
    elec_price:+v('s-elec'),device_cny:+v('s-dev'),power_prefill_w:+v('s-pre'),power_decode_w:+v('s-gen')},
    push_seconds:+v('s-push'),lang:+v('s-lang'),lan_access:document.getElementById('s-lan').checked,
    lan_code:v('s-code'),port:+v('s-port')};
  const r=await api('/api/settings',{settings:s});
  document.getElementById('setNote').textContent=r.restart?'部分设置需要重启控制台生效':'已保存';toast('设置已保存');
};
function v(id){return document.getElementById(id).value}
async function cmd(id,type){await api('/api/device/cmd',{id,type});toast('已发送 '+type);refresh()}
async function removeDev(id){if(!confirm('移除这台设备？'))return;await api('/api/device/remove',{id});refresh()}
document.getElementById('loadLogs').onclick=async()=>{const r=await api('/api/logs');document.getElementById('logs').textContent=r.text||'(空)'};

refresh(); setInterval(()=>{if(!document.hidden)refresh()},4000);
</script></body></html>"""


# ---------------------------------------------------------------- HTTP 服务
class Handler(BaseHTTPRequestHandler):
    server_version = f"oMLXMateAgent/{VERSION}"

    def log_message(self, fmt, *args):
        pass

    def _authed(self) -> bool:
        ip = self.client_address[0]
        if ip in ("127.0.0.1", "::1", "::ffff:127.0.0.1"):
            return True
        with CFG_LOCK:
            if not CFG.get("lan_access"):
                return False
            code = self.headers.get("X-Mate-Code", "")
            if not code:
                code = parse_qs(urlparse(self.path).query).get("code", [""])[0]
            return bool(code) and code == CFG.get("lan_code")

    def _send(self, code: int, body: bytes, ctype: str):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, obj, code: int = 200):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8")

    def _body(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length", "0"))
            return json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except Exception:
            return {}

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            if not self._authed():
                return self._send(401, b"need code", "text/plain")
            return self._send(200, INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8")
        if not self._authed():
            return self._json({"ok": False, "error": "需要访问码"}, 401)
        if path == "/api/overview":
            with CFG_LOCK:
                cfg = json.loads(json.dumps(CFG))
            snap = CACHED_SNAP or build_snapshot(cfg)
            cfg["devices"] = [device_public(d) for d in cfg.get("devices", [])]
            return self._json({
                "ok": 1,
                "agent": {"version": VERSION, "port": ACTUAL_PORT,
                          "uptime": int(time.time() - STARTED_AT),
                          "lan": cfg.get("lan_access"), "label": LABEL},
                "snapshot": snap,
                "devices": cfg["devices"],
                "display": cfg["display"],
                "settings": {"pricing": cfg["pricing"], "push_seconds": cfg["push_seconds"],
                             "lang": cfg["lang"], "lan_access": cfg["lan_access"],
                             "lan_code": cfg["lan_code"], "port": cfg["port"]},
            })
        if path == "/api/logs":
            try:
                text = LOG_PATH.read_text(encoding="utf-8", errors="replace")[-20000:]
            except Exception:
                text = "(no log yet)"
            return self._json({"ok": 1, "text": text})
        return self._json({"ok": False, "error": "not found"}, 404)

    def do_POST(self):
        if not self._authed():
            return self._json({"ok": False, "error": "需要访问码"}, 401)
        path = urlparse(self.path).path
        body = self._body()
        with CFG_LOCK:
            if path == "/api/display":
                CFG["display"] = sanitize_display(body.get("display"))
                save_cfg(CFG)
                return self._json({"ok": 1})
            if path == "/api/settings":
                s = body.get("settings") or {}
                restart = False
                if "pricing" in s:
                    p = CFG["pricing"]
                    for k in ("ds_model", "currency", "peak", "power_prefill_w",
                              "power_decode_w", "elec_price", "device_cny", "depreciate_days"):
                        if k in s["pricing"]:
                            p[k] = s["pricing"][k]
                for k in ("push_seconds", "lang", "lan_access", "lan_code", "port"):
                    if k in s and s[k] != CFG.get(k):
                        if k in ("lan_access", "port", "lan_code"):
                            restart = True
                        CFG[k] = s[k]
                if CFG["lan_access"] and not CFG["lan_code"]:
                    CFG["lan_code"] = str(int(time.time()))[-6:]
                save_cfg(CFG)
                if restart:
                    threading.Thread(target=lambda: (time.sleep(0.6), os._exit(0)),
                                     daemon=True).start()
                return self._json({"ok": 1, "restart": restart})
            if path == "/api/device/cmd":
                dev = self._find(body.get("id"))
                if not dev:
                    return self._json({"ok": False, "error": "device not found"}, 404)
                typ = str(body.get("type") or "")
                seq = int(dev.get("cmd_seq", 0)) + 1
                dev["cmd_seq"] = seq
                dev["pending"] = {"seq": seq, "type": typ,
                                  "value": body.get("value")}
                save_cfg(CFG)
                log(f"cmd {typ} -> {dev.get('name') or dev.get('id')}")
                return self._json({"ok": 1, "seq": seq})
            if path == "/api/device/remove":
                before = len(CFG["devices"])
                CFG["devices"] = [d for d in CFG["devices"] if d.get("id") != body.get("id")]
                save_cfg(CFG)
                return self._json({"ok": 1, "removed": before - len(CFG["devices"])})
            if path == "/api/device/rename":
                dev = self._find(body.get("id"))
                if not dev:
                    return self._json({"ok": False}, 404)
                dev["name"] = str(body.get("name") or dev.get("name"))[:24]
                save_cfg(CFG)
                return self._json({"ok": 1})
        return self._json({"ok": False, "error": "not found"}, 404)

    def _find(self, dev_id):
        for d in CFG.get("devices", []):
            if d.get("id") == dev_id:
                return d
        return None


# ---------------------------------------------------------------- 安装/服务
def add_device(url: str, token: str, name: str) -> dict:
    cfg = load_cfg()
    url = url.rstrip("/")
    dev_id = ""
    try:
        host = urlparse(url).hostname or ""
        dev_id = host.split(".")[0]
        if not dev_id:
            dev_id = url
    except Exception:
        dev_id = url
    dev = {"id": dev_id, "name": name or dev_id, "url": url, "token": token,
           "added": int(time.time()), "online": False, "last_seen": 0,
           "state": {}, "pending": None, "cmd_seq": 0}
    cfg["devices"] = [d for d in cfg.get("devices", []) if d.get("id") != dev_id]
    cfg["devices"].append(dev)
    save_cfg(cfg)
    return dev


def write_plist() -> Path:
    python = sys.executable or "/usr/bin/python3"
    agent = MATE_DIR / "agent.py"
    MATE_DIR.mkdir(parents=True, exist_ok=True)
    PLIST_PATH.parent.mkdir(parents=True, exist_ok=True)
    plist = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>{LABEL}</string>
  <key>ProgramArguments</key><array>
    <string>{python}</string><string>{agent}</string><string>--serve</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>WorkingDirectory</key><string>{MATE_DIR}</string>
  <key>StandardOutPath</key><string>{MATE_DIR}/agent.out.log</string>
  <key>StandardErrorPath</key><string>{MATE_DIR}/agent.err.log</string>
</dict></plist>
"""
    PLIST_PATH.write_text(plist, encoding="utf-8")
    return PLIST_PATH


def find_port(preferred: int, host: str) -> int:
    for p in range(int(preferred), int(preferred) + 40):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, p))
                return p
            except OSError:
                continue
    raise RuntimeError("no free port")


def serve(args) -> None:
    global CFG, ACTUAL_PORT
    MATE_DIR.mkdir(parents=True, exist_ok=True)
    CFG = load_cfg()
    if args.port:
        CFG["port"] = args.port
    if args.lan is not None:
        CFG["lan_access"] = args.lan
    host = "0.0.0.0" if CFG.get("lan_access") else "127.0.0.1"
    port = find_port(CFG.get("port", 8788), host)
    ACTUAL_PORT = port
    if port != CFG.get("port"):
        log(f"port {CFG.get('port')} busy, using {port}")
        CFG["port"] = port
        save_cfg(CFG)
    threading.Thread(target=pusher_loop, args=(CFG,), daemon=True).start()
    httpd = ThreadingHTTPServer((host, port), Handler)
    try:
        (MATE_DIR / "url.txt").write_text(f"http://127.0.0.1:{port}\n", encoding="utf-8")
    except Exception:
        pass
    log(f"{APP_NAME} v{VERSION} http://{host}:{port}  devices={len(CFG.get('devices', []))}")
    if CFG.get("lan_access"):
        log(f"LAN access ON, code={CFG.get('lan_code')}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("bye")


def once() -> None:
    global CFG
    CFG = load_cfg()
    snap = build_snapshot(CFG)
    print(json.dumps({k: snap[k] for k in ("ok", "srv", "peak", "metrics", "series", "models", "omlx", "err")},
                     ensure_ascii=False, indent=2)[:4000])
    for dev in CFG.get("devices", []):
        push_device(dev, snap, CFG["display"], CFG)
        print(f"push {dev.get('name')} -> {'ok' if dev.get('online') else dev.get('error', 'fail')}")


def main() -> None:
    ap = argparse.ArgumentParser(description=APP_NAME)
    ap.add_argument("--serve", action="store_true")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--lan", dest="lan", action="store_true", default=None)
    ap.add_argument("--version", action="store_true")
    ap.add_argument("--add-device", nargs=3, metavar=("URL", "TOKEN", "NAME"))
    ap.add_argument("--write-plist", action="store_true")
    args = ap.parse_args()

    if args.version:
        print(VERSION)
        return
    if args.add_device:
        dev = add_device(*args.add_device)
        print(json.dumps(dev, ensure_ascii=False))
        return
    if args.write_plist:
        print(write_plist())
        return
    if args.once:
        once()
        return
    serve(args)


if __name__ == "__main__":
    main()
