#include "main.h"
#include "ethernet_jl1101.h"
#include "esp_wifi.h"

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
PhaseSwitch phaseSwitch;
#ifdef BOARD_DINGTIAN
TelnetPrint debugOut;
#endif
WiFiManager wm(debugOut);
static bool s_wifi_saved_in_portal = false;

static void applyWifiConfig(Config &cfg)
{
  auto hostname = cfg.getHostname();
  if (hostname.length() > 0) {
    WiFi.setHostname(hostname.c_str());
  }
  if (!cfg.getWifiDhcp()) {
    IPAddress ip;
    IPAddress gw;
    IPAddress mask;
    IPAddress dns1;
    IPAddress dns2;
    ip.fromString(cfg.getWifiIp());
    gw.fromString(cfg.getWifiGw());
    mask.fromString(cfg.getWifiMask());
    dns1.fromString(cfg.getWifiDns1());
    dns2.fromString(cfg.getWifiDns2());
    WiFi.config(ip, gw, mask, dns1, dns2);
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
}

static void disableWifiForEthernet()
{
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t err = esp_wifi_get_mode(&mode);
  if (err == ESP_ERR_WIFI_NOT_INIT) {
    return;
  }
  if (err != ESP_OK) {
    return;
  }
  if (mode != WIFI_MODE_NULL) {
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_stop();
  }
}

static void enableWifiAfterEthernet(Config &cfg)
{
  WiFi.softAPdisconnect(true);
  WiFi.enableAP(false);
  WiFi.mode(WIFI_STA);
  applyWifiConfig(cfg);
  WiFi.begin();
}

static bool ethernetIsActive()
{
  // Some PHY/driver combinations report ETH_GOT_IP without a reliable LINK_UP event.
  return ethernetHasLink() || ethernetHasIp();
}

#ifdef BOARD_DINGTIAN
static bool s_telnet_started = false;

static void startTelnetIfWifiEnabled()
{
  if (!s_telnet_started && WiFi.getMode() != WIFI_OFF) {
    debugOut.begin(23, false);
    s_telnet_started = true;
  }
}
#endif

static void syncWifiCredsFlag(Config &cfg)
{
#ifdef ESP32
  wifi_config_t wifi_cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
    if (wifi_cfg.sta.ssid[0] != '\0') {
      cfg.setWifiCredsSet(true);
    }
  }
#endif
}

static bool hasSavedStaSsid()
{
#ifdef ESP32
  wifi_config_t wifi_cfg = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
    return wifi_cfg.sta.ssid[0] != '\0';
  }
#endif
  return false;
}

static void kickWifiDhcpIfConnectedWithoutIp()
{
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    dbgln("[wifi] connected without IP, restarting DHCP");
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.disconnect(false, false);
    delay(200);
    WiFi.reconnect();
  }
}

void setup() {
#ifndef BOARD_DINGTIAN
  debugOut.begin(115200);
#endif
  dbgln("[gpio] start");
  phaseSwitch.begin();
  dbgln("[gpio] finished");
  dbgln("[config] load")
  prefs.begin("hec_1p3p");
  config.begin(&prefs);
  phaseSwitch.setSwitchDelay(config.getSwitchDelay());

#ifdef BOARD_DINGTIAN
  setupEthernet();
  if (config.getHostname().length() > 0) {
    ethernetSetHostname(config.getHostname().c_str());
  }
  if (config.getEthDhcp()) {
    ethernetConfigureDhcp();
  } else {
    IPAddress ip;
    IPAddress gw;
    IPAddress mask;
    IPAddress dns1;
    IPAddress dns2;
    ip.fromString(config.getEthIp());
    gw.fromString(config.getEthGw());
    mask.fromString(config.getEthMask());
    dns1.fromString(config.getEthDns1());
    dns2.fromString(config.getEthDns2());
    ethernetConfigureStatic(ip, gw, mask, dns1, dns2);
  }
  const uint32_t link_wait_ms = 5000;
  const uint32_t link_start = millis();
  while (!ethernetIsActive() && (millis() - link_start) < link_wait_ms) {
    delay(100);
  }
  const bool eth_link = ethernetIsActive();
  if (eth_link) {
    (void)ethernetWaitForIp(30000);
  }
  const bool eth_ok = eth_link;
#endif
  
#ifdef BOARD_DINGTIAN
  if (!eth_ok) {
    startTelnetIfWifiEnabled();
  }
#endif
  wm.setDebugOutput(false);

  pinMode(PIN_FACTORY_LED, OUTPUT);
  digitalWrite(PIN_FACTORY_LED, LOW);

  wm.setClass("invert");
  wm.setSaveConfigCallback([&](){
    config.setWifiCredsSet(true);
    s_wifi_saved_in_portal = true;
  });
#ifdef BOARD_DINGTIAN
  if (!eth_ok) {
    dbgln("[wifi] start");
    WiFi.mode(WIFI_STA);
    applyWifiConfig(config);
    syncWifiCredsFlag(config);
    wm.autoConnect();
    kickWifiDhcpIfConnectedWithoutIp();
  } else {
    disableWifiForEthernet();
  }
#else
  dbgln("[wifi] start");
  WiFi.mode(WIFI_STA);
  applyWifiConfig(config);
  syncWifiCredsFlag(config);
  wm.autoConnect();
  kickWifiDhcpIfConnectedWithoutIp();
#endif
  MBUlogLvl = LOG_LEVEL_WARNING;
  LOGDEVICE = &debugOut;
  dbgln("[wifi] finished");
  dbgln("[modbus] start");
  if (config.getModbusEnabled()) {
    phaseSwitch.beginModbus();
    dbgln("[modbus] finished");
  } else {
    dbgln("[modbus] disabled in config");
  }
  setupPages(&webServer, &phaseSwitch, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
}

void loop() {
  uptime::calculateUptime();
#ifdef BOARD_DINGTIAN
  debugOut.loop();
  static bool wifi_disabled_by_eth = false;
  static bool wifi_portal_triggered = false;
  static bool wifi_state_initialized = false;

  if (!wifi_state_initialized) {
    // If we boot with Ethernet active and WiFi already off, remember that
    // WiFi was intentionally suppressed by Ethernet policy.
    if (ethernetIsActive() && WiFi.getMode() == WIFI_OFF) {
      wifi_disabled_by_eth = true;
    }
    wifi_state_initialized = true;
  }

  if (ethernetIsActive()) {
    wifi_portal_triggered = false;
    if (!wifi_disabled_by_eth && WiFi.getMode() != WIFI_OFF) {
      dbgln("[wifi] disabled due to ethernet");
      disableWifiForEthernet();
      wifi_disabled_by_eth = true;
    }
  } else {
    if (wifi_disabled_by_eth) {
      dbgln("[wifi] ethernet down, re-enabling wifi");
      if (hasSavedStaSsid()) {
        enableWifiAfterEthernet(config);
        WiFi.reconnect();
      } else if (!wifi_portal_triggered) {
        dbgln("[wifi] no saved credentials, starting config portal");
        webServer.end();
        WiFi.mode(WIFI_STA);
        applyWifiConfig(config);
        wm.setConfigPortalBlocking(true);
        s_wifi_saved_in_portal = false;
        (void)wm.startConfigPortal();
        wm.stopWebPortal();
        wm.stopConfigPortal();
        if (s_wifi_saved_in_portal) {
          dbgln("[wifi] portal finished, rebooting");
          delay(500);
          ESP.restart();
        }
        wifi_portal_triggered = s_wifi_saved_in_portal;
      }
      startTelnetIfWifiEnabled();
      wifi_disabled_by_eth = false;
    }
  }
#endif
  static uint32_t wifi_no_ip_since = 0;
  static uint32_t wifi_reconnect_since = 0;
  if (WiFi.getMode() != WIFI_OFF) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      if (wifi_no_ip_since == 0) {
        wifi_no_ip_since = millis();
      } else if (millis() - wifi_no_ip_since > 10000) {
        dbgln("[wifi] no IP, restarting DHCP");
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        WiFi.disconnect(false, false);
        WiFi.reconnect();
        wifi_no_ip_since = 0;
      }
    } else {
      wifi_no_ip_since = 0;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (!hasSavedStaSsid()) {
        wifi_reconnect_since = 0;
      } else {
        if (wifi_reconnect_since == 0) {
          wifi_reconnect_since = millis();
        } else if (millis() - wifi_reconnect_since > 15000) {
          dbgln("[wifi] not connected, retrying");
          applyWifiConfig(config);
          WiFi.reconnect();
          wifi_reconnect_since = 0;
        }
      }
    } else {
      wifi_reconnect_since = 0;
    }
  } else {
    wifi_no_ip_since = 0;
    wifi_reconnect_since = 0;
  }
  delay(1);
  phaseSwitch.loop();
}
