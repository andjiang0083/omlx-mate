#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstring>

#include "board.h"   // 板级按键判据
#include "config.h"
#include "ui.h"

static const char* AP_PASS = "omlxmate";
static const int MAX_NETS = 16;
static String s_nets[MAX_NETS];
static int s_netCount = 0;

static String esc(const String& in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c; break;
    }
  }
  return o;
}

static void scanNets() {
  s_netCount = 0;
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n && s_netCount < MAX_NETS; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (int k = 0; k < s_netCount; k++)
      if (s_nets[k] == ssid) dup = true;
    if (dup) continue;
    s_nets[s_netCount++] = ssid;
  }
  WiFi.scanDelete();
}

static String pageForm(const Config& cfg) {
  String opts;
  for (int i = 0; i < s_netCount; i++) opts += "<option value=\"" + esc(s_nets[i]) + "\">";
  String page;
  page.reserve(3600);
  page += F(
      "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>oMLX Mate 配网</title><style>"
      ":root{color-scheme:dark}*{box-sizing:border-box}"
      "body{margin:0;background:#0E1614;color:#E8F1EE;font:15px/1.5 -apple-system,"
      "'PingFang SC','Microsoft YaHei',sans-serif}"
      ".wrap{max-width:430px;margin:0 auto;padding:26px 18px 40px}"
      ".brand{display:flex;align-items:center;gap:10px}"
      ".dot{width:10px;height:10px;border-radius:50%;background:#12B886;"
      "box-shadow:0 0 12px #12B886}h1{font-size:20px;margin:0}"
      "p.sub{color:#8FA3A0;margin:6px 0 20px;font-size:13px}"
      "label{display:block;font-size:12px;color:#8FA3A0;margin:16px 0 6px}"
      "input,select{width:100%;padding:12px 13px;border-radius:10px;"
      "border:1px solid #25332F;background:#131D1B;color:#E8F1EE;font-size:15px;outline:none}"
      "input:focus,select:focus{border-color:#12B886}"
      ".hint{font-size:11px;color:#6C7F7D;margin-top:6px}"
      "button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:11px;"
      "background:#0CA678;color:#04241C;font-size:16px;font-weight:700}"
      ".card{background:#131D1B;border:1px solid #25332F;border-radius:14px;"
      "padding:14px;margin-top:20px;font-size:12px;color:#8FA3A0}"
      "</style></head><body><div class=wrap>"
      "<div class=brand><span class=dot></span><h1>oMLX Mate</h1></div>"
      "<p class=sub>第 1 步 / 2 · 连接家里的 Wi-Fi</p>"
      "<form method=post action=/save>"
      "<label>Wi-Fi 名称</label>"
      "<input name=ssid list=nets value='");
  page += esc(String(cfg.ssid));
  page += F("' placeholder='选择或输入 2.4G Wi-Fi' autocomplete=off required>");
  page += "<datalist id=nets>";
  page += opts;
  page += F(
      "</datalist>"
      "<label>Wi-Fi 密码</label>"
      "<input name=pass type=password value='");
  page += esc(String(cfg.pass));
  page += F(
      "' placeholder='留空表示开放网络' autocomplete=off>"
      "<label>设备语言</label><select name=lang>"
      "<option value=0");
  if (cfg.lang == 0) page += F(" selected");
  page += F(">中文</option><option value=1");
  if (cfg.lang == 1) page += F(" selected");
  page += F(
      ">English</option></select>"
      "<label>屏幕亮度</label>"
      "<input name=bright type=number min=10 max=255 value=");
  page += String(cfg.brightness);
  page += F(
      ">"
      "<button type=submit>保存并重启</button></form>"
      "<div class=card>连上 Wi-Fi 后，设备屏幕会显示一个 Mac 安装地址。"
      "在 Mac 上打开它，复制安装命令即可。</div>"
      "</div></body></html>");
  return page;
}

static String pageDone() {
  return String(F(
      "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{margin:0;background:#0E1614;color:#E8F1EE;font:15px/1.6 "
      "-apple-system,'PingFang SC',sans-serif;display:flex;min-height:100vh;"
      "align-items:center;justify-content:center;text-align:center}"
      ".ok{width:64px;height:64px;border-radius:50%;background:#0CA678;"
      "margin:0 auto 18px;display:flex;align-items:center;justify-content:center;"
      "font-size:32px;color:#04241C}</style></head><body><div>"
      "<div class=ok>&#10003;</div><h2>已保存</h2>"
      "<p style='color:#8FA3A0'>设备正在重启并连接 Wi-Fi…<br>"
      "请留意屏幕上的下一步提示。</p></div></body></html>"));
}

static String pageErr(const String& msg) {
  String p = F("<!doctype html><meta charset=utf-8><body style='background:#0E1614;"
               "color:#E8F1EE;font-family:-apple-system,sans-serif;padding:24px'>"
               "<h3 style='color:#E03131'>保存失败</h3><p>");
  p += esc(msg);
  p += F("</p><p><a style='color:#12B886' href='/'>返回修改</a></p></body>");
  return p;
}

PortalResult portalRun(Config& cfg, uint32_t timeoutMs) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apSsid[24];
  std::snprintf(apSsid, sizeof(apSsid), "oMLX-Mate-%02X%02X", mac[4], mac[5]);

  uiSplash("正在准备配网", "扫描附近的 Wi-Fi…", "");
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP_STA);
  scanNets();

  WiFi.softAP(apSsid, AP_PASS, 1, false, 4);
  IPAddress apIp = WiFi.softAPIP();
  const char* url = "http://192.168.4.1";
  uiPortalScreen(apSsid, AP_PASS, url);
  Serial.printf("[PORTAL] ap=%s pass=%s url=%s nets=%d\n", apSsid, AP_PASS, url, s_netCount);

  DNSServer dns;
  dns.start(53, "*", apIp);
  WebServer server(80);
  static bool saved = false;
  saved = false;

  server.on("/", HTTP_GET, [&]() { server.send(200, "text/html; charset=utf-8", pageForm(cfg)); });
  server.on("/save", HTTP_POST, [&]() {
    String ssid = server.arg("ssid");
    ssid.trim();
    if (!ssid.length()) {
      server.send(400, "text/html; charset=utf-8", pageErr("Wi-Fi 名称不能为空"));
      return;
    }
    Config next = cfg;
    std::strncpy(next.ssid, ssid.c_str(), sizeof(next.ssid) - 1);
    std::strncpy(next.pass, server.arg("pass").c_str(), sizeof(next.pass) - 1);
    next.lang = server.arg("lang").toInt() == 1 ? 1 : 0;
    int b = server.arg("bright").toInt();
    next.brightness = static_cast<uint8_t>(b < 10 ? 10 : (b > 255 ? 255 : b));
    if (!configSave(next)) {
      server.send(500, "text/html; charset=utf-8", pageErr("写入 NVS 失败"));
      return;
    }
    cfg = next;
    saved = true;
    server.send(200, "text/html; charset=utf-8", pageDone());
  });
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + apIp.toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();

  uint32_t t0 = millis();
  bool cancel = false;
  char cmdBuf[40];
  size_t cmdLen = 0;
  while (!saved && millis() - t0 < timeoutMs) {
    dns.processNextRequest();
    server.handleClient();
    boardUpdate();
    if (boardPortalWantsCancel()) {
      cancel = true;
      break;
    }
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        cmdBuf[cmdLen] = '\0';
        if (!strcasecmp(cmdBuf, "SKIP") || !strcasecmp(cmdBuf, "CANCEL")) cancel = true;
        cmdLen = 0;
        if (cancel) break;
      } else if (cmdLen < sizeof(cmdBuf) - 1) {
        cmdBuf[cmdLen++] = c;
      }
    }
    if (cancel) break;
    delay(4);
  }

  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  Serial.printf("[PORTAL] exit saved=%d cancel=%d\n", saved, cancel);
  if (saved) return PORTAL_SAVED;
  return cancel ? PORTAL_CANCEL : PORTAL_TIMEOUT;
}
