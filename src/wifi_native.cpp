#include "wifi_native.h"

#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/inet.h"

static bool s_wifi_inited = false;

static bool isOkayOrAlready(esp_err_t err)
{
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

static esp_netif_t *staNetif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static esp_netif_t *apNetif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

static bool parseIpv4(const std::string &in, esp_ip4_addr_t *out)
{
    if (out == NULL) {
        return false;
    }
    ip4_addr_t addr;
    if (ip4addr_aton(in.c_str(), &addr) == 0) {
        return false;
    }
    out->addr = addr.addr;
    return true;
}

static bool parseIpv4Optional(const std::string &in, esp_ip4_addr_t *out)
{
    if (in.empty()) {
        out->addr = 0;
        return false;
    }
    return parseIpv4(in, out);
}

void wifiNativeInit()
{
    if (s_wifi_inited) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_create_default());

    if (staNetif() == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    if (apNetif() == NULL) {
        esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t initErr = esp_wifi_init(&cfg);
    if (!isOkayOrAlready(initErr)) {
        ESP_ERROR_CHECK(initErr);
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    s_wifi_inited = true;
}

wifi_mode_t wifiNativeGetMode()
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return WIFI_MODE_NULL;
    }
    return mode;
}

void wifiNativeEnsureStaMode()
{
    wifiNativeInit();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
}

void wifiNativeEnsureApMode()
{
    wifiNativeInit();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
}

void wifiNativeDisable()
{
    wifiNativeInit();
    wifi_mode_t mode = wifiNativeGetMode();
    if (mode == WIFI_MODE_NULL) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_NULL));
}

void wifiNativeConnectSta()
{
    wifiNativeEnsureStaMode();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

void wifiNativeReconnectSta()
{
    wifiNativeEnsureStaMode();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

void wifiNativeDisconnectSta(bool eraseConfig)
{
    wifiNativeEnsureStaMode();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    if (eraseConfig) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_restore());
    }
}

bool wifiNativeIsStaConnected()
{
    wifi_ap_record_t apInfo = {};
    return esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK;
}

bool wifiNativeIsStaGotIp()
{
    esp_netif_t *netif = staNetif();
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ipInfo = {};
    if (esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) {
        return false;
    }
    return ipInfo.ip.addr != 0;
}

std::string wifiNativeStaIpString()
{
    if (!wifiNativeIsStaGotIp()) {
        return std::string();
    }
    esp_netif_t *netif = staNetif();
    if (netif == NULL) {
        return std::string();
    }
    esp_netif_ip_info_t ipInfo = {};
    if (esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) {
        return std::string();
    }
    char ipBuf[16] = {0};
    if (esp_ip4addr_ntoa(&ipInfo.ip, ipBuf, sizeof(ipBuf)) == NULL) {
        return std::string();
    }
    return std::string(ipBuf);
}

std::string wifiNativeStaMacString()
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        return std::string();
    }
    char out[18];
    snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(out);
}

std::string wifiNativeStaConnectedSsid()
{
    wifi_ap_record_t apInfo = {};
    if (esp_wifi_sta_get_ap_info(&apInfo) != ESP_OK) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char *>(apInfo.ssid));
}

int32_t wifiNativeStaRssi()
{
    wifi_ap_record_t apInfo = {};
    if (esp_wifi_sta_get_ap_info(&apInfo) != ESP_OK) {
        return 0;
    }
    return apInfo.rssi;
}

bool wifiNativeHasStoredStaSsid()
{
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return false;
    }
    return cfg.sta.ssid[0] != '\0';
}

bool wifiNativeSetStaCredentials(const std::string &ssid, const std::string &pass)
{
    wifiNativeEnsureStaMode();
    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(cfg.sta.password), pass.c_str(), sizeof(cfg.sta.password) - 1);
    return esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK;
}

bool wifiNativeSetHostname(const char *hostname)
{
    if (hostname == NULL || hostname[0] == '\0') {
        return false;
    }
    esp_netif_t *netif = staNetif();
    if (netif == NULL) {
        return false;
    }
    return esp_netif_set_hostname(netif, hostname) == ESP_OK;
}

bool wifiNativeSetStaDhcp()
{
    esp_netif_t *netif = staNetif();
    if (netif == NULL) {
        return false;
    }
    esp_err_t stopErr = esp_netif_dhcpc_stop(netif);
    if (!isOkayOrAlready(stopErr)) {
        return false;
    }
    esp_err_t startErr = esp_netif_dhcpc_start(netif);
    return isOkayOrAlready(startErr);
}

bool wifiNativeSetStaStatic(const std::string &ip, const std::string &gw, const std::string &mask, const std::string &dns1, const std::string &dns2)
{
    esp_netif_t *netif = staNetif();
    if (netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ipInfo = {};
    if (!parseIpv4(ip, &ipInfo.ip) || !parseIpv4(gw, &ipInfo.gw) || !parseIpv4(mask, &ipInfo.netmask)) {
        return false;
    }

    esp_err_t stopErr = esp_netif_dhcpc_stop(netif);
    if (!isOkayOrAlready(stopErr)) {
        return false;
    }
    if (esp_netif_set_ip_info(netif, &ipInfo) != ESP_OK) {
        return false;
    }

    esp_netif_dns_info_t dnsInfo = {};
    if (parseIpv4Optional(dns1, &dnsInfo.ip.u_addr.ip4)) {
        dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dnsInfo);
    }
    if (parseIpv4Optional(dns2, &dnsInfo.ip.u_addr.ip4)) {
        dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dnsInfo);
    }
    return true;
}

bool wifiNativeStartAp(const std::string &ssid)
{
    wifiNativeEnsureApMode();
    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char *>(cfg.ap.ssid), ssid.c_str(), sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = ssid.length();
    cfg.ap.channel = 1;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;
    return esp_wifi_set_config(WIFI_IF_AP, &cfg) == ESP_OK;
}

int wifiNativeScan(std::vector<WifiScanEntry> *results)
{
    if (results == NULL) {
        return -1;
    }
    results->clear();

    wifi_mode_t original_mode = wifiNativeGetMode();
    if (original_mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else if (original_mode != WIFI_MODE_APSTA && original_mode != WIFI_MODE_STA) {
        wifiNativeEnsureStaMode();
    }

    wifi_scan_config_t scanCfg = {};
    scanCfg.show_hidden = true;
    if (esp_wifi_scan_start(&scanCfg, true) != ESP_OK) {
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return -1;
    }

    uint16_t apCount = 0;
    if (esp_wifi_scan_get_ap_num(&apCount) != ESP_OK) {
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return -1;
    }
    if (apCount == 0) {
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return 0;
    }

    std::vector<wifi_ap_record_t> records(apCount);
    if (esp_wifi_scan_get_ap_records(&apCount, records.data()) != ESP_OK) {
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return -1;
    }

    if (original_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    results->reserve(apCount);
    for (uint16_t i = 0; i < apCount; i++) {
        std::string ssid(reinterpret_cast<const char *>(records[i].ssid));
        if (ssid.empty()) {
            continue;
        }
        results->push_back({ssid, records[i].rssi});
    }
    return static_cast<int>(results->size());
}
