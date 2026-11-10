#include "time_sync.h"
#include <cstdlib>
#include <mutex>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "Time";
// Germany, CET/CEST with the EU switch dates
static const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
// before this the clock was never set (2025-01-01)
static constexpr time_t VALID_TIME = 1735689600;

static std::mutex s_mutex;
static std::string s_server;
static bool s_started = false;

// lwIP keeps the server name pointer, so stop SNTP before s_server changes.
static void startLocked(const std::string &server)
{
  if (s_started) {
    esp_netif_sntp_deinit();
    s_started = false;
  }
  s_server = server;
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server.c_str());
  cfg.wait_for_sync = false;
  if (esp_netif_sntp_init(&cfg) == ESP_OK) {
    s_started = true;
    ESP_LOGI(TAG, "SNTP started, server %s", s_server.c_str());
  } else {
    ESP_LOGE(TAG, "SNTP start failed");
  }
}

void timeSyncStart(const std::string &server)
{
  setenv("TZ", TIMEZONE, 1);
  tzset();
  std::lock_guard<std::mutex> lock(s_mutex);
  startLocked(server);
}

void timeSyncSetServer(const std::string &server)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  if (s_server == server && s_started) return;
  startLocked(server);
}

bool timeIsSet()
{
  return time(nullptr) > VALID_TIME;
}

std::string formatLocalTime(time_t t)
{
  if (t <= VALID_TIME) return std::string();
  struct tm local;
  localtime_r(&t, &local);
  char text[24];
  strftime(text, sizeof(text), "%d.%m.%Y %H:%M:%S", &local);
  return text;
}

std::string timeStatusText()
{
  std::string text = timeIsSet() ? formatLocalTime(time(nullptr)) : std::string("not set yet");
  std::lock_guard<std::mutex> lock(s_mutex);
  if (!s_started) return text + " (NTP not started)";
  return text + " (NTP " + s_server + ")";
}
