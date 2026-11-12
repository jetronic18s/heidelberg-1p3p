#ifndef CONFIG_H
    #define CONFIG_H
    #include <cstdint>
    #include <mutex>
    #include <nvs.h>
    #include <string>

    #ifdef BOARD_DINGTIAN
        #define PIN_1P_IN 36
        #define PIN_1P_OUT 16
        #define PIN_3P_IN 39
        #define PIN_3P_OUT 2
        #define PIN_RS485_DE 33
        #define PIN_FACTORY_LED 32
        #define PIN_FACTORY_BTN 34
        #define PIN_ETH_MDC 23
        #define PIN_ETH_MDIO 18
        #define PIN_ETH_PWR 0
        #define PIN_ETH_CLK 17
        #define RELAY_ON 1
        #define RELAY_OFF 0
    #else
        #define PIN_1P_IN 33
        #define PIN_1P_OUT 26
        #define PIN_3P_IN 25
        #define PIN_3P_OUT 27
        #define PIN_RS485_DE -1
        #define RELAY_ON 0
        #define RELAY_OFF 1
    #endif

    class Config{
        private:
            // The web server writes the settings while the main loop and
            // others read them; std::string copies need the lock.
            mutable std::mutex _mutex;
            bool _nvsReady;
            nvs_handle_t _nvsHandle;
            uint32_t _switchDelay;
            bool _ethDhcp;
            std::string _ethIp;
            std::string _ethGw;
            std::string _ethMask;
            std::string _ethDns1;
            std::string _ethDns2;
            bool _wifiDhcp;
            std::string _wifiIp;
            std::string _wifiGw;
            std::string _wifiMask;
            std::string _wifiDns1;
            std::string _wifiDns2;
            bool _wifiCredsSet;
            bool _wifiResetPending;
            bool _modbusEnabled;
            bool _telnetEnabled;
            std::string _hostname;
            std::string _ntpServer;
            bool readBool(const char *key, bool defaultValue);
            bool tryReadBool(const char *key, bool *outValue);
            uint32_t readUInt32(const char *key, uint32_t defaultValue);
            std::string readString(const char *key, const std::string &defaultValue);
            bool writeBool(const char *key, bool value);
            bool writeUInt32(const char *key, uint32_t value);
            bool writeString(const char *key, const std::string &value);
        public:
            Config();
            void begin();
            // The setters return false when the value is invalid or could not be
            // saved; the old value then stays. An unchanged value counts as saved.
            uint32_t getSwitchDelay();
            bool setSwitchDelay(uint32_t value);
            bool getEthDhcp();
            bool setEthDhcp(bool value);
            std::string getEthIp();
            bool setEthIp(const std::string &value);
            std::string getEthGw();
            bool setEthGw(const std::string &value);
            std::string getEthMask();
            bool setEthMask(const std::string &value);
            std::string getEthDns1();
            bool setEthDns1(const std::string &value);
            std::string getEthDns2();
            bool setEthDns2(const std::string &value);
            bool getWifiDhcp();
            bool setWifiDhcp(bool value);
            std::string getWifiIp();
            bool setWifiIp(const std::string &value);
            std::string getWifiGw();
            bool setWifiGw(const std::string &value);
            std::string getWifiMask();
            bool setWifiMask(const std::string &value);
            std::string getWifiDns1();
            bool setWifiDns1(const std::string &value);
            std::string getWifiDns2();
            bool setWifiDns2(const std::string &value);
            bool getWifiCredsSet();
            bool setWifiCredsSet(bool value);
            bool getWifiResetPending();
            bool setWifiResetPending(bool value);
            bool getModbusEnabled();
            bool setModbusEnabled(bool value);
            bool getTelnetEnabled();
            bool setTelnetEnabled(bool value);
            std::string getHostname();
            bool setHostname(const std::string &value);
            std::string getNtpServer();
            bool setNtpServer(const std::string &value);
            static bool isHostnameValid(const std::string &value);
            static bool isNtpServerValid(const std::string &value);
            // The contactors need time to open; longer than 10 min is a typo.
            static constexpr uint32_t MIN_SWITCH_DELAY_MS = 1000;
            static constexpr uint32_t MAX_SWITCH_DELAY_MS = 600000;
            static bool isSwitchDelayValid(uint32_t value);
            static bool isIpv4Valid(const std::string &value, bool allowEmpty = false);
    };

#endif /* CONFIG_H */
