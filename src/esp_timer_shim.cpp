#include <stdint.h>

#if __has_include("esp_idf_version.h")
#include "esp_idf_version.h"
#endif

// ESP-IDF 5.1.x doesn't export esp_timer_impl_update_apb_freq, but Arduino core
// expects it. Provide a no-op shim to satisfy the linker when building against
// IDF < 5.2.
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
extern "C" void esp_timer_impl_update_apb_freq(uint32_t apb_ticks_per_us) {
  (void)apb_ticks_per_us;
}
#endif
