#include "config.h"
#include <mutex>

#include <ctype.h>
#include <algorithm>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "event_log.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

static const char *DEFAULT_NTP_SERVER = "pool.ntp.org";

static constexpr const char *NVS_NAMESPACE = "hec_1p3p";

static constexpr const char *KEY_WIFI_RESET_PENDING = "wifiRstPend";
static constexpr const char *KEY_WIFI_RESET_PENDING_OLD = "wifiRstP";

static std::string trimCopy(const std::string &in)
{
    auto first = in.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    auto last = in.find_last_not_of(" \t\r\n");
    return in.substr(first, last - first + 1);
}

static std::string sanitizeHostname(const std::string &value)
{
    std::string trimmed = trimCopy(value);
    std::string out;
    out.reserve(32);
    for (size_t i = 0; i < trimmed.length() && out.length() < 32; i++) {
        char c = trimmed[i];
        if (isalnum(static_cast<unsigned char>(c)) || c == '-') {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.front() == '-') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

bool Config::isHostnameValid(const std::string &value)
{
    std::string trimmed = trimCopy(value);
    if (trimmed.length() == 0 || trimmed.length() > 32) {
        return false;
    }
    if (trimmed[0] == '-' || trimmed[trimmed.length() - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < trimmed.length(); i++) {
        char c = trimmed[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-')) {
            return false;
        }
    }
    return true;
}

bool Config::isIpv4Valid(const std::string &value, bool allowEmpty)
{
    // "255.255.255.255" has 15 characters; the value is saved as it came,
    // and NVS refuses strings of 4000 bytes and more.
    if (value.length() > 32) {
        return false;
    }
    // No blanks around it: the network setup at the next start parses the
    // saved text and refuses e.g. a leading blank, the static address would
    // then silently not be used.
    if (trimCopy(value).length() != value.length()) {
        return false;
    }
    if (value.empty()) {
        return allowEmpty;
    }
    ip4_addr_t parsed;
    return ip4addr_aton(value.c_str(), &parsed) != 0;
}

Config::Config()
    :_nvsReady(false)
    ,_nvsHandle(0)
    ,_switchDelay(120000)
    ,_ethDhcp(true)
    ,_ethIp("192.168.178.200")
    ,_ethGw("192.168.178.1")
    ,_ethMask("255.255.255.0")
    ,_ethDns1("192.168.178.1")
    ,_ethDns2("")
    ,_wifiDhcp(true)
    ,_wifiIp("192.168.178.201")
    ,_wifiGw("192.168.178.1")
    ,_wifiMask("255.255.255.0")
    ,_wifiDns1("192.168.178.1")
    ,_wifiDns2("")
    ,_wifiCredsSet(false)
    ,_wifiResetPending(false)
    ,_modbusEnabled(true)
    ,_telnetEnabled(false)
    ,_hostname("heidelberg-1p3p")
    ,_ntpServer(DEFAULT_NTP_SERVER)
{}

void Config::begin()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &_nvsHandle);
    ESP_ERROR_CHECK(err);
    _nvsReady = true;

    const uint32_t switchDelay = readUInt32("switchDelay", _switchDelay);
    // a value saved before the range check: bring it into range
    _switchDelay = switchDelay < MIN_SWITCH_DELAY_MS ? MIN_SWITCH_DELAY_MS
      : switchDelay > MAX_SWITCH_DELAY_MS ? MAX_SWITCH_DELAY_MS : switchDelay;
    _ethDhcp = readBool("ethDhcp", _ethDhcp);
    _ethIp = readString("ethIp", _ethIp);
    _ethGw = readString("ethGw", _ethGw);
    _ethMask = readString("ethMask", _ethMask);
    _ethDns1 = readString("ethDns1", _ethDns1);
    _ethDns2 = readString("ethDns2", _ethDns2);
    _wifiDhcp = readBool("wifiDhcp", _wifiDhcp);
    _wifiIp = readString("wifiIp", _wifiIp);
    _wifiGw = readString("wifiGw", _wifiGw);
    _wifiMask = readString("wifiMask", _wifiMask);
    _wifiDns1 = readString("wifiDns1", _wifiDns1);
    _wifiDns2 = readString("wifiDns2", _wifiDns2);
    _wifiCredsSet = readBool("wifiCredsSet", _wifiCredsSet);

    bool wifiResetPending = false;
    if (tryReadBool(KEY_WIFI_RESET_PENDING, &wifiResetPending)) {
        _wifiResetPending = wifiResetPending;
    } else if (tryReadBool(KEY_WIFI_RESET_PENDING_OLD, &wifiResetPending)) {
        _wifiResetPending = wifiResetPending;
    }

    _modbusEnabled = readBool("modbusEnabled", _modbusEnabled);
    _telnetEnabled = readBool("telnetEnabled", _telnetEnabled);

    _ntpServer = readString("ntpServer", _ntpServer);
    if (_ntpServer.empty() || !isNtpServerValid(_ntpServer)) _ntpServer = DEFAULT_NTP_SERVER;

    std::string storedHostname = readString("hostname", _hostname);
    if (isHostnameValid(storedHostname)) {
        _hostname = storedHostname;
    } else {
        std::string sanitized = sanitizeHostname(storedHostname);
        if (isHostnameValid(sanitized)) {
            _hostname = sanitized;
        }
    }
}

bool Config::readBool(const char *key, bool defaultValue)
{
    bool value = defaultValue;
    if (tryReadBool(key, &value)) {
        return value;
    }
    return defaultValue;
}

bool Config::tryReadBool(const char *key, bool *outValue)
{
    if (!_nvsReady || outValue == NULL) {
        return false;
    }
    uint8_t raw = 0;
    esp_err_t err = nvs_get_u8(_nvsHandle, key, &raw);
    if (err == ESP_OK) {
        *outValue = (raw != 0);
        return true;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
    }
    return false;
}

uint32_t Config::readUInt32(const char *key, uint32_t defaultValue)
{
    if (!_nvsReady) {
        return defaultValue;
    }
    uint32_t value = defaultValue;
    esp_err_t err = nvs_get_u32(_nvsHandle, key, &value);
    if (err == ESP_OK) {
        return value;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
    }
    return defaultValue;
}

std::string Config::readString(const char *key, const std::string &defaultValue)
{
    if (!_nvsReady) {
        return defaultValue;
    }
    size_t requiredSize = 0;
    esp_err_t err = nvs_get_str(_nvsHandle, key, NULL, &requiredSize);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return defaultValue;
    }
    ESP_ERROR_CHECK(err);
    if (requiredSize == 0) {
        return defaultValue;
    }

    std::vector<char> buffer(requiredSize, '\0');
    err = nvs_get_str(_nvsHandle, key, buffer.data(), &requiredSize);
    ESP_ERROR_CHECK(err);
    return std::string(buffer.data());
}

// A failed save must not restart the board: the contactors would drop while
// the car charges. The setter keeps the old value, so the page shows what
// is really saved and saving again tries again. One event per boot is
// enough: the WiFi state is saved again on every connect.
static bool commitOrLog(nvs_handle_t handle, const char *key, esp_err_t err)
{
    static bool s_eventLogged = false;
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) return true;
    ESP_LOGE("Config", "saving %s failed: %s", key, esp_err_to_name(err));
    if (!s_eventLogged) {
        s_eventLogged = true;
        eventLog(EventType::SettingNotSaved, 0, 0, 0, (uint32_t)err);
    }
    return false;
}

bool Config::writeBool(const char *key, bool value)
{
    if (!_nvsReady) {
        return false;
    }
    return commitOrLog(_nvsHandle, key, nvs_set_u8(_nvsHandle, key, value ? 1 : 0));
}

bool Config::writeUInt32(const char *key, uint32_t value)
{
    if (!_nvsReady) {
        return false;
    }
    return commitOrLog(_nvsHandle, key, nvs_set_u32(_nvsHandle, key, value));
}

bool Config::writeString(const char *key, const std::string &value)
{
    if (!_nvsReady) {
        return false;
    }
    return commitOrLog(_nvsHandle, key, nvs_set_str(_nvsHandle, key, value.c_str()));
}

uint32_t Config::getSwitchDelay(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _switchDelay;
}

bool Config::setSwitchDelay(uint32_t value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (!isSwitchDelayValid(value)) return false;
    if (_switchDelay == value) return true;
    if (!writeUInt32("switchDelay", value)) return false;
    _switchDelay = value;
    return true;
}

bool Config::getEthDhcp(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ethDhcp;
}

bool Config::setEthDhcp(bool value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_ethDhcp == value) return true;
    if (!writeBool("ethDhcp", value)) return false;
    _ethDhcp = value;
    return true;
}

std::string Config::getEthIp(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ethIp;
}

bool Config::setEthIp(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_ethIp == value) return true;
    if (!writeString("ethIp", value)) return false;
    _ethIp = value;
    return true;
}

std::string Config::getEthGw(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ethGw;
}

bool Config::setEthGw(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_ethGw == value) return true;
    if (!writeString("ethGw", value)) return false;
    _ethGw = value;
    return true;
}

std::string Config::getEthMask(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ethMask;
}

bool Config::setEthMask(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_ethMask == value) return true;
    if (!writeString("ethMask", value)) return false;
    _ethMask = value;
    return true;
}

std::string Config::getEthDns1(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ethDns1;
}

bool Config::setEthDns1(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_ethDns1 == value) return true;
    if (!writeString("ethDns1", value)) return false;
    _ethDns1 = value;
    return true;
}

std::string Config::getEthDns2(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ethDns2;
}

bool Config::setEthDns2(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_ethDns2 == value) return true;
    if (!writeString("ethDns2", value)) return false;
    _ethDns2 = value;
    return true;
}

bool Config::getWifiDhcp(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiDhcp;
}

bool Config::setWifiDhcp(bool value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiDhcp == value) return true;
    if (!writeBool("wifiDhcp", value)) return false;
    _wifiDhcp = value;
    return true;
}

std::string Config::getWifiIp(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiIp;
}

bool Config::setWifiIp(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiIp == value) return true;
    if (!writeString("wifiIp", value)) return false;
    _wifiIp = value;
    return true;
}

std::string Config::getWifiGw(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiGw;
}

bool Config::setWifiGw(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiGw == value) return true;
    if (!writeString("wifiGw", value)) return false;
    _wifiGw = value;
    return true;
}

std::string Config::getWifiMask(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiMask;
}

bool Config::setWifiMask(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiMask == value) return true;
    if (!writeString("wifiMask", value)) return false;
    _wifiMask = value;
    return true;
}

std::string Config::getWifiDns1(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiDns1;
}

bool Config::setWifiDns1(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiDns1 == value) return true;
    if (!writeString("wifiDns1", value)) return false;
    _wifiDns1 = value;
    return true;
}

std::string Config::getWifiDns2(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiDns2;
}

bool Config::setWifiDns2(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiDns2 == value) return true;
    if (!writeString("wifiDns2", value)) return false;
    _wifiDns2 = value;
    return true;
}

bool Config::getWifiCredsSet(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiCredsSet;
}

bool Config::setWifiCredsSet(bool value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiCredsSet == value) return true;
    if (!writeBool("wifiCredsSet", value)) return false;
    _wifiCredsSet = value;
    return true;
}

bool Config::getWifiResetPending(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _wifiResetPending;
}

bool Config::setWifiResetPending(bool value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wifiResetPending == value) return true;
    if (!writeBool(KEY_WIFI_RESET_PENDING, value)) return false;
    _wifiResetPending = value;
    return true;
}

bool Config::getModbusEnabled(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _modbusEnabled;
}

bool Config::setModbusEnabled(bool value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_modbusEnabled == value) return true;
    if (!writeBool("modbusEnabled", value)) return false;
    _modbusEnabled = value;
    return true;
}

bool Config::getTelnetEnabled(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _telnetEnabled;
}

bool Config::setTelnetEnabled(bool value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (_telnetEnabled == value) return true;
    if (!writeBool("telnetEnabled", value)) return false;
    _telnetEnabled = value;
    return true;
}

std::string Config::getHostname(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _hostname;
}

bool Config::setHostname(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    if (!isHostnameValid(value)) return false;
    std::string sanitized = sanitizeHostname(value);
    if (_hostname == sanitized) return true;
    if (!writeString("hostname", sanitized)) return false;
    _hostname = sanitized;
    return true;
}

std::string Config::getNtpServer(){
    std::lock_guard<std::mutex> lock(_mutex);
    return _ntpServer;
}

// empty restores the default server
bool Config::setNtpServer(const std::string &value){
    std::lock_guard<std::mutex> lock(_mutex);
    const std::string server = value.empty() ? std::string(DEFAULT_NTP_SERVER) : value;
    if (!isNtpServerValid(server)) return false;
    if (_ntpServer == server) return true;
    if (!writeString("ntpServer", server)) return false;
    _ntpServer = server;
    return true;
}

// host name or IPv4 address, empty for the default
bool Config::isSwitchDelayValid(uint32_t value)
{
    return value >= MIN_SWITCH_DELAY_MS && value <= MAX_SWITCH_DELAY_MS;
}

bool Config::isNtpServerValid(const std::string &value)
{
    if (value.length() > 63) return false;
    for (char c : value) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '.') return false;
    }
    return value.empty() || (value.front() != '.' && value.back() != '.');
}
