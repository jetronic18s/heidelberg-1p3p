#include "cycle_counter.h"
#include <atomic>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "Cycles";
static constexpr const char *NVS_NAMESPACE = "hec_cycles";

static nvs_handle_t s_handle = 0;
static std::atomic<uint32_t> s_on1p{0}, s_on3p{0}, s_switches{0};

static void store(const char *key, uint32_t value)
{
  if (!s_handle) return;
  if (nvs_set_u32(s_handle, key, value) != ESP_OK || nvs_commit(s_handle) != ESP_OK) {
    ESP_LOGW(TAG, "could not store %s", key);
  }
}

static uint32_t load(const char *key)
{
  uint32_t value = 0;
  nvs_get_u32(s_handle, key, &value);
  return value;
}

void cycleCounterInit()
{
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_handle) != ESP_OK) {
    s_handle = 0;
    ESP_LOGE(TAG, "NVS not available, counters not kept");
    return;
  }
  s_on1p = load("on1p");
  s_on3p = load("on3p");
  s_switches = load("switches");
}

void cycleCounterContactorOn(uint8_t phases)
{
  if (phases == 3) store("on3p", ++s_on3p);
  else store("on1p", ++s_on1p);
}

void cycleCounterPhaseSwitch()
{
  store("switches", ++s_switches);
}

std::string cycleCounterText()
{
  return "1p " + std::to_string(s_on1p.load()) + "x on, 3p " + std::to_string(s_on3p.load())
    + "x on, " + std::to_string(s_switches.load()) + " phase switches (since v1.2.0)";
}
