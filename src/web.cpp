#include "web.h"

#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WebServer.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "assets.h"
#include "config.h"
#include "display.h"
#include "net.h"
#include "snapshot.h"
#include "store.h"

static WebServer server(HTTP_PORT);
static bool s_running = false;

// ---------------------------------------------------------------- 解析
static void parseRange(JsonVariantConst v, RangeData& r) {
  if (v.isNull()) return;
  r.valid = true;
  r.net = v["net"] | 0.0f;
  r.api = v["api"] | 0.0f;
  r.elec = v["elec"] | 0.0f;
  r.cs = v["cs"] | 0.0f;
  r.pk = v["pk"] | 0.0f;
  r.hit = v["hit"] | 0.0f;
  r.tok = v["tok"] | static_cast<uint64_t>(0);
  r.req = v["req"] | static_cast<uint32_t>(0);
}

static void parseSeries(JsonVariantConst v, float* dst, int n) {
  int i = 0;
  for (JsonVariantConst x : v.as<JsonArrayConst>()) {
    if (i >= n) break;
    dst[i++] = x.as<float>();
  }
}

static bool parseSnapshot(JsonDocument& doc, Snapshot& s, DisplayConfig& dc, int& ack) {
  JsonVariantConst metrics = doc["metrics"];
  if (metrics.isNull()) return false;

  Snapshot n;
  n.ok = (doc["ok"] | 0) == 1;
  n.ts = doc["ts"] | 0;
  std::strncpy(n.srv, doc["srv"] | "--:--", sizeof(n.srv) - 1);
  n.peak = (doc["peak"] | 0) == 1;
  parseRange(metrics["today"], n.today);
  parseRange(metrics["y"], n.y);
  parseRange(metrics["d7"], n.d7);
  parseRange(metrics["d30"], n.d30);
  parseSeries(doc["series"]["d7"], n.series7, 7);
  parseSeries(doc["series"]["d30"], n.series30, 30);
  parseSeries(doc["series"]["hourly"], n.hourly, 24);

  for (JsonVariantConst m : doc["models"].as<JsonArrayConst>()) {
    if (n.modelCount >= 5) break;
    ModelRow& row = n.models[n.modelCount++];
    std::strncpy(row.name, m["n"] | "unknown", sizeof(row.name) - 1);
    row.cost = m["c"] | 0.0f;
    row.tok = m["t"] | static_cast<uint64_t>(0);
    row.req = m["r"] | static_cast<uint32_t>(0);
  }

  JsonObjectConst om = doc["omlx"].as<JsonObjectConst>();
  n.omlxUp = (om["up"] | 0) == 1;
  std::strncpy(n.omlxVer, om["ver"] | "", sizeof(n.omlxVer) - 1);
  std::strncpy(n.omlxModel, om["model"] | "", sizeof(n.omlxModel) - 1);
  std::strncpy(n.err, doc["err"] | "", sizeof(n.err) - 1);
  n.receivedAtMs = millis();

  DisplayConfig parsed;
  if (displayFromDoc(doc, parsed) && !displayEquals(parsed, dc)) {
    dc = parsed;
    displaySave(dc);
    g_needRender = true;
  }

  JsonVariantConst cmd = doc["cmd"];
  if (!cmd.isNull()) {
    ack = cmd["seq"] | 0;
    if (!ack || ack != g_lastCmdSeq) {
      g_lastCmdSeq = ack;
      const char* type = cmd["type"] | "";
      int value = cmd["value"] | 0;
      appApplyCommand(type, value);
    }
  }

  s = n;
  return true;
}

// ---------------------------------------------------------------- 页面/脚本
static String mateUrl() { return String("http://") + g_hostname + ".local"; }

static String setupScript() {
  String s(SETUP_SH);
  s.replace("__MATE_URL__", mateUrl());
  s.replace("__MATE_TOKEN__", g_cfg.token);
  s.replace("__MATE_NAME__", String("oMLX Mate ") + g_hostname);
  return s;
}

static String rootPage() {
  String html = F(
      "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>oMLX Mate</title><style>"
      "body{margin:0;background:#0E1614;color:#E8F1EE;font:15px/1.6 "
      "-apple-system,'PingFang SC',sans-serif;padding:28px 20px}"
      ".box{max-width:540px;margin:0 auto}.brand{display:flex;gap:10px;align-items:center}"
      ".dot{width:10px;height:10px;border-radius:50%;background:#12B886;"
      "box-shadow:0 0 12px #12B886}h1{font-size:20px;margin:0}"
      "p{color:#8FA3A0;font-size:13px}"
      ".cmd{background:#0A100F;border:1px solid #25332F;border-radius:12px;padding:14px;"
      "font:13px/1.5 ui-monospace,Menlo,monospace;color:#12B886;"
      "word-break:break-all;margin:14px 0}"
      "button{background:#0CA678;color:#04241C;border:0;border-radius:9px;"
      "padding:10px 16px;font-weight:700;font-size:14px}"
      ".kv{display:flex;justify-content:space-between;border-bottom:1px solid #1A2523;"
      "padding:8px 0;font-size:13px;color:#8FA3A0}.kv b{color:#BAC9C5;font-weight:500}"
      ".ok{color:#12B886}.wait{color:#F59F00}"
      "</style></head><body><div class=box>"
      "<div class=brand><span class=dot></span><h1>oMLX Mate</h1></div>"
      "<p>把这条命令粘贴到 Mac 终端，安装后台 agent。装好后屏幕自动开始显示。</p>"
      "<div class=cmd id=cmd>");
  html += "curl -sL ";
  html += mateUrl();
  html += F("/setup.sh | bash</div>"
            "<button onclick=\"copyCmd(this)\">复制命令</button>"
            "<div class=kv><span>设备</span><b>");
  html += g_hostname;
  html += F("</b></div><div class=kv><span>IP</span><b>");
  html += netIp();
  html += F("</b></div><div class=kv><span>固件</span><b>v" FW_VERSION "</b></div>"
            "<div class=kv><span>数据</span><b id=feed class=wait>等待 Mac 推送…</b></div>"
            "<p style='margin-top:18px'>卸载命令：<code>curl -sL ");
  html += mateUrl();
  html += F("/uninstall.sh | bash</code></p>"
            "</div><script>"
            "function copyCmd(b){const t=document.getElementById('cmd').textContent;"
            "const ok=()=>b.textContent='已复制';"
            "const fb=()=>{const a=document.createElement('textarea');a.value=t;"
            "a.style.position='fixed';a.style.opacity=0;document.body.appendChild(a);"
            "a.select();try{document.execCommand('copy');ok()}catch(e){b.textContent='请手动复制'}"
            "a.remove()};"
            "if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t)"
            ".then(ok).catch(fb)}else{fb()}}"
            "fetch('/api/status').then(r=>r.json()).then(d=>{const e=document.getElementById"
            "('feed');if(d.data){e.textContent='已连接';e.className='ok'}"
            "}).catch(()=>{});</script></body></html>");
  return html;
}

// ---------------------------------------------------------------- 路由
static void handleSnapshot() {
  if (g_cfg.token[0]) {
    String tok = server.header("X-Mate-Token");
    if (!tok.length()) tok = server.arg("k");
    if (tok != g_cfg.token) {
      Serial.printf("[WEB] token reject hdr='%s' q='%s' expect='%s'\n",
                    server.header("X-Mate-Token").c_str(),
                    server.arg("k").c_str(), g_cfg.token);
      server.send(401, "application/json", "{\"ok\":0,\"error\":\"token\"}");
      return;
    }
  }
  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  int ack = 0;
  if (err || !parseSnapshot(doc, g_snap, g_display, ack)) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"bad json\"}");
    return;
  }
  g_newSnapshot = true;
  g_needRender = true;
  if (g_page >= g_display.count) g_page = 0;

  char resp[340];
  std::snprintf(resp, sizeof(resp),
                "{\"ok\":1,\"ack\":%d,\"state\":{\"page\":%u,\"brightness\":%u,"
                "\"battery\":%d,\"charging\":%d,\"rssi\":%d,\"fw\":\"%s\","
                "\"uptime\":%u,\"pages\":%u,\"lang\":%u,\"host\":\"%s\"}}",
                ack, g_page, g_cfg.brightness, g_battery, g_charging ? 1 : 0,
                netRssi(), FW_VERSION, static_cast<unsigned>(millis() / 1000),
                g_display.count, g_cfg.lang, g_hostname);
  server.send(200, "application/json", resp);
}

static void handleStatus() {
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "{\"ok\":1,\"fw\":\"%s\",\"host\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                "\"page\":%u,\"pages\":%u,\"lang\":%u,\"data\":%d,\"age\":%u,"
                "\"uptime\":%u}",
                FW_VERSION, g_hostname, netIp(), netRssi(), g_page, g_display.count,
                g_cfg.lang, g_snap.ok ? 1 : 0,
                g_snap.receivedAtMs ? static_cast<unsigned>((millis() - g_snap.receivedAtMs) / 1000)
                                    : 0,
                static_cast<unsigned>(millis() / 1000));
  server.send(200, "application/json", buf);
}

void webBegin() {
  if (s_running) return;
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", rootPage());
  });
  server.on("/setup.sh", HTTP_GET, []() {
    server.send(200, "text/x-sh; charset=utf-8", setupScript());
  });
  server.on("/agent.py", HTTP_GET, []() {
    server.send_P(200, "text/x-python; charset=utf-8", AGENT_PY);
  });
  server.on("/uninstall.sh", HTTP_GET, []() {
    server.send_P(200, "text/x-sh; charset=utf-8", UNINSTALL_SH);
  });
  server.on("/api/snapshot", HTTP_POST, handleSnapshot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound([]() {
    server.send(404, "application/json", "{\"ok\":0,\"error\":\"not found\"}");
  });
  server.begin();
  s_running = true;
  Serial.printf("[WEB] listening :%d\n", HTTP_PORT);
}

void webLoop() {
  if (s_running) server.handleClient();
}

bool webIsRunning() { return s_running; }
