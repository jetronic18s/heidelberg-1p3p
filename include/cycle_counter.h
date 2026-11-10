#ifndef CYCLE_COUNTER_H
    #define CYCLE_COUNTER_H
    #include <cstdint>
    #include <string>

    // Contactor wear counters in NVS, kept across power loss: switch-on
    // cycles per contactor and completed phase switches, counted since v1.2.0.
    // NVS spreads the writes over its pages, so one write per cycle is fine.
    void cycleCounterInit(); // after nvs_flash_init()
    void cycleCounterContactorOn(uint8_t phases);
    void cycleCounterPhaseSwitch();
    std::string cycleCounterText();
#endif /* CYCLE_COUNTER_H */
