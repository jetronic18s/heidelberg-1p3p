#ifndef PAGES_H
    #define PAGES_H

    #include <esp_http_server.h>
    #include <string>
    #include "uptime_native.h"
    #include "config.h"
    #include "switch.h"
    #include "debug.h"

    void setupPages(PhaseSwitch *phaseSwitch, Config *config);
    std::string ErrorName(uint8_t code);
    std::string WiFiQuality(int rssiValue);
    std::string ChargingState(uint16_t state);
    std::string Uptime();
#endif /* PAGES_H */
