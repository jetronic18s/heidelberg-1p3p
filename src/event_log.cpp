#include "event_log.h"
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "time_sync.h"

static const char *TAG = "EventLog";

// Bump the magic when the layout changes after a release.
static constexpr uint32_t EVENT_LOG_MAGIC = 0x45564C31; // "EVL1"
static constexpr uint16_t EVENT_LOG_SIZE = 100;

struct EventEntry {
  uint32_t time;     // Unix time, 0 while the clock was not set
  uint32_t uptimeS;  // seconds since the start it belongs to
  uint16_t start;    // start number, counts up with every start
  EventType type;
  uint8_t a, b, c;
  uint8_t reserved[3];
  uint32_t value;
};

struct EventLog {
  uint32_t magic;
  uint16_t head;     // next slot to write
  uint16_t count;
  uint16_t start;
  uint8_t carClass;
  uint8_t reserved;
  uint32_t chargeStartWh;
  EventEntry entries[EVENT_LOG_SIZE];
  uint32_t crc;
};

static RTC_NOINIT_ATTR EventLog s_log;
static std::mutex s_mutex;

static uint32_t logCrc()
{
  return esp_rom_crc32_le(0, (const uint8_t *)&s_log, offsetof(EventLog, crc));
}

static uint32_t uptimeS()
{
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

// Entries of this start written before the clock was set get their time now.
static void fillTimes()
{
  if (!timeIsSet()) return;
  const uint32_t now = (uint32_t)time(nullptr);
  const uint32_t up = uptimeS();
  for (uint16_t i = 0; i < s_log.count; i++) {
    EventEntry &e = s_log.entries[(s_log.head + EVENT_LOG_SIZE - 1 - i) % EVENT_LOG_SIZE];
    if (e.start != s_log.start) break;
    if (e.time == 0) e.time = now - (up - e.uptimeS);
  }
}

void eventLogInit()
{
  const esp_reset_reason_t reason = esp_reset_reason();
  std::lock_guard<std::mutex> lock(s_mutex);
  // RTC memory is random after a power loss, and a brownout may have
  // corrupted it.
  const bool keep = reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT
    && s_log.magic == EVENT_LOG_MAGIC && s_log.crc == logCrc()
    && s_log.count <= EVENT_LOG_SIZE && s_log.head < EVENT_LOG_SIZE;
  if (!keep) {
    s_log = EventLog{};
    s_log.magic = EVENT_LOG_MAGIC;
    s_log.carClass = 0xFF;
    ESP_LOGI(TAG, "event log cleared");
  }
  s_log.start++;
  s_log.crc = logCrc();
}

void eventLog(EventType type, uint8_t a, uint8_t b, uint8_t c, uint32_t value)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  if (s_log.magic != EVENT_LOG_MAGIC) return; // before eventLogInit()
  fillTimes();
  EventEntry &e = s_log.entries[s_log.head];
  e = EventEntry{};
  e.time = timeIsSet() ? (uint32_t)time(nullptr) : 0;
  e.uptimeS = uptimeS();
  e.start = s_log.start;
  e.type = type;
  e.a = a;
  e.b = b;
  e.c = c;
  e.value = value;
  s_log.head = (s_log.head + 1) % EVENT_LOG_SIZE;
  if (s_log.count < EVENT_LOG_SIZE) s_log.count++;
  s_log.crc = logCrc();
}

void eventLogLockForRestart()
{
  // Never unlocked: the restart follows. The log mutex is a leaf lock and
  // held only for a few microseconds, so this cannot block for long.
  s_mutex.lock();
}

void eventLogClear()
{
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_log.magic != EVENT_LOG_MAGIC) return;
    s_log.head = 0;
    s_log.count = 0;
    s_log.crc = logCrc();
  }
  eventLog(EventType::LogCleared);
}

uint8_t eventLogCarClass()
{
  std::lock_guard<std::mutex> lock(s_mutex);
  return s_log.carClass;
}

uint32_t eventLogChargeStartWh()
{
  std::lock_guard<std::mutex> lock(s_mutex);
  return s_log.chargeStartWh;
}

void eventLogSetCarState(uint8_t carClass, uint32_t chargeStartWh)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  s_log.carClass = carClass;
  s_log.chargeStartWh = chargeStartWh;
  s_log.crc = logCrc();
}

const char *resetReasonText(int reason)
{
  switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software (reboot, OTA)";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

// "37.0 s", "14 min", "3.5 h"
static std::string durationText(uint32_t ms)
{
  char buf[24];
  if (ms < 60000) snprintf(buf, sizeof(buf), "%u.%u s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
  else if (ms < 3600000) snprintf(buf, sizeof(buf), "%u min", (unsigned)(ms / 60000));
  else snprintf(buf, sizeof(buf), "%u.%u h", (unsigned)(ms / 3600000), (unsigned)(ms % 3600000 / 360000));
  return buf;
}

static const char *faultText(uint32_t code)
{
  switch (code) {
    case 0xE101: return "both contactor feedback inputs active";
    case 0xE102: return "contactor did not confirm switching on";
    case 0xE103: return "invalid contactor feedback while running";
    case 0xE104: return "wallbox not responding after power-on";
    case 0xE105: return "contactor feedback still on after switching off";
    default: return "unknown";
  }
}

static std::string eventText(const EventEntry &e)
{
  char buf[96];
  switch (e.type) {
    case EventType::Start:
      return std::string("start: ") + resetReasonText(e.a);
    case EventType::RestartRequested:
      switch ((RestartSource)e.a) {
        case RestartSource::WebPage: return "restart requested on the web page";
        case RestartSource::FirmwareUpdate: return "restart after a firmware update";
        case RestartSource::WifiSettings: return "restart after new WiFi settings";
      }
      return "restart requested";
    case EventType::PhaseSwitch:
      snprintf(buf, sizeof(buf), "phase switch %up to %up (%s), took %s", e.a, e.b,
               (SwitchSource)e.c == SwitchSource::WebPage ? "web page" : "evcc",
               durationText(e.value).c_str());
      return buf;
    case EventType::CarConnected: return "car connected";
    case EventType::CarDisconnected: return "car disconnected";
    case EventType::ChargeStart: return "charging started";
    case EventType::ChargeEnd:
      if (e.value == EVENT_VALUE_UNKNOWN) return "charging stopped";
      snprintf(buf, sizeof(buf), "charging stopped, %lu.%lu kWh", (unsigned long)((e.value + 50) / 1000),
               (unsigned long)((e.value + 50) % 1000 / 100));
      return buf;
    case EventType::EvccTimeout:
      snprintf(buf, sizeof(buf), "evcc timeout, failsafe current %lu.%lu A",
               (unsigned long)(e.value / 10), (unsigned long)(e.value % 10));
      return buf;
    case EventType::EvccBack:
      return "evcc back after " + durationText(e.value);
    case EventType::WallboxSilent: return "wallbox not responding";
    case EventType::WallboxBack:
      return "wallbox answers again after " + durationText(e.value);
    case EventType::SafetyFault:
      snprintf(buf, sizeof(buf), "safety fault 0x%04lX: %s", (unsigned long)e.value, faultText(e.value));
      return buf;
    case EventType::ModbusRefused:
      if (e.value <= 1) return "Modbus TCP connection refused (client limit reached)";
      snprintf(buf, sizeof(buf), "%lu Modbus TCP connections refused (client limit reached)",
               (unsigned long)e.value);
      return buf;
    case EventType::LogCleared: return "event log cleared on the web page";
    case EventType::ChargePaused: return "charging paused, the car waits for current";
    case EventType::ChargeResumed: return "charging resumed";
  }
  return "unknown event";
}

static std::string timeText(const EventEntry &e)
{
  if (e.time) return formatLocalTime((time_t)e.time);
  char buf[40];
  snprintf(buf, sizeof(buf), "start %u + %02lu:%02lu:%02lu", e.start, (unsigned long)(e.uptimeS / 3600),
           (unsigned long)(e.uptimeS / 60 % 60), (unsigned long)(e.uptimeS % 60));
  return buf;
}

std::string eventLogHtml()
{
  // on the heap: 2.4 kB would weigh on the web server's stack
  std::vector<EventEntry> copy;
  uint16_t count;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    fillTimes();
    s_log.crc = logCrc();
    count = s_log.count;
    copy.reserve(count);
    for (uint16_t i = 0; i < count; i++) {
      copy.push_back(s_log.entries[(s_log.head + EVENT_LOG_SIZE - 1 - i) % EVENT_LOG_SIZE]);
    }
  }
  std::string html;
  if (count == 0) return "<tr><td colspan=\"2\">no events yet</td></tr>";
  for (uint16_t i = 0; i < count; i++) {
    html += "<tr><td>" + timeText(copy[i]) + "</td><td>" + eventText(copy[i]) + "</td></tr>";
  }
  return html;
}
