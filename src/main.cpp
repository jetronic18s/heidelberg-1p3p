#include "main.h"
#include "ethernet_jl1101.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

Config config;
PhaseSwitch phaseSwitch;
static volatile bool s_sta_connected_or_got_ip = false;
static bool s_sta_ever_connected = false;
static bool s_modbus_started = false;

static uint32_t nowMs()
{
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void sleepMs(uint32_t ms)
{
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static void applyWifiConfig(Config &cfg)
{
  auto hostname = cfg.getHostname();
  if (hostname.length() > 0) {
    wifiNativeSetHostname(hostname.c_str());
  }
  if (!cfg.getWifiDhcp()) {
    wifiNativeSetStaStatic(std::string(cfg.getWifiIp().c_str()),
                           std::string(cfg.getWifiGw().c_str()),
                           std::string(cfg.getWifiMask().c_str()),
                           std::string(cfg.getWifiDns1().c_str()),
                           std::string(cfg.getWifiDns2().c_str()));
  } else {
    wifiNativeSetStaDhcp();
  }
}

static void disableWifiForEthernet()
{
  if (wifiNativeGetMode() != WIFI_MODE_NULL) {
    wifiNativeDisable();
  }
}

static void enableWifiAfterEthernet(Config &cfg)
{
  wifiNativeEnsureStaMode();
  applyWifiConfig(cfg);
  wifiNativeConnectSta();
}

static void forceStaOnlyMode(Config &cfg)
{
  wifi_mode_t mode = wifiNativeGetMode();
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA || mode == WIFI_MODE_NULL) {
    wifiNativeEnsureStaMode();
    applyWifiConfig(cfg);
  }
}

static bool ethernetIsActive()
{
  // Some PHY/driver combinations report ETH_GOT_IP without a reliable LINK_UP event.
  return ethernetHasLink() || ethernetHasIp();
}

static void syncWifiCredsFlag(Config &cfg)
{
  if (wifiNativeHasStoredStaSsid()) {
    cfg.setWifiCredsSet(true);
  }
}

static bool hasSavedStaSsid();

static void prepareStaInterfaceAndSyncCreds(Config &cfg)
{
  wifiNativeEnsureStaMode();
  applyWifiConfig(cfg);
  syncWifiCredsFlag(cfg);
  if (hasSavedStaSsid()) {
    cfg.setWifiCredsSet(true);
  }
}

static bool hasSavedStaSsid()
{
  return wifiNativeHasStoredStaSsid();
}

static bool hasConfiguredWifi(Config &cfg)
{
  // When WiFi driver is stopped (WIFI_MODE_NULL), esp_wifi_get_config may fail.
  // Keep a persistent fallback flag so we don't incorrectly open AP portal.
  return hasSavedStaSsid() || cfg.getWifiCredsSet() || (wifiNativeStaConnectedSsid().length() > 0);
}

static bool shouldStartSetupAp(Config &cfg)
{
  return !hasConfiguredWifi(cfg) && !s_sta_ever_connected;
}

static bool isStaModeActive()
{
  wifi_mode_t mode = wifiNativeGetMode();
  return mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
}

static bool shouldStartModbusNow(Config &cfg)
{
  if (!cfg.getModbusEnabled()) {
    return false;
  }
#ifdef BOARD_DINGTIAN
  if (ethernetIsActive()) {
    return true;
  }
#endif
  return wifiNativeIsStaConnected();
}

static bool shouldEnableTelnetNow(Config &cfg)
{
  return cfg.getModbusEnabled() && cfg.getTelnetEnabled();
}

static void startWifiSetupAccessPoint(Config &cfg)
{
  std::string apName = cfg.getHostname();
  if (apName.empty()) {
    apName = "heidelberg-1p3p";
  }
  apName += "-setup";
  wifiNativeStartAp(apName);
}

static void kickWifiDhcpIfConnectedWithoutIp()
{
  if (wifiNativeIsStaConnected() && !wifiNativeIsStaGotIp()) {
    dbgln("[wifi] connected without IP, restarting DHCP");
    wifiNativeReconnectSta();
  }
}

static void wifiEventHandler(void *, esp_event_base_t eventBase, int32_t eventId, void *)
{
  if ((eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_CONNECTED) ||
      (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP)) {
    s_sta_connected_or_got_ip = true;
    s_sta_ever_connected = true;
  }
}

void setup() {
  debugInit();
  wifiNativeInit();
  dbgln("[gpio] start");
  phaseSwitch.begin();
  dbgln("[gpio] finished");
  dbgln("[config] load")
  config.begin();
  debugSetTelnetEnabled(shouldEnableTelnetNow(config));
  if (config.getWifiResetPending()) {
    dbgln("[wifi] pending reset: clearing stored wifi credentials");
    config.setWifiResetPending(false);
    config.setWifiCredsSet(false);
    wifiNativeEnsureStaMode();
    sleepMs(50);
    esp_err_t restoreErr = esp_wifi_restore();
    if (restoreErr != ESP_OK) {
      dbg("[wifi] esp_wifi_restore failed: ");
      dbgln((int)restoreErr);
    }
    wifiNativeDisconnectSta(false);
    wifiNativeDisable();
    sleepMs(50);
  }
  phaseSwitch.setSwitchDelay(config.getSwitchDelay());

#ifdef BOARD_DINGTIAN
  setupEthernet();
  if (config.getHostname().length() > 0) {
    ethernetSetHostname(config.getHostname().c_str());
  }
  if (config.getEthDhcp()) {
    ethernetConfigureDhcp();
  } else {
    ethernetConfigureStatic(config.getEthIp().c_str(),
                            config.getEthGw().c_str(),
                            config.getEthMask().c_str(),
                            config.getEthDns1().c_str(),
                            config.getEthDns2().c_str());
  }
  const uint32_t link_wait_ms = 5000;
  const uint32_t link_start = nowMs();
  while (!ethernetIsActive() && (nowMs() - link_start) < link_wait_ms) {
    sleepMs(100);
  }
  const bool eth_link = ethernetIsActive();
  if (eth_link) {
    (void)ethernetWaitForIp(30000);
  }
  const bool eth_ok = eth_link;
#endif

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr);

  gpio_reset_pin((gpio_num_t)PIN_FACTORY_LED);
  gpio_set_direction((gpio_num_t)PIN_FACTORY_LED, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_FACTORY_LED, 0);
#ifdef BOARD_DINGTIAN
  if (!eth_ok) {
    dbgln("[wifi] start");
    prepareStaInterfaceAndSyncCreds(config);
    if (hasConfiguredWifi(config)) {
      wifiNativeConnectSta();
      forceStaOnlyMode(config);
      kickWifiDhcpIfConnectedWithoutIp();
    } else if (shouldStartSetupAp(config)) {
      startWifiSetupAccessPoint(config);
    } else {
      wifiNativeConnectSta();
    }
  } else {
    disableWifiForEthernet();
  }
#else
  dbgln("[wifi] start");
  prepareStaInterfaceAndSyncCreds(config);
  if (hasConfiguredWifi(config)) {
    wifiNativeConnectSta();
    forceStaOnlyMode(config);
    kickWifiDhcpIfConnectedWithoutIp();
  } else if (shouldStartSetupAp(config)) {
    startWifiSetupAccessPoint(config);
  } else {
    wifiNativeConnectSta();
  }
#endif
  dbgln("[wifi] finished");
  dbgln("[modbus] start");
  if (shouldStartModbusNow(config)) {
    phaseSwitch.beginModbus();
    s_modbus_started = true;
    dbgln("[modbus] finished");
  } else {
    if (!config.getModbusEnabled()) {
      dbgln("[modbus] disabled in config");
    } else {
      dbgln("[modbus] deferred (network not ready)");
    }
  }
  setupPages(&phaseSwitch, &config);
  dbgln("[setup] finished");
}

void loop() {
  debugSetTelnetEnabled(shouldEnableTelnetNow(config));
  uptime::calculateUptime();
  if (s_sta_connected_or_got_ip) {
    config.setWifiCredsSet(true);
    s_sta_ever_connected = true;
    s_sta_connected_or_got_ip = false;
  }
#ifdef BOARD_DINGTIAN
  debugLoop();
  static bool wifi_disabled_by_eth = false;
  static bool wifi_state_initialized = false;
  static uint32_t eth_inactive_since = 0;
  static uint32_t wifi_reenable_after = 0;

  if (!wifi_state_initialized) {
    // If we boot with Ethernet active and WiFi already off, remember that
    // WiFi was intentionally suppressed by Ethernet policy.
    if (ethernetIsActive() && wifiNativeGetMode() == WIFI_MODE_NULL) {
      wifi_disabled_by_eth = true;
    }
    wifi_state_initialized = true;
  }

  const bool eth_active = ethernetIsActive();

  if (eth_active) {
    eth_inactive_since = 0;
    if (wifiNativeGetMode() != WIFI_MODE_NULL) {
      dbgln("[wifi] disabled due to ethernet");
      disableWifiForEthernet();
      wifi_disabled_by_eth = true;
      wifi_reenable_after = nowMs() + 5000;
    }
  } else {
    if (eth_inactive_since == 0) {
      eth_inactive_since = nowMs();
    }
    // Avoid false LAN-down transitions on flaky link/IP events.
    if ((nowMs() - eth_inactive_since) < 5000) {
      sleepMs(1);
      phaseSwitch.loop();
      return;
    }
    if (wifi_reenable_after != 0 && nowMs() < wifi_reenable_after) {
      sleepMs(1);
      phaseSwitch.loop();
      return;
    }
    if (wifi_disabled_by_eth) {
      dbgln("[wifi] ethernet down, re-enabling wifi");
      prepareStaInterfaceAndSyncCreds(config);
      // Runtime LAN->WiFi fallback stays STA-only to avoid AP/STA race conditions
      // in esp_netif on fast link flaps. Initial setup AP is still available at boot.
      enableWifiAfterEthernet(config);
      wifi_disabled_by_eth = false;
      wifi_reenable_after = 0;
    }
    if (hasConfiguredWifi(config)) {
      forceStaOnlyMode(config);
    }
  }
#endif
  if (!s_modbus_started && shouldStartModbusNow(config)) {
    dbgln("[modbus] starting after network ready");
    phaseSwitch.beginModbus();
    s_modbus_started = true;
  }
  static uint32_t wifi_no_ip_since = 0;
  static uint32_t wifi_reconnect_since = 0;
  if (isStaModeActive()) {
    if (wifiNativeIsStaConnected() && !wifiNativeIsStaGotIp()) {
      if (wifi_no_ip_since == 0) {
        wifi_no_ip_since = nowMs();
      } else if (nowMs() - wifi_no_ip_since > 3000) {
        dbgln("[wifi] no IP, restarting DHCP");
        wifiNativeReconnectSta();
        wifi_no_ip_since = 0;
      }
    } else {
      wifi_no_ip_since = 0;
    }

    if (!wifiNativeIsStaConnected()) {
      if (!hasSavedStaSsid()) {
        wifi_reconnect_since = 0;
      } else {
        if (wifi_reconnect_since == 0) {
          wifi_reconnect_since = nowMs();
        } else if (nowMs() - wifi_reconnect_since > 15000) {
          dbgln("[wifi] not connected, retrying");
          applyWifiConfig(config);
          wifiNativeReconnectSta();
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
  sleepMs(1);
  phaseSwitch.loop();
}
