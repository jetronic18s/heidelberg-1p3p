#ifndef EVENT_LOG_H
    #define EVENT_LOG_H
    #include <cstdint>
    #include <string>

    // Event log for the status page, kept in RTC memory: it survives a
    // software restart, an OTA update and a crash, but not a power loss.
    enum class EventType : uint8_t {
        Start = 1,          // a = reset reason
        RestartRequested,   // a = RestartSource
        PhaseSwitch,        // a = from, b = to, c = SwitchSource, value = duration ms
        CarConnected,
        CarDisconnected,
        ChargeStart,
        ChargeEnd,          // value = energy Wh, 0xFFFFFFFF unknown
        EvccTimeout,        // value = failsafe current in 0.1 A
        EvccBack,           // value = duration ms
        WallboxSilent,
        WallboxBack,        // value = duration ms
        SafetyFault,        // value = fault code
        ModbusRefused,      // value = refused connections since the last entry
        LogCleared,         // the log was cleared on the web page
        ChargePaused,       // the car waits for current (status C1)
        ChargeResumed,
        SwitchAborted,      // a = from, b = to, c = SwitchSource
        SettingNotSaved,    // value = esp_err_t; once per boot
    };

    enum class RestartSource : uint8_t { WebPage = 1, FirmwareUpdate, WifiSettings };
    enum class SwitchSource : uint8_t { Evcc = 1, WebPage };

    static constexpr uint32_t EVENT_VALUE_UNKNOWN = 0xFFFFFFFF;

    // Call once at boot: keeps the log after a restart, clears it after a
    // power loss, and adds the start entry.
    void eventLogInit();
    void eventLog(EventType type, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint32_t value = 0);
    // Holds the log until the restart, so that no entry is half written
    // when esp_restart() runs. Call right before esp_restart().
    void eventLogLockForRestart();
    // Drops all entries and logs LogCleared; the car state is kept.
    void eventLogClear();
    // Table rows for the status page, newest first.
    std::string eventLogHtml();

    // Car and charge state across restarts, for the car and charge events.
    // carClass: 0 no car, 1 connected, 2 charging, 3 charge paused, 0xFF unknown
    uint8_t eventLogCarClass();
    uint32_t eventLogChargeStartWh();
    void eventLogSetCarState(uint8_t carClass, uint32_t chargeStartWh);

    const char *resetReasonText(int reason);
#endif /* EVENT_LOG_H */
