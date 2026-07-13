#include "config.h"

#include <ctype.h>
#include <algorithm>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

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

    _switchDelay = readUInt32("switchDelay", _switchDelay);
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

void Config::writeBool(const char *key, bool value)
{
    if (!_nvsReady) {
        return;
    }
    ESP_ERROR_CHECK(nvs_set_u8(_nvsHandle, key, value ? 1 : 0));
    ESP_ERROR_CHECK(nvs_commit(_nvsHandle));
}

void Config::writeUInt32(const char *key, uint32_t value)
{
    if (!_nvsReady) {
        return;
    }
    ESP_ERROR_CHECK(nvs_set_u32(_nvsHandle, key, value));
    ESP_ERROR_CHECK(nvs_commit(_nvsHandle));
}

void Config::writeString(const char *key, const std::string &value)
{
    if (!_nvsReady) {
        return;
    }
    ESP_ERROR_CHECK(nvs_set_str(_nvsHandle, key, value.c_str()));
    ESP_ERROR_CHECK(nvs_commit(_nvsHandle));
}

uint32_t Config::getSwitchDelay(){
    return _switchDelay;
}

void Config::setSwitchDelay(uint32_t value){
    if (_switchDelay == value) return;
    _switchDelay = value;
    writeUInt32("switchDelay", _switchDelay);
}

bool Config::getEthDhcp(){
    return _ethDhcp;
}

void Config::setEthDhcp(bool value){
    if (_ethDhcp == value) return;
    _ethDhcp = value;
    writeBool("ethDhcp", _ethDhcp);
}

std::string Config::getEthIp(){
    return _ethIp;
}

void Config::setEthIp(const std::string &value){
    if (_ethIp == value) return;
    _ethIp = value;
    writeString("ethIp", _ethIp);
}

std::string Config::getEthGw(){
    return _ethGw;
}

void Config::setEthGw(const std::string &value){
    if (_ethGw == value) return;
    _ethGw = value;
    writeString("ethGw", _ethGw);
}

std::string Config::getEthMask(){
    return _ethMask;
}

void Config::setEthMask(const std::string &value){
    if (_ethMask == value) return;
    _ethMask = value;
    writeString("ethMask", _ethMask);
}

std::string Config::getEthDns1(){
    return _ethDns1;
}

void Config::setEthDns1(const std::string &value){
    if (_ethDns1 == value) return;
    _ethDns1 = value;
    writeString("ethDns1", _ethDns1);
}

std::string Config::getEthDns2(){
    return _ethDns2;
}

void Config::setEthDns2(const std::string &value){
    if (_ethDns2 == value) return;
    _ethDns2 = value;
    writeString("ethDns2", _ethDns2);
}

bool Config::getWifiDhcp(){
    return _wifiDhcp;
}

void Config::setWifiDhcp(bool value){
    if (_wifiDhcp == value) return;
    _wifiDhcp = value;
    writeBool("wifiDhcp", _wifiDhcp);
}

std::string Config::getWifiIp(){
    return _wifiIp;
}

void Config::setWifiIp(const std::string &value){
    if (_wifiIp == value) return;
    _wifiIp = value;
    writeString("wifiIp", _wifiIp);
}

std::string Config::getWifiGw(){
    return _wifiGw;
}

void Config::setWifiGw(const std::string &value){
    if (_wifiGw == value) return;
    _wifiGw = value;
    writeString("wifiGw", _wifiGw);
}

std::string Config::getWifiMask(){
    return _wifiMask;
}

void Config::setWifiMask(const std::string &value){
    if (_wifiMask == value) return;
    _wifiMask = value;
    writeString("wifiMask", _wifiMask);
}

std::string Config::getWifiDns1(){
    return _wifiDns1;
}

void Config::setWifiDns1(const std::string &value){
    if (_wifiDns1 == value) return;
    _wifiDns1 = value;
    writeString("wifiDns1", _wifiDns1);
}

std::string Config::getWifiDns2(){
    return _wifiDns2;
}

void Config::setWifiDns2(const std::string &value){
    if (_wifiDns2 == value) return;
    _wifiDns2 = value;
    writeString("wifiDns2", _wifiDns2);
}

bool Config::getWifiCredsSet(){
    return _wifiCredsSet;
}

void Config::setWifiCredsSet(bool value){
    if (_wifiCredsSet == value) return;
    _wifiCredsSet = value;
    writeBool("wifiCredsSet", _wifiCredsSet);
}

bool Config::getWifiResetPending(){
    return _wifiResetPending;
}

void Config::setWifiResetPending(bool value){
    if (_wifiResetPending == value) return;
    _wifiResetPending = value;
    writeBool(KEY_WIFI_RESET_PENDING, _wifiResetPending);
}

bool Config::getModbusEnabled(){
    return _modbusEnabled;
}

void Config::setModbusEnabled(bool value){
    if (_modbusEnabled == value) return;
    _modbusEnabled = value;
    writeBool("modbusEnabled", _modbusEnabled);
}

bool Config::getTelnetEnabled(){
    return _telnetEnabled;
}

void Config::setTelnetEnabled(bool value){
    if (_telnetEnabled == value) return;
    _telnetEnabled = value;
    writeBool("telnetEnabled", _telnetEnabled);
}

std::string Config::getHostname(){
    return _hostname;
}

void Config::setHostname(const std::string &value){
    if (!isHostnameValid(value)) return;
    std::string sanitized = sanitizeHostname(value);
    if (_hostname == sanitized) return;
    _hostname = sanitized;
    writeString("hostname", _hostname);
}
