#ifndef SWITCH_H
    #define SWITCH_H
    #include <cstddef>
    #include <cstdint>
    #include <string>
    #include <vector>
    #include <ModbusClientRTU.h>
    #include "modbus_tcp_bridge.h"
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
            ModbusClientRTU _client;
            ModbusTcpBridge _bridge;
            uint8_t _serverId;
            uint32_t _requestToken;
            uint16_t _safetyFaultCode;
            std::string _safetyFaultText;
            uint32_t _switchOnDeadlineMs;
            void enterSafetyFault(uint16_t code, const char *text);
            bool hasSafetyFault();
            ModbusMessage onWriteHolding(ModbusMessage msg);
            ModbusMessage bridgeCall(ModbusMessage msg);
            ModbusMessage cacheWriteHolding(ModbusMessage msg);
            ModbusMessage onWriteMultiple(ModbusMessage msg);
            ModbusMessage cacheWriteMultiple(ModbusMessage msg);
            ModbusMessage onReadHolding(ModbusMessage msg);
            ModbusMessage cacheReadHolding(ModbusMessage msg);
            ModbusMessage onReadInput(ModbusMessage msg);
            ModbusMessage cacheReadInput(ModbusMessage msg);
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
            ModbusMessage sendRtuRequest(uint8_t serverID, uint8_t functionCode, uint16_t p1, uint16_t p2);
            std::string getState();
            uint16_t getSafetyFaultCode();
            std::string getSafetyFaultText();
            uint16_t getHoldingRegister(size_t reg);
            uint16_t getInputRegister(size_t reg);
            bool updateCachedRegisters();
    };
#endif /* SWITCH_H */
