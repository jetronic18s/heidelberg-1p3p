#ifndef CONFIG_H
    #define CONFIG_H
    #include <cstdint>
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
            bool readBool(const char *key, bool defaultValue);
            bool tryReadBool(const char *key, bool *outValue);
            uint32_t readUInt32(const char *key, uint32_t defaultValue);
            std::string readString(const char *key, const std::string &defaultValue);
            void writeBool(const char *key, bool value);
            void writeUInt32(const char *key, uint32_t value);
            void writeString(const char *key, const std::string &value);
        public:
            Config();
            void begin();
            uint32_t getSwitchDelay();
            void setSwitchDelay(uint32_t value);
            bool getEthDhcp();
            void setEthDhcp(bool value);
            std::string getEthIp();
            void setEthIp(const std::string &value);
            std::string getEthGw();
            void setEthGw(const std::string &value);
            std::string getEthMask();
            void setEthMask(const std::string &value);
            std::string getEthDns1();
            void setEthDns1(const std::string &value);
            std::string getEthDns2();
            void setEthDns2(const std::string &value);
            bool getWifiDhcp();
            void setWifiDhcp(bool value);
            std::string getWifiIp();
            void setWifiIp(const std::string &value);
            std::string getWifiGw();
            void setWifiGw(const std::string &value);
            std::string getWifiMask();
            void setWifiMask(const std::string &value);
            std::string getWifiDns1();
            void setWifiDns1(const std::string &value);
            std::string getWifiDns2();
            void setWifiDns2(const std::string &value);
            bool getWifiCredsSet();
            void setWifiCredsSet(bool value);
            bool getWifiResetPending();
            void setWifiResetPending(bool value);
            bool getModbusEnabled();
            void setModbusEnabled(bool value);
            bool getTelnetEnabled();
            void setTelnetEnabled(bool value);
            std::string getHostname();
            void setHostname(const std::string &value);
            static bool isHostnameValid(const std::string &value);
    };

#endif /* CONFIG_H */
