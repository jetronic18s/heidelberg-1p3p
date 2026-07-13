#ifndef UPTIME_NATIVE_H
#define UPTIME_NATIVE_H

#include <cstdint>

namespace uptime {
    void calculateUptime();
    uint32_t getSeconds();
    uint32_t getMinutes();
    uint32_t getHours();
    uint32_t getDays();
}

#endif // UPTIME_NATIVE_H
