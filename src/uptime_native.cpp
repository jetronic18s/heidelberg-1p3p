#include "uptime_native.h"
#include "esp_timer.h"

namespace uptime {

static uint32_t s_seconds = 0;
static uint32_t s_minutes = 0;
static uint32_t s_hours = 0;
static uint32_t s_days = 0;

void calculateUptime() {
    // Nothing to do here if we calculate on demand, 
    // but we'll keep it for compatibility with the header
}

uint32_t getSeconds() { return (uint32_t)((esp_timer_get_time() / 1000000ULL) % 60); }
uint32_t getMinutes() { return (uint32_t)((esp_timer_get_time() / 60000000ULL) % 60); }
uint32_t getHours() { return (uint32_t)((esp_timer_get_time() / 3600000000ULL) % 24); }
uint32_t getDays() { return (uint32_t)(esp_timer_get_time() / 86400000000ULL); }


}
