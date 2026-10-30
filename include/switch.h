#ifndef SWITCH_H
    #define SWITCH_H
    #include <cstddef>
    #include <cstdint>
    #include <atomic>
    #include <mutex>
    #include <string>
    #include <vector>
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include <mbcontroller.h>
    #include "config.h"
    #include "debug.h"

    #define HEC_REG_WATCHDOG (uint16_t)257
    #define HEC_REG_REMOTE_LOCK (uint16_t)259
    #define HEC_REG_MAX_CURRENT (uint16_t)261
    #define HEC_REG_FAILSAFE_CURRENT (uint16_t)262
    #define HEC_MIN_CURRENT (uint16_t)60 // 6.0 A in 0.1 A units
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
        //delay passed -> Unlocking
        Fault,
        //released the remote lock, waiting until the wallbox reports it -> Running
        Unlocking,
        //restart requested: sent MaxCurrent=0 and lock, waiting for both
        Restarting,
    };

    class PhaseSwitch{
        private:
            uint32_t _previous;
            uint32_t _delay;
            std::atomic<State> _state; // read by the bridge tasks without the mutex
            uint8_t _desiredPhases;
            bool _phaseSwitchRequested; // false while the boot sequence runs
            bool _currentWrittenDuringSwitch; // evcc set the current for the new phase count
            bool _switchingSupported;
            bool _firmwareSupported;
            bool _bootCacheValid; // cache restored from before a software restart
            uint16_t _reportedStatus; // charging state reported outside Running
            std::vector<uint16_t> _inputRegister;
            std::vector<uint16_t> _holdingRegister;
            uint32_t _switchDelay;
            bool validateSetup();
            uint8_t getActivePhases();
            //modbus
            void* _master_handle;
            bool _masterStarted;
            TaskHandle_t _tcpTaskHandle;
            uint8_t _serverId;
            int _listenFd;
            bool _tcpServerStarted;
            // RTU requests come from the main loop and the Modbus TCP bridge task
            std::atomic<uint32_t> _rtuMessageCount;
            std::atomic<uint32_t> _rtuPendingRequestCount;
            std::atomic<uint32_t> _rtuErrorCount;
            uint32_t _bridgeMessageCount;
            uint32_t _bridgeActiveClientCount;
            uint32_t _bridgeErrorCount;
            uint16_t _safetyFaultCode;
            std::string _safetyFaultText;
            uint32_t _switchOnDeadlineMs;
            uint32_t _wallboxResponseDeadlineMs;
            uint32_t _unlockDeadlineMs;
            std::atomic<bool> _restartRequested;
            std::atomic<bool> _updateInProgress;
            // The main loop, the bridge tasks (evcc) and the web page start
            // switches; the loop holds it for a whole pass.
            std::mutex _stateMutex;
            std::atomic<uint8_t> _requestedPhases; // phases requested outside Running, 0 = none
            std::atomic<uint32_t> _lastBridgeRequestMs; // last Modbus TCP request (evcc)
            std::atomic<bool> _evccTimeout; // failsafe current applied, evcc silent
            // Keeps a request from evcc out of the watchdog's check and failsafe
            // write, so the failsafe cannot overwrite evcc's next current.
            std::mutex _evccMutex;
            std::atomic<uint32_t> _lastWallboxOkMs; // last successful Modbus RTU transaction
            std::atomic<bool> _wallboxSilent; // wallbox not answering in Running
            uint32_t _wallboxSilentSinceMs;
            // wallbox and evcc diagnostics for the status page
            std::atomic<uint32_t> _rtuWindowStartMs, _rtuWindowMsgs, _rtuWindowErrs;
            std::atomic<uint32_t> _rtuPrevMsgs, _rtuPrevErrs;
            std::atomic<bool> _rtuPrevValid;
            std::atomic<esp_err_t> _lastRtuError;
            std::atomic<uint32_t> _lastRtuErrorMs;
            uint32_t _wallboxOutages, _lastOutageEndMs, _lastOutageMs;
            uint32_t _evccTimeouts;
            uint16_t _hwMaxCurrent, _hwMinCurrent, _swRevision;
            uint32_t _switchCount, _switchStartMs, _lastSwitchDoneMs, _lastSwitchMs;
            uint8_t _lastSwitchFrom, _lastSwitchTo;
            std::atomic<uint32_t> _lastClientAddr; // last Modbus TCP client, network order
            uint32_t _restartDeadlineMs;
            void handleRestart();
            void requestPhases(uint8_t phases);
            void startSwitch(uint8_t phases);
            uint8_t reportedPhases();
            void checkEvccWatchdog();
            void checkWallboxSilent();
            void noteRtuResult(esp_err_t err);
            void finishSwitchStats();
            void rememberCarState();
            void saveBootCache();
            void loadBootCache();
            [[noreturn]] void restartNow();
            bool servesFromCache();
            bool waitingForWallbox();
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
            // Stop charging, then restart the ESP32 from the main loop.
            void requestRestart();
            // Answer evcc from the cache while an OTA upload stalls Modbus RTU.
            void setUpdateInProgress(bool inProgress);
            bool isWallboxSilent();
            uint32_t wallboxSilentForMs();
            std::string wallboxStatusText();
            std::string wallboxHistoryText();
            std::string wallboxInfoText();
            std::string evccStatusText();
            std::string evccClientText();
            std::string switchStatusText();
            std::string contactorText();
            bool bootCacheRestored();
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
