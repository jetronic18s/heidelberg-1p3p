#ifndef SWITCH_H
    #define SWITCH_H
    #include <cstddef>
    #include <cstdint>
    #include <string>
    #include <vector>
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include <mbcontroller.h>
    #include "config.h"
    #include "debug.h"

    #define HEC_REG_REMOTE_LOCK (uint16_t)259
    #define HEC_REG_MAX_CURRENT (uint16_t)261
    #define HOLDING_REG_OFFSET (uint16_t)256

    enum State {
        //wait for phase switch
        Running,
        //received request to switch phases
        SwitchPhases,    
        //sent MaxCurrent=0
        WaitingForZero, 
        //received L1-3 Current = 0
        ConfirmedZero,  
        //switched Wallbox off
        WaitingForOff,
        //confirmed all is off
        ConfirmedOff,
        //switched Wallbox on with desired phases
        SwitchedOn,
        //confirmed phases -> Wait for Delay,
        Delay,
        //delay passed -> Running
        Fault,
    };

    class PhaseSwitch{
        private:
            uint32_t _previous;
            uint32_t _delay;
            State _state;
            uint8_t _desiredPhases;
            bool _switchingSupported;
            bool _firmwareSupported;
            std::vector<uint16_t> _inputRegister;
            std::vector<uint16_t> _holdingRegister;
            uint32_t _switchDelay;
            bool validateSetup();
            uint8_t getActivePhases();
            //modbus
            void* _master_handle;
            TaskHandle_t _tcpTaskHandle;
            uint8_t _serverId;
            int _listenFd;
            bool _tcpServerStarted;
            uint32_t _rtuMessageCount;
            uint32_t _rtuPendingRequestCount;
            uint32_t _rtuErrorCount;
            uint32_t _bridgeMessageCount;
            uint32_t _bridgeActiveClientCount;
            uint32_t _bridgeErrorCount;
            uint16_t _safetyFaultCode;
            std::string _safetyFaultText;
            uint32_t _switchOnDeadlineMs;
            void enterSafetyFault(uint16_t code, const char *text);
            bool hasSafetyFault();
            
            // Native Modbus helpers
            esp_err_t masterReadInput(uint16_t addr, uint16_t count, uint16_t* dest);
            esp_err_t masterReadHolding(uint16_t addr, uint16_t count, uint16_t* dest);
            esp_err_t masterWriteHolding(uint16_t addr, uint16_t value);
            esp_err_t masterWriteMultipleHolding(uint16_t addr, uint16_t count, const uint16_t* values);
            void startTcpBridge();
            void tcpBridgeTask();
            void handleTcpClient(int clientFd);
            bool handleModbusRequest(uint8_t unitId, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& responsePdu);
            bool handleReadHolding(uint8_t unitId, uint16_t addr, uint16_t count, std::vector<uint8_t>& responsePdu);
            bool handleReadInput(uint8_t unitId, uint16_t addr, uint16_t count, std::vector<uint8_t>& responsePdu);
            bool handleWriteHolding(uint8_t unitId, uint16_t addr, uint16_t value, std::vector<uint8_t>& responsePdu);
            bool handleWriteMultiple(uint8_t unitId, uint16_t addr, uint16_t count, const uint16_t* values, std::vector<uint8_t>& responsePdu);
            void buildException(uint8_t functionCode, uint8_t exceptionCode, std::vector<uint8_t>& responsePdu);
            void buildReadResponse(uint8_t functionCode, const uint16_t* values, uint16_t count, std::vector<uint8_t>& responsePdu);
            void cacheHolding(uint16_t addr, const uint16_t* values, uint16_t count);
            void cacheInput(uint16_t addr, const uint16_t* values, uint16_t count);
            bool readCachedHolding(uint16_t addr, uint16_t count, std::vector<uint16_t>& values);
            bool readCachedInput(uint16_t addr, uint16_t count, std::vector<uint16_t>& values);
            
        public:
            PhaseSwitch();
            void begin();
            void beginModbus();
            void loop();
            void switchTo1P();
            void switchTo3P();
            bool canSwitchTo1P();
            bool canSwitchTo3P();
            void setSwitchDelay(uint32_t delayMs);
            uint32_t getRtuMessageCount();
            uint32_t getRtuPendingRequestCount();
            uint32_t getRtuErrorCount();
            uint32_t getBridgeMessageCount();
            uint32_t getBridgeActiveClientCount();
            uint32_t getBridgeErrorCount();
            std::string getState();
            uint16_t getSafetyFaultCode();
            std::string getSafetyFaultText();
            uint16_t getHoldingRegister(size_t reg);
            uint16_t getInputRegister(size_t reg);
            bool updateCachedRegisters();
    };
#endif /* SWITCH_H */
