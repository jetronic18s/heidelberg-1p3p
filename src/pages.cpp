#include "pages.h"
#include "app_git_version.h"

#ifdef BOARD_DINGTIAN
#include "ethernet_jl1101.h"
#endif

#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "wifi_native.h"

#define ETAG "\"" __DATE__ "" __TIME__ "\""

static bool s_wifi_scan_requested = false;
static PhaseSwitch *s_phase_switch = nullptr;
static Config *s_config = nullptr;
static httpd_handle_t s_server = nullptr;
static esp_timer_handle_t s_restart_timer = nullptr;

struct KeyValue {
  std::string key;
  std::string value;
};

static void appendf(std::string &out, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  char small[256];
  int needed = vsnprintf(small, sizeof(small), fmt, args);
  va_end(args);

  if (needed <= 0) {
    return;
  }
  if (needed < (int)sizeof(small)) {
    out += small;
    return;
  }

  std::vector<char> large((size_t)needed + 1);
  va_start(args, fmt);
  vsnprintf(large.data(), large.size(), fmt, args);
  va_end(args);
  out += large.data();
}

static std::string urlDecode(const std::string &in)
{
  std::string out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length()) {
      char h1 = in[i + 1];
      char h2 = in[i + 2];
      int v1 = (h1 >= '0' && h1 <= '9') ? h1 - '0' : (h1 >= 'A' && h1 <= 'F') ? h1 - 'A' + 10 : (h1 >= 'a' && h1 <= 'f') ? h1 - 'a' + 10 : -1;
      int v2 = (h2 >= '0' && h2 <= '9') ? h2 - '0' : (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10 : (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10 : -1;
      if (v1 >= 0 && v2 >= 0) {
        out += (char)((v1 << 4) | v2);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

static std::vector<KeyValue> parseWwwForm(const std::string &text)
{
  std::vector<KeyValue> out;
  size_t start = 0;
  while (start <= text.length()) {
    size_t amp = text.find('&', start);
    std::string pair = (amp != std::string::npos) ? text.substr(start, amp - start) : text.substr(start);
    if (!pair.empty()) {
      size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        out.push_back({urlDecode(pair.substr(0, eq)), urlDecode(pair.substr(eq + 1))});
      } else {
        out.push_back({urlDecode(pair), std::string()});
      }
    }
    if (amp == std::string::npos) {
      break;
    }
    start = amp + 1;
  }
  return out;
}

static std::string findParam(const std::vector<KeyValue> &params, const char *key, const std::string &defaultValue = std::string())
{
  for (const auto &kv : params) {
    if (kv.key == key) {
      return kv.value;
    }
  }
  return defaultValue;
}

static bool hasParam(const std::vector<KeyValue> &params, const char *key)
{
  for (const auto &kv : params) {
    if (kv.key == key) {
      return true;
    }
  }
  return false;
}

static std::string readBody(httpd_req_t *req)
{
  std::string body;
  int remaining = req->content_len;
  body.reserve(remaining > 0 ? remaining : 0);
  char buf[256];
  while (remaining > 0) {
    int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
    int r = httpd_req_recv(req, buf, chunk);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (r <= 0) {
      break;
    }
    body.append(buf, (size_t)r);
    remaining -= r;
  }
  return body;
}

static std::vector<KeyValue> readQueryParams(httpd_req_t *req)
{
  size_t len = httpd_req_get_url_query_len(req);
  if (len == 0) {
    return {};
  }
  std::vector<char> query(len + 1, '\0');
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
    return {};
  }
  return parseWwwForm(std::string(query.data()));
}

static void sendResponse(httpd_req_t *req, const char *status, const char *type, const std::string &body)
{
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  httpd_resp_send(req, body.c_str(), body.length());
}

static void sendHtml(httpd_req_t *req, const std::string &body)
{
  sendResponse(req, "200 OK", "text/html", body);
}

static void sendText(httpd_req_t *req, const char *status, const char *body)
{
  sendResponse(req, status, "text/plain", std::string(body));
}

static void redirect(httpd_req_t *req, const char *location)
{
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_send(req, nullptr, 0);
}

static void scheduleRestart(uint64_t delayUs)
{
  if (s_restart_timer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = [](void *) { esp_restart(); };
    args.name = "restart";
    esp_timer_create(&args, &s_restart_timer);
  }
  esp_timer_stop(s_restart_timer);
  esp_timer_start_once(s_restart_timer, delayUs);
}

static bool hasSavedWifiCredentials()
{
  if (wifiNativeHasStoredStaSsid()) {
    return true;
  }
  return s_config ? s_config->getWifiCredsSet() : false;
}

static std::string wifiMacSafe()
{
  if (wifiNativeGetMode() == WIFI_MODE_NULL || !wifiNativeIsStaConnected()) {
    return std::string();
  }
  return wifiNativeStaMacString();
}

static std::string escapeHtml(const std::string &in)
{
  std::string out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

static void sendMinCss(std::string &response)
{
  response += "body{";
  response += "font-family:sans-serif;";
  response += "text-align:center;";
  response += "background:#252525;";
  response += "color:#faffff;";
  response += "}";
  response += "#content{";
  response += "display:inline-block;";
  response += "min-width:340px;";
  response += "}";
  response += "button{";
  response += "width:100%;";
  response += "line-height:2.4rem;";
  response += "background:#1fa3ec;";
  response += "border:0;";
  response += "border-radius:0.3rem;";
  response += "font-size:1.2rem;";
  response += "transition-duration:0.4s;";
  response += "color:#faffff;";
  response += "}";
  response += "button:hover{";
  response += "background:#0e70a4;";
  response += "}";
}

static void sendResponseHeader(std::string &response, const char *title, bool inlineStyle = false)
{
  response += "<!DOCTYPE html><html lang=\"en\"><head><meta charset='utf-8'>";
  response += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\"/>";
  appendf(response, "<title>Heidelberg Phase Switch - %s</title>", title);
  if (inlineStyle) {
    response += "<style>";
    sendMinCss(response);
    response += "</style>";
  } else {
    response += "<link rel=\"stylesheet\" href=\"style.css\">";
  }
  response += "</head><body><h2>Heidelberg Phase Switch</h2>";
  appendf(response, "<h3>%s</h3>", title);
  response += "<div id=\"content\">";
}

static void sendResponseTrailer(std::string &response)
{
  response += "</div></body></html>";
}

static void sendButton(std::string &response, const char *title, const char *action, const char *css = "")
{
  appendf(response,
          "<form method=\"get\" action=\"%s\"><button class=\"%s\">%s</button></form><p></p>",
          action, css, title);
}

static void sendPostButton(std::string &response, const char *title, const char *action)
{
  appendf(response,
          "<form method=\"post\" action=\"%s\"><button type=\"submit\">%s</button></form><p></p>",
          action, title);
}

static void sendTableRow(std::string &response, const char *name, const std::string &value)
{
  appendf(response, "<tr><td>%s:</td><td>%s</td></tr>", name, value.c_str());
}

static void sendTableRow(std::string &response, const char *name, const char *value)
{
  appendf(response, "<tr><td>%s:</td><td>%s</td></tr>", name, value);
}

static void sendTableRow(std::string &response, const char *name, float value)
{
  appendf(response, "<tr><td>%s:</td><td>%.1f</td></tr>", name, value);
}

static void sendTableRow(std::string &response, const char *name, uint32_t value)
{
  appendf(response, "<tr><td>%s:</td><td>%lu</td></tr>", name, (unsigned long)value);
}

static void sendTableRow(std::string &response, const char *name, uint16_t value)
{
  sendTableRow(response, name, (uint32_t)value);
}

static void sendDebugForm(std::string &response, const std::string &slaveId, const std::string &reg, const std::string &function, const std::string &count)
{
  response += "<form method=\"post\"><table>";
  appendf(response, "<tr><td><label for=\"slave\">Slave ID</label></td><td><input type=\"number\" min=\"0\" max=\"247\" id=\"slave\" name=\"slave\" value=\"%s\"></td></tr>", slaveId.c_str());
  appendf(response, "<tr><td><label for=\"func\">Function</label></td><td><select id=\"func\" name=\"func\" data-value=\"%s\">", function.c_str());
  response += "<option value=\"1\">01 Read Coils</option><option value=\"2\">02 Read Discrete Inputs</option><option value=\"3\">03 Read Holding Register</option><option value=\"4\">04 Read Input Register</option></select></td></tr>";
  appendf(response, "<tr><td><label for=\"reg\">Register</label></td><td><input type=\"number\" min=\"0\" max=\"65535\" id=\"reg\" name=\"reg\" value=\"%s\"></td></tr>", reg.c_str());
  appendf(response, "<tr><td><label for=\"count\">Count</label></td><td><input type=\"number\" min=\"0\" max=\"65535\" id=\"count\" name=\"count\" value=\"%s\"></td></tr>", count.c_str());
  response += "</table><button class=\"r\">Send</button></form><p></p>";
  response += "<script>(function(){var s=document.querySelectorAll('select[data-value]');for(var i=0;i<s.length;i++){var d=s[i];var o=d.querySelector(\"option[value='\"+d.dataset.value+\"']\");if(o){o.selected=true;}}})();</script>";
}

static esp_err_t handleRootGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /");
  if (wifiNativeGetMode() == WIFI_MODE_AP) {
    redirect(req, "/wifi-setup");
    return ESP_OK;
  }

  std::string body;
  sendResponseHeader(body, s_phase_switch->getState().c_str());
  if (s_phase_switch->canSwitchTo1P()) {
    sendPostButton(body, "Switch to 1P", "/1p");
  }
  if (s_phase_switch->canSwitchTo3P()) {
    sendPostButton(body, "Switch to 3P", "/3p");
  }
  sendButton(body, "Status", "/status");
  sendButton(body, "Config", "/config");
  sendButton(body, "Debug", "/debug");
  sendButton(body, "Firmware update", "/update");
  sendButton(body, "WiFi setup", "/wifi-setup");
  sendButton(body, "WiFi reset", "/wifi", "r");
  sendButton(body, "Reboot", "/reboot", "r");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleStatusGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /status");
  std::string body;
  sendResponseHeader(body, "Status");
  body += "<table>";

  const bool wifiConnected = (wifiNativeGetMode() != WIFI_MODE_NULL) && wifiNativeIsStaConnected();
  const bool wifiCredsSet = hasSavedWifiCredentials();
  sendTableRow(body, "WiFi Credentials", wifiCredsSet ? "set" : "not set");
  sendTableRow(body, "WiFi SSID", wifiConnected ? wifiNativeStaConnectedSsid() : std::string());
  sendTableRow(body, "WiFi RSSI", wifiConnected ? std::to_string(wifiNativeStaRssi()) : std::string());
  sendTableRow(body, "WiFi Quality", wifiConnected ? WiFiQuality(wifiNativeStaRssi()) : std::string(""));
  sendTableRow(body, "WiFi MAC", wifiMacSafe());
  sendTableRow(body, "WiFi IP", wifiConnected ? wifiNativeStaIpString() : std::string());
#ifdef BOARD_DINGTIAN
  sendTableRow(body, "ETH MAC", ethernetGetMacString());
  sendTableRow(body, "ETH IP", ethernetGetIpString());
#endif
  body += "<tr><td>&nbsp;</td><td></td></tr>";

  sendTableRow(body, "RTU Messages", s_phase_switch->getRtuMessageCount());
  sendTableRow(body, "RTU Pending Messages", s_phase_switch->getRtuPendingRequestCount());
  sendTableRow(body, "RTU Errors", s_phase_switch->getRtuErrorCount());
  sendTableRow(body, "Bridge Message", s_phase_switch->getBridgeMessageCount());
  sendTableRow(body, "Bridge Clients", s_phase_switch->getBridgeActiveClientCount());
  sendTableRow(body, "Bridge Errors", s_phase_switch->getBridgeErrorCount());
  char safetyCode[16];
  snprintf(safetyCode, sizeof(safetyCode), "0x%04X", (unsigned int)s_phase_switch->getSafetyFaultCode());
  sendTableRow(body, "Safety Fault Code", safetyCode);
  sendTableRow(body, "Safety Fault", s_phase_switch->getSafetyFaultText());
  sendTableRow(body, "Telnet Debug", (s_config->getModbusEnabled() && s_config->getTelnetEnabled()) ? "enabled (port 23)" : "disabled");
  body += "<tr><td>&nbsp;</td><td></td></tr>";

  char regLayout[16];
  snprintf(regLayout, sizeof(regLayout), "0x%X", (unsigned int)s_phase_switch->getInputRegister(4));
  sendTableRow(body, "Modbus Register-Layouts Version", regLayout);
  sendTableRow(body, "Charging State", ChargingState(s_phase_switch->getInputRegister(5)));
  sendTableRow(body, "L1 - Current (A)", s_phase_switch->getInputRegister(6) * 0.1f);
  sendTableRow(body, "L2 - Current (A)", s_phase_switch->getInputRegister(7) * 0.1f);
  sendTableRow(body, "L3 - Current (A)", s_phase_switch->getInputRegister(8) * 0.1f);
  sendTableRow(body, "PCB-Temperatur (C)", s_phase_switch->getInputRegister(9) * 0.1f);
  sendTableRow(body, "Voltage L1 (V)", s_phase_switch->getInputRegister(10));
  sendTableRow(body, "Voltage L2 (V)", s_phase_switch->getInputRegister(11));
  sendTableRow(body, "Voltage L3 (V)", s_phase_switch->getInputRegister(12));
  sendTableRow(body, "extern lock state", s_phase_switch->getInputRegister(13) == 0 ? "locked" : "unlocked");
  sendTableRow(body, "Power (VA)", s_phase_switch->getInputRegister(14));
  sendTableRow(body, "Energy since PowerOn (Wh)", (uint32_t)((uint32_t)s_phase_switch->getInputRegister(15) << 16 | s_phase_switch->getInputRegister(16)));
  sendTableRow(body, "Energy since Installation (Wh)", (uint32_t)((uint32_t)s_phase_switch->getInputRegister(17) << 16 | s_phase_switch->getInputRegister(18)));
  body += "<tr><td>&nbsp;</td><td></td></tr>";

  sendTableRow(body, "ModBus-Master WatchDog Timeout (ms)", s_phase_switch->getHoldingRegister(257));
  sendTableRow(body, "Standby Function Control", s_phase_switch->getHoldingRegister(258) == 0 ? "enabled" : "disabled");
  sendTableRow(body, "Remote lock", s_phase_switch->getHoldingRegister(259) == 0 ? "locked" : "unlocked");
  sendTableRow(body, "Maximal current command (A)", s_phase_switch->getHoldingRegister(261) * 0.1f);
  sendTableRow(body, "FailSafe Current configuration (A)", s_phase_switch->getHoldingRegister(262) * 0.1f);

  body += "<tr><td>&nbsp;</td><td></td></tr>";
  sendTableRow(body, "Build time", __DATE__ " " __TIME__);
#if APP_GIT_DIRTY
  sendTableRow(body, "Git SHA", APP_GIT_SHA "-dirty");
#else
  sendTableRow(body, "Git SHA", APP_GIT_SHA);
#endif
  sendTableRow(body, "Uptime", Uptime());
  body += "</table><p></p>";

  if (s_config->getModbusEnabled()) {
    body += "<form method=\"post\"><button class=\"r\">Update register</button></form><p></p>";
  } else {
    body += "<p class=\"e\">Modbus disabled in config; register update not available.</p>";
  }

  sendButton(body, "Back", "/");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleStatusPost(httpd_req_t *req)
{
  s_phase_switch->updateCachedRegisters();
  redirect(req, "/status");
  return ESP_OK;
}

static esp_err_t handleRebootGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /reboot");
  std::string body;
  sendResponseHeader(body, "Really?");
  sendButton(body, "Back", "/");
  body += "<form method=\"post\"><button class=\"r\">Yes, do it!</button></form>";
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleRebootPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /reboot");
  std::string body;
  sendResponseHeader(body, "Reboot");
  body += "<p>Rebooting...</p>";
  sendResponseTrailer(body);
  sendHtml(req, body);
  scheduleRestart(250000);
  return ESP_OK;
}

static esp_err_t handleConfigGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /config");
  auto query = readQueryParams(req);
  bool hostnameInvalid = findParam(query, "err") == "hostname";

  std::string body;
  sendResponseHeader(body, "Config");
  body += "<form method=\"post\"><table>";
  appendf(body, "<tr><td><label for=\"sd\">Phase switch delay (ms)</label></td><td><input type=\"number\" min=\"1\" id=\"sd\" name=\"sd\" value=\"%lu\"></td></tr>", (unsigned long)s_config->getSwitchDelay());

  appendf(body, "<tr><td><label for=\"hostname\">Hostname</label></td><td><input type=\"text\" id=\"hostname\" name=\"hostname\" value=\"%s\">", s_config->getHostname().c_str());
  if (hostnameInvalid) {
    body += "<span class=\"e\" style=\"margin-left:0.6em;\">Ungueltiger Hostname (1-32 Zeichen, A-Z, 0-9, '-'; kein '-' am Anfang/Ende)</span>";
  }
  body += "</td></tr>";

  appendf(body, "<tr><td><label>WiFi mode</label></td><td><label><input type=\"radio\" name=\"wifimode\" value=\"dhcp\" %s> DHCP</label><label><input type=\"radio\" name=\"wifimode\" value=\"static\" %s> Static</label></td></tr>",
          s_config->getWifiDhcp() ? "checked" : "", s_config->getWifiDhcp() ? "" : "checked");
  appendf(body, "<tr class=\"wifi-static\"><td><label for=\"wifiip\">WiFi IP</label></td><td><input type=\"text\" id=\"wifiip\" name=\"wifiip\" value=\"%s\"></td></tr>", s_config->getWifiIp().c_str());
  appendf(body, "<tr class=\"wifi-static\"><td><label for=\"wifigw\">WiFi Gateway</label></td><td><input type=\"text\" id=\"wifigw\" name=\"wifigw\" value=\"%s\"></td></tr>", s_config->getWifiGw().c_str());
  appendf(body, "<tr class=\"wifi-static\"><td><label for=\"wifimask\">WiFi Netmask</label></td><td><input type=\"text\" id=\"wifimask\" name=\"wifimask\" value=\"%s\"></td></tr>", s_config->getWifiMask().c_str());
  appendf(body, "<tr class=\"wifi-static\"><td><label for=\"wifidns1\">WiFi DNS 1</label></td><td><input type=\"text\" id=\"wifidns1\" name=\"wifidns1\" value=\"%s\"></td></tr>", s_config->getWifiDns1().c_str());
  appendf(body, "<tr class=\"wifi-static\"><td><label for=\"wifidns2\">WiFi DNS 2</label></td><td><input type=\"text\" id=\"wifidns2\" name=\"wifidns2\" value=\"%s\"></td></tr>", s_config->getWifiDns2().c_str());

#ifdef BOARD_DINGTIAN
  appendf(body, "<tr><td><label>Ethernet mode</label></td><td><label><input type=\"radio\" name=\"ethmode\" value=\"dhcp\" %s> DHCP</label><label><input type=\"radio\" name=\"ethmode\" value=\"static\" %s> Static</label></td></tr>",
          s_config->getEthDhcp() ? "checked" : "", s_config->getEthDhcp() ? "" : "checked");
  appendf(body, "<tr class=\"eth-static\"><td><label for=\"ethip\">Ethernet IP</label></td><td><input type=\"text\" id=\"ethip\" name=\"ethip\" value=\"%s\"></td></tr>", s_config->getEthIp().c_str());
  appendf(body, "<tr class=\"eth-static\"><td><label for=\"ethgw\">Ethernet Gateway</label></td><td><input type=\"text\" id=\"ethgw\" name=\"ethgw\" value=\"%s\"></td></tr>", s_config->getEthGw().c_str());
  appendf(body, "<tr class=\"eth-static\"><td><label for=\"ethmask\">Ethernet Netmask</label></td><td><input type=\"text\" id=\"ethmask\" name=\"ethmask\" value=\"%s\"></td></tr>", s_config->getEthMask().c_str());
  appendf(body, "<tr class=\"eth-static\"><td><label for=\"ethdns1\">Ethernet DNS 1</label></td><td><input type=\"text\" id=\"ethdns1\" name=\"ethdns1\" value=\"%s\"></td></tr>", s_config->getEthDns1().c_str());
  appendf(body, "<tr class=\"eth-static\"><td><label for=\"ethdns2\">Ethernet DNS 2</label></td><td><input type=\"text\" id=\"ethdns2\" name=\"ethdns2\" value=\"%s\"></td></tr>", s_config->getEthDns2().c_str());
#endif

  body += "</table><p></p>";
  body += "<label><input type=\"checkbox\" name=\"modbus\" value=\"1\" ";
  body += s_config->getModbusEnabled() ? "checked" : "";
  body += "> Modbus/RS485 aktiv</label>";
  body += "<br><label><input type=\"checkbox\" id=\"telnet\" name=\"telnet\" value=\"1\" ";
  if (s_config->getModbusEnabled() && s_config->getTelnetEnabled()) {
    body += "checked ";
  }
  body += "> Telnet-Debug aktiv (Port 23)</label>";
  body += "<p style=\"font-size:0.9em;opacity:0.8;\">Hinweis: Statische IP-Einstellungen werden nach einem Reboot aktiv.</p>";
  body += "<script>function toggleRows(g,s){var r=document.getElementsByClassName(g+'-static');for(var i=0;i<r.length;i++){r[i].style.display=s?'table-row':'none';}}";
  body += "function updateDebugOptions(){var m=document.querySelector('input[name=\\\"modbus\\\"]');var t=document.getElementById('telnet');var on=!!(m&&m.checked);if(t){t.disabled=!on;if(!on){t.checked=false;}}}";
  body += "function updateNetMode(){var w=document.querySelector('input[name=\\\"wifimode\\\"]:checked');var e=document.querySelector('input[name=\\\"ethmode\\\"]:checked');toggleRows('wifi',w&&w.value==='static');if(e){toggleRows('eth',e.value==='static');}}";
  body += "var radios=document.querySelectorAll('input[name=\\\"wifimode\\\"],input[name=\\\"ethmode\\\"]');for(var i=0;i<radios.length;i++){radios[i].addEventListener('change',updateNetMode);}var m=document.querySelector('input[name=\\\"modbus\\\"]');if(m){m.addEventListener('change',updateDebugOptions);}updateNetMode();updateDebugOptions();</script>";
  body += "<button class=\"r\">Save</button></form><p></p>";
  sendButton(body, "Back", "/");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleConfigPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /config");
  auto params = parseWwwForm(readBody(req));

  if (hasParam(params, "sd")) {
    uint32_t delay = (uint32_t)strtoul(findParam(params, "sd").c_str(), nullptr, 10);
    s_config->setSwitchDelay(delay);
    s_phase_switch->setSwitchDelay(delay);
    dbgln("[webserver] saved switch delay");
  }

  if (hasParam(params, "hostname")) {
    std::string hostname = findParam(params, "hostname");
    if (!Config::isHostnameValid(hostname)) {
      redirect(req, "/config?err=hostname");
      return ESP_OK;
    }
    s_config->setHostname(hostname);
    if (s_config->getHostname().length() > 0) {
      wifiNativeSetHostname(s_config->getHostname().c_str());
#ifdef BOARD_DINGTIAN
      ethernetSetHostname(s_config->getHostname().c_str());
#endif
    }
  }

  if (hasParam(params, "wifimode")) {
    s_config->setWifiDhcp(findParam(params, "wifimode") == "dhcp");
  }
  if (hasParam(params, "wifiip")) s_config->setWifiIp(std::string(findParam(params, "wifiip").c_str()));
  if (hasParam(params, "wifigw")) s_config->setWifiGw(std::string(findParam(params, "wifigw").c_str()));
  if (hasParam(params, "wifimask")) s_config->setWifiMask(std::string(findParam(params, "wifimask").c_str()));
  if (hasParam(params, "wifidns1")) s_config->setWifiDns1(std::string(findParam(params, "wifidns1").c_str()));
  if (hasParam(params, "wifidns2")) s_config->setWifiDns2(std::string(findParam(params, "wifidns2").c_str()));

#ifdef BOARD_DINGTIAN
  if (hasParam(params, "ethmode")) {
    s_config->setEthDhcp(findParam(params, "ethmode") == "dhcp");
  }
  if (hasParam(params, "ethip")) s_config->setEthIp(std::string(findParam(params, "ethip").c_str()));
  if (hasParam(params, "ethgw")) s_config->setEthGw(std::string(findParam(params, "ethgw").c_str()));
  if (hasParam(params, "ethmask")) s_config->setEthMask(std::string(findParam(params, "ethmask").c_str()));
  if (hasParam(params, "ethdns1")) s_config->setEthDns1(std::string(findParam(params, "ethdns1").c_str()));
  if (hasParam(params, "ethdns2")) s_config->setEthDns2(std::string(findParam(params, "ethdns2").c_str()));

  if (s_config->getEthDhcp()) {
    ethernetConfigureDhcp();
  } else {
    ethernetConfigureStatic(s_config->getEthIp().c_str(),
                            s_config->getEthGw().c_str(),
                            s_config->getEthMask().c_str(),
                            s_config->getEthDns1().c_str(),
                            s_config->getEthDns2().c_str());
  }
#endif

  const bool modbusEnabled = hasParam(params, "modbus");
  s_config->setModbusEnabled(modbusEnabled);
  s_config->setTelnetEnabled(modbusEnabled && hasParam(params, "telnet"));
  redirect(req, "/");
  return ESP_OK;
}

static esp_err_t handle1pPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /1p");
  s_phase_switch->switchTo1P();
  redirect(req, "/");
  return ESP_OK;
}

static esp_err_t handle3pPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /3p");
  s_phase_switch->switchTo3P();
  redirect(req, "/");
  return ESP_OK;
}

static esp_err_t handleDebugGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /debug");
  std::string body;
  sendResponseHeader(body, "Debug");
  sendDebugForm(body, "1", "1", "3", "1");
  sendButton(body, "Back", "/");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleDebugPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /debug");
  auto params = parseWwwForm(readBody(req));
  std::string slaveId = findParam(params, "slave", "1");
  std::string reg = findParam(params, "reg", "1");
  std::string func = findParam(params, "func", "3");
  std::string count = findParam(params, "count", "1");

  std::string body;
  sendResponseHeader(body, "Debug");
  body += "<pre>Direct RTU requests via Web UI are currently disabled in the native version.</pre>";

  sendDebugForm(body, slaveId, reg, func, count);
  sendButton(body, "Back", "/");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleUpdateGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /update");
  std::string body;
  sendResponseHeader(body, "Firmware Update");
  body += "<form id=\"ota\"><input type=\"file\" id=\"file\" required/><p></p><button class=\"r\" type=\"submit\">Upload</button></form><p id=\"msg\"></p>";
  body += "<script>document.getElementById('ota').addEventListener('submit',async function(e){e.preventDefault();var f=document.getElementById('file').files[0];if(!f)return;document.getElementById('msg').textContent='Uploading...';var r=await fetch('/update?name='+encodeURIComponent(f.name),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});document.getElementById('msg').textContent=await r.text();});</script>";
  sendButton(body, "Back", "/");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleUpdatePost(httpd_req_t *req)
{
  dbgln("[webserver] POST /update");
  auto query = readQueryParams(req);
  std::string filename = findParam(query, "name", "firmware.bin");
  if (filename == "filesystem") {
    sendText(req, "400 Bad Request", "Filesystem OTA not supported");
    return ESP_OK;
  }

  const esp_partition_t *updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    sendText(req, "400 Bad Request", "OTA could not begin");
    return ESP_OK;
  }
  esp_ota_handle_t otaHandle = 0;
  if (esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle) != ESP_OK) {
    sendText(req, "400 Bad Request", "OTA could not begin");
    return ESP_OK;
  }

  int remaining = req->content_len;
  uint8_t buf[1024];
  while (remaining > 0) {
    int readLen = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
    int r = httpd_req_recv(req, (char *)buf, readLen);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (r <= 0) {
      esp_ota_abort(otaHandle);
      sendText(req, "400 Bad Request", "OTA read failed");
      return ESP_OK;
    }
    if (esp_ota_write(otaHandle, buf, (size_t)r) != ESP_OK) {
      esp_ota_abort(otaHandle);
      sendText(req, "400 Bad Request", "OTA could not write data");
      return ESP_OK;
    }
    remaining -= r;
  }

  if (esp_ota_end(otaHandle) != ESP_OK) {
    sendText(req, "400 Bad Request", "Could not end OTA");
    return ESP_OK;
  }
  if (esp_ota_set_boot_partition(updatePartition) != ESP_OK) {
    sendText(req, "400 Bad Request", "Could not end OTA");
    return ESP_OK;
  }

  sendText(req, "200 OK", "Update successful. Rebooting...");
  scheduleRestart(500000);
  return ESP_OK;
}

static esp_err_t handleWifiGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /wifi");
  std::string body;
  sendResponseHeader(body, "WiFi reset");
  body += "<p class=\"e\">This will delete the stored WiFi config<br/>and restart the ESP in AP mode.<br/> Are you sure?</p>";
  sendButton(body, "Back", "/");
  body += "<p></p><form method=\"post\"><button class=\"r\">Yes, do it!</button></form>";
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleWifiPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /wifi");
  s_config->setWifiCredsSet(false);
  s_config->setWifiResetPending(true);
  dbgln("[webserver] scheduled wifi config erase on next boot");

  std::string body;
  sendResponseHeader(body, "WiFi reset");
  body += "<p>WiFi config will be erased after reboot. Rebooting...</p>";
  sendResponseTrailer(body);
  sendHtml(req, body);
  scheduleRestart(300000);
  return ESP_OK;
}

static esp_err_t handleWifiSetupGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /wifi-setup");
  auto query = readQueryParams(req);
  bool doScan = s_wifi_scan_requested || hasParam(query, "scan");
  s_wifi_scan_requested = false;

  std::vector<WifiScanEntry> scanResults;
  int found = -1;
  if (doScan) {
    found = wifiNativeScan(&scanResults);
  }

  std::string body;
  sendResponseHeader(body, "WiFi setup");

#ifdef BOARD_DINGTIAN
  bool ethActive = ethernetHasLink() || ethernetHasIp();
#else
  bool ethActive = false;
#endif

  if (ethActive) {
    body += "<p style=\"color: #d9534f; font-weight: bold; margin: 10px 0;\">WLAN ist deaktiviert, da eine aktive LAN-Verbindung besteht.<br>(WiFi is disabled because an active LAN link is present.)</p>";
  }

  body += "<form method=\"post\"><table>";
  body += "<tr><td><label for=\"ssid_pick\">Gefundene WLANs</label></td><td><select id=\"ssid_pick\" onchange=\"if(this.value){document.getElementById('ssid').value=this.value;}\"";
  if (ethActive) body += " disabled";
  body += ">";
  body += "<option value=\"\">-- WLAN auswaehlen --</option>";

  if (!doScan) {
    body += "<option value=\"\">Noch nicht gescannt</option>";
  } else if (found > 0) {
    for (int i = 0; i < found; i++) {
      std::string ssid = scanResults[i].ssid;
      if (ssid.empty()) {
        continue;
      }
      std::string safe = escapeHtml(ssid);
      appendf(body, "<option value=\"%s\">%s (%d dBm)</option>", safe.c_str(), safe.c_str(), (int)scanResults[i].rssi);
    }
  } else if (found == 0) {
    body += "<option value=\"\">Keine WLANs gefunden</option>";
  } else {
    body += "<option value=\"\">Scan fehlgeschlagen</option>";
  }

  body += "</select></td></tr>";
  body += "<tr><td><label for=\"ssid\">WiFi SSID</label></td><td><input type=\"text\" id=\"ssid\" name=\"ssid\" required";
  if (ethActive) body += " disabled";
  body += "></td></tr>";
  body += "<tr><td><label for=\"pass\">WiFi Password</label></td><td><input type=\"password\" id=\"pass\" name=\"pass\"";
  if (ethActive) body += " disabled";
  body += "></td></tr>";
  body += "</table><p></p><button class=\"r\"";
  if (ethActive) body += " disabled style=\"opacity: 0.5; cursor: not-allowed;\"";
  body += ">Save</button></form><p></p>";

  if (ethActive) {
    appendf(body, "<form method=\"get\" action=\"/wifi-scan\"><button class=\"\" disabled style=\"opacity: 0.5; cursor: not-allowed;\">%s</button></form><p></p>", "WLAN scannen");
  } else {
    sendButton(body, "WLAN scannen", "/wifi-scan");
  }
  sendButton(body, "Back", "/");
  sendResponseTrailer(body);
  sendHtml(req, body);
  return ESP_OK;
}

static esp_err_t handleWifiScanGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /wifi-scan");
  s_wifi_scan_requested = true;
  redirect(req, "/wifi-setup");
  return ESP_OK;
}

static esp_err_t handleWifiSetupPost(httpd_req_t *req)
{
  dbgln("[webserver] POST /wifi-setup");
  auto params = parseWwwForm(readBody(req));
  std::string ssid = findParam(params, "ssid");
  std::string pass = findParam(params, "pass");
  auto first = ssid.find_first_not_of(" \t\r\n");
  auto last = ssid.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    redirect(req, "/wifi-setup");
    return ESP_OK;
  }
  ssid = ssid.substr(first, last - first + 1);

  s_config->setWifiCredsSet(true);
  wifiNativeEnsureStaMode();
  if (s_config->getHostname().length() > 0) {
    wifiNativeSetHostname(s_config->getHostname().c_str());
  }

  if (!wifiNativeSetStaCredentials(ssid, pass)) {
    sendText(req, "500 Internal Server Error", "Failed to save WiFi credentials");
    return ESP_OK;
  }

  if (s_config->getWifiDhcp()) {
    if (!wifiNativeSetStaDhcp()) {
      sendText(req, "500 Internal Server Error", "Failed to apply DHCP config");
      return ESP_OK;
    }
  } else {
    if (!wifiNativeSetStaStatic(std::string(s_config->getWifiIp().c_str()),
                                std::string(s_config->getWifiGw().c_str()),
                                std::string(s_config->getWifiMask().c_str()),
                                std::string(s_config->getWifiDns1().c_str()),
                                std::string(s_config->getWifiDns2().c_str()))) {
      sendText(req, "400 Bad Request", "Invalid static IP config");
      return ESP_OK;
    }
  }

  std::string body;
  sendResponseHeader(body, "WiFi setup");
  body += "<p>Saved. Rebooting...</p>";
  sendResponseTrailer(body);
  sendHtml(req, body);
  scheduleRestart(300000);
  return ESP_OK;
}

static esp_err_t handleFaviconGet(httpd_req_t *req)
{
  dbgln("[webserver] GET /favicon.ico");
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t handleStyleGet(httpd_req_t *req)
{
  char etag[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", etag, sizeof(etag)) == ESP_OK) {
    if (std::string(etag) == std::string(ETAG)) {
      httpd_resp_set_status(req, "304 Not Modified");
      httpd_resp_send(req, nullptr, 0);
      return ESP_OK;
    }
  }

  dbgln("[webserver] GET /style.css");
  std::string body;
  sendMinCss(body);
  body += "button.r{background:#d43535;}";
  body += "button.r:hover{background:#931f1f;}";
  body += "table{text-align:left;width:100%;}";
  body += "input{width:100%;}";
  body += ".e{color:red;}";
  body += "pre{text-align:left;}";

  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "text/css");
  httpd_resp_set_hdr(req, "ETag", ETAG);
  httpd_resp_send(req, body.c_str(), body.length());
  return ESP_OK;
}

static esp_err_t handle404(httpd_req_t *req, httpd_err_code_t)
{
  dbg("[webserver] request to ");
  dbg(req->uri);
  dbgln(" not found");
  sendText(req, "404 Not Found", "404");
  return ESP_OK;
}

void setupPages(PhaseSwitch *phaseSwitch, Config *config)
{
  s_phase_switch = phaseSwitch;
  s_config = config;

  if (s_server != nullptr) {
    httpd_stop(s_server);
    s_server = nullptr;
  }

  httpd_config_t serverConfig = HTTPD_DEFAULT_CONFIG();
  serverConfig.server_port = 80;
  serverConfig.max_uri_handlers = 24;
  serverConfig.stack_size = 12288;

  if (httpd_start(&s_server, &serverConfig) != ESP_OK) {
    dbgln("[webserver] httpd_start failed");
    return;
  }

  auto registerUri = [](const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) {
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    route.user_ctx = nullptr;
    return httpd_register_uri_handler(s_server, &route);
  };

  registerUri("/", HTTP_GET, handleRootGet);
  registerUri("/status", HTTP_GET, handleStatusGet);
  registerUri("/status", HTTP_POST, handleStatusPost);
  registerUri("/reboot", HTTP_GET, handleRebootGet);
  registerUri("/reboot", HTTP_POST, handleRebootPost);
  registerUri("/config", HTTP_GET, handleConfigGet);
  registerUri("/config", HTTP_POST, handleConfigPost);
  registerUri("/1p", HTTP_POST, handle1pPost);
  registerUri("/3p", HTTP_POST, handle3pPost);
  registerUri("/debug", HTTP_GET, handleDebugGet);
  registerUri("/debug", HTTP_POST, handleDebugPost);
  registerUri("/update", HTTP_GET, handleUpdateGet);
  registerUri("/update", HTTP_POST, handleUpdatePost);
  registerUri("/wifi", HTTP_GET, handleWifiGet);
  registerUri("/wifi", HTTP_POST, handleWifiPost);
  registerUri("/wifi-setup", HTTP_GET, handleWifiSetupGet);
  registerUri("/wifi-setup", HTTP_POST, handleWifiSetupPost);
  registerUri("/wifi-scan", HTTP_GET, handleWifiScanGet);
  registerUri("/favicon.ico", HTTP_GET, handleFaviconGet);
  registerUri("/style.css", HTTP_GET, handleStyleGet);

  httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle404);
}

std::string ErrorName(uint8_t code)
{
  switch (code)
  {
      case 0x00: return "Success";
      case 0x01: return "Illegal function";
      case 0x02: return "Illegal data address";
      case 0x03: return "Illegal data value";
      case 0x04: return "Server device failure";
      case 0x05: return "Acknowledge";
      case 0x06: return "Server device busy";
      case 0x07: return "Negative acknowledge";
      case 0x08: return "Memory parity error";
      default: return "undefined error";
  }
}

std::string WiFiQuality(int rssiValue)
{
  switch (rssiValue)
  {
      case -30 ... 0: return "Amazing";
      case -67 ... -31: return "Very Good";
      case -70 ... -68: return "Okay";
      case -80 ... -71: return "Not Good";
      default: return "Unusable";
  }
}

std::string ChargingState(uint16_t state){
  switch(state){
    case 2: return "A1";
    case 3: return "A2";
    case 4: return "B1";
    case 5: return "B2";
    case 6: return "C1";
    case 7: return "C2";
    case 8: return "derating";
    case 9: return "E";
    case 10: return "F";
    case 11: return "ERR";
      default: return std::to_string(state);
  }
}

std::string Uptime(){
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu", uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  auto result = std::string(buffer);
  if (uptime::getDays() > 0){
    result = std::to_string(uptime::getDays()) + "." + result;
  }
  return result;
}
