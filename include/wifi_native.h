#ifndef WIFI_NATIVE_H
#define WIFI_NATIVE_H

#include <esp_wifi.h>
#include <string>
#include <vector>

struct WifiScanEntry {
    std::string ssid;
    int32_t rssi;
};

void wifiNativeInit();
void wifiNativeEnsureStaMode();
void wifiNativeEnsureApMode();
void wifiNativeDisable();
void wifiNativeConnectSta();
void wifiNativeReconnectSta();
void wifiNativeDisconnectSta(bool eraseConfig);

wifi_mode_t wifiNativeGetMode();
bool wifiNativeIsStaConnected();
bool wifiNativeIsStaGotIp();
std::string wifiNativeStaIpString();
std::string wifiNativeStaMacString();
std::string wifiNativeStaConnectedSsid();
int32_t wifiNativeStaRssi();
bool wifiNativeHasStoredStaSsid();
bool wifiNativeSetStaCredentials(const std::string &ssid, const std::string &pass);
bool wifiNativeSetHostname(const char *hostname);
bool wifiNativeSetStaDhcp();
bool wifiNativeSetStaStatic(const std::string &ip, const std::string &gw, const std::string &mask, const std::string &dns1, const std::string &dns2);
bool wifiNativeStartAp(const std::string &ssid);
int wifiNativeScan(std::vector<WifiScanEntry> *results);

#endif /* WIFI_NATIVE_H */
