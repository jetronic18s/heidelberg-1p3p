#include "main.h"
#include "ethernet_jl1101.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Main";

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
  return ethernetHasLink() || ethernetHasIp();
}

static void syncWifiCredsFlag(Config &cfg)
{
  if (wifiNativeHasStoredStaSsid()) {
    cfg.setWifiCredsSet(true);
  }
}

static void prepareStaInterfaceAndSyncCreds(Config &cfg)
{
  wifiNativeEnsureStaMode();
  applyWifiConfig(cfg);
  syncWifiCredsFlag(cfg);
}

static bool hasSavedStaSsid()
{
  return wifiNativeHasStoredStaSsid();
}

static bool hasConfiguredWifi(Config &cfg)
{
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

static void wifiEventHandler(void *, esp_event_base_t eventBase, int32_t eventId, void *)
{
  if ((eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_CONNECTED) ||
      (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP)) {
    s_sta_connected_or_got_ip = true;
    s_sta_ever_connected = true;
  }
}

static void main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting system initialization...");
    
    wifiNativeInit();
    ESP_LOGI(TAG, "gpio start");
    phaseSwitch.begin();
    
    debugSetTelnetEnabled(shouldEnableTelnetNow(config));
    
    if (config.getWifiResetPending()) {
        ESP_LOGI(TAG, "pending reset: clearing stored wifi credentials");
        config.setWifiResetPending(false);
        config.setWifiCredsSet(false);
        wifiNativeEnsureStaMode();
        sleepMs(50);
        esp_wifi_restore();
        wifiNativeDisconnectSta(false);
        wifiNativeDisable();
        sleepMs(50);
    }
    
    phaseSwitch.setSwitchDelay(config.getSwitchDelay());

    bool eth_ok = false;
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
    
    eth_ok = ethernetIsActive();
    if (eth_ok) {
        (void)ethernetWaitForIp(30000);
    }
#endif

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr);

    gpio_reset_pin((gpio_num_t)PIN_FACTORY_LED);
    gpio_set_direction((gpio_num_t)PIN_FACTORY_LED, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_FACTORY_LED, 0);

#ifdef BOARD_DINGTIAN
    if (!eth_ok) {
        ESP_LOGI(TAG, "wifi start");
        prepareStaInterfaceAndSyncCreds(config);
        if (hasConfiguredWifi(config)) {
            wifiNativeConnectSta();
            forceStaOnlyMode(config);
        } else if (shouldStartSetupAp(config)) {
            startWifiSetupAccessPoint(config);
        } else {
            wifiNativeConnectSta();
        }
    } else {
        disableWifiForEthernet();
    }
#else
    ESP_LOGI(TAG, "wifi start");
    prepareStaInterfaceAndSyncCreds(config);
    if (hasConfiguredWifi(config)) {
        wifiNativeConnectSta();
        forceStaOnlyMode(config);
    } else if (shouldStartSetupAp(config)) {
        startWifiSetupAccessPoint(config);
    } else {
        wifiNativeConnectSta();
    }
#endif

    if (shouldStartModbusNow(config)) {
        ESP_LOGI(TAG, "modbus start");
        phaseSwitch.beginModbus();
        s_modbus_started = true;
    }

    setupPages(&phaseSwitch, &config);
    ESP_LOGI(TAG, "Setup finished. Starting main loop.");

    while (1) {
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
            if (ethernetIsActive() && wifiNativeGetMode() == WIFI_MODE_NULL) {
                wifi_disabled_by_eth = true;
            }
            wifi_state_initialized = true;
        }

        const bool eth_active = ethernetIsActive();

        if (eth_active) {
            eth_inactive_since = 0;
            if (wifiNativeGetMode() != WIFI_MODE_NULL) {
                ESP_LOGI(TAG, "wifi disabled due to ethernet");
                disableWifiForEthernet();
                wifi_disabled_by_eth = true;
                wifi_reenable_after = nowMs() + 5000;
            }
        } else {
            if (eth_inactive_since == 0) {
                eth_inactive_since = nowMs();
            }
            if ((nowMs() - eth_inactive_since) < 5000) {
                sleepMs(10);
                phaseSwitch.loop();
                continue;
            }
            if (wifi_reenable_after != 0 && nowMs() < wifi_reenable_after) {
                sleepMs(10);
                phaseSwitch.loop();
                continue;
            }
            if (wifi_disabled_by_eth) {
                ESP_LOGI(TAG, "ethernet down, re-enabling wifi");
                prepareStaInterfaceAndSyncCreds(config);
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
            ESP_LOGI(TAG, "starting modbus after network ready");
            phaseSwitch.beginModbus();
            s_modbus_started = true;
        }

        static uint32_t last_heartbeat = 0;
        if (nowMs() - last_heartbeat > 10000) {
            ESP_LOGI(TAG, "System Heartbeat - Uptime: %u s, RAM: %u bytes", 
                     (unsigned int)(esp_timer_get_time() / 1000000ULL),
                     (unsigned int)esp_get_free_heap_size());
            last_heartbeat = nowMs();
        }

        static uint32_t wifi_no_ip_since = 0;
        static uint32_t wifi_reconnect_since = 0;
        if (isStaModeActive()) {
            if (wifiNativeIsStaConnected() && !wifiNativeIsStaGotIp()) {
                if (wifi_no_ip_since == 0) {
                    wifi_no_ip_since = nowMs();
                } else if (nowMs() - wifi_no_ip_since > 3000) {
                    ESP_LOGI(TAG, "no IP, restarting DHCP");
                    wifiNativeReconnectSta();
                    wifi_no_ip_since = 0;
                }
            } else {
                wifi_no_ip_since = 0;
            }

            if (!wifiNativeIsStaConnected()) {
                if (hasSavedStaSsid()) {
                    if (wifi_reconnect_since == 0) {
                        wifi_reconnect_since = nowMs();
                    } else if (nowMs() - wifi_reconnect_since > 15000) {
                        ESP_LOGI(TAG, "not connected, retrying");
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

        phaseSwitch.loop();
        sleepMs(10);
    }
}

extern "C" void app_main(void)
{
    debugInit();
    
    ESP_LOGI(TAG, "Config load...");
    config.begin();
    
    // Create the system startup and main task.
    // Stack size increased to 12k for safety.
    xTaskCreate(main_task, "main_task", 12288, nullptr, 5, nullptr);
    
    ESP_LOGI(TAG, "app_main returned. Initialization continuing in main_task.");
}
