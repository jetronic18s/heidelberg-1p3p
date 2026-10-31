#include "switch.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <new>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "PhaseSwitch";

static uint32_t nowMs()
{
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Time since a timestamp the other task may update. Read it before the clock:
// a newer value stored in between would wrap the difference to ~49 days.
static uint32_t msSince(const std::atomic<uint32_t>& stamp)
{
  const uint32_t then = stamp;
  return nowMs() - then;
}

// Deadlines are uint32_t milliseconds and wrap after 49.7 days.
static bool pastDeadline(uint32_t deadlineMs)
{
  return (int32_t)(nowMs() - deadlineMs) > 0;
}

static constexpr uint16_t SAFETY_FAULT_NONE = 0x0000;
static constexpr uint16_t SAFETY_FAULT_FEEDBACK_BOTH_ACTIVE = 0xE101;
static constexpr uint16_t SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT = 0xE102;
static constexpr uint16_t SAFETY_FAULT_RUNNING_FEEDBACK_INVALID = 0xE103;
static constexpr uint16_t SAFETY_FAULT_WALLBOX_NOT_RESPONDING = 0xE104;
static constexpr uint16_t SAFETY_FAULT_SWITCH_OFF_TIMEOUT = 0xE105;
static constexpr uint32_t SWITCH_CONFIRM_TIMEOUT_MS = 8000;
static constexpr uint32_t SWITCH_POWER_ON_SETTLE_MS = 7000;
// The Heidelberg answers about 20 s after power-on.
static constexpr uint32_t WALLBOX_RESPONSE_TIMEOUT_MS = 60000;
static constexpr uint32_t ZERO_LOAD_TIMEOUT_MS = 15000;
static constexpr uint32_t RESTART_STOP_TIMEOUT_MS = 10000;
static constexpr uint32_t UNLOCK_CONFIRM_TIMEOUT_MS = 15000;
// A wallbox silent this long in Running is reported as not responding.
static constexpr uint32_t WALLBOX_SILENT_TIMEOUT_MS = 60000;
static constexpr uint16_t MODBUS_TCP_PORT = 502;
static constexpr uint16_t MODBUS_TCP_MAX_PDU = 253;
static constexpr uint32_t MODBUS_TCP_CLIENT_TIMEOUT_SEC = 5;
// each client gets a task with 6 KB stack
static constexpr uint32_t MAX_MODBUS_TCP_CLIENTS = 10;
static constexpr uint8_t FC_READ_HOLDING = 3;
static constexpr uint8_t FC_READ_INPUT = 4;
static constexpr uint8_t FC_WRITE_HOLDING = 6;
static constexpr uint8_t FC_WRITE_MULTIPLE = 16;
static constexpr uint8_t EX_ILLEGAL_FUNCTION = 1;
static constexpr uint8_t EX_ILLEGAL_DATA_ADDRESS = 2;
static constexpr uint8_t EX_ILLEGAL_DATA_VALUE = 3;
static constexpr uint8_t EX_SERVER_FAILURE = 4;
static constexpr uint8_t EX_GATEWAY_TARGET_FAILED = 11;

// Charging states (input register 5) reported to evcc outside Running.
static constexpr uint16_t HEC_STATUS_A1 = 2;
static constexpr uint16_t HEC_STATUS_B1 = 4;

// Register cache kept across a software restart (OTA, reboot button) in RTC
// memory, so evcc gets the last known values until the wallbox answers again.
// The contents are random after a power loss, hence the magic and the CRC.
// Bump the magic when the layout of BootCache changes after a release.
static constexpr uint32_t BOOT_CACHE_MAGIC = 0x48454331; // "HEC1"
struct BootCache {
  uint32_t magic;
  uint16_t input[20];
  uint16_t holding[10];
  uint16_t reportedStatus;
  uint8_t desiredPhases;
  // status page counters, so they count since power-on, not since a restart
  uint32_t switchCount;
  uint32_t wallboxOutages;
  uint32_t evccTimeouts;
  uint32_t crc;
};
static RTC_NOINIT_ATTR BootCache s_bootCache;

static uint32_t bootCacheCrc(const BootCache &cache)
{
  return esp_rom_crc32_le(0, (const uint8_t *)&cache, offsetof(BootCache, crc));
}

struct TcpClientContext {
  PhaseSwitch *phaseSwitch;
  int fd;
};

static void pinModeOutput(int pin)
{
  gpio_reset_pin((gpio_num_t)pin);
  // input too, so the status page can read back the relay state
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);
}

static void pinModeInput(int pin)
{
  gpio_reset_pin((gpio_num_t)pin);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
}

static void writePin(int pin, int level)
{
  gpio_set_level((gpio_num_t)pin, level);
}

static int readPin(int pin)
{
  return gpio_get_level((gpio_num_t)pin);
}

PhaseSwitch::PhaseSwitch()
  :_previous(0)
  ,_delay(0)
  ,_state(State::WaitingForOff)
  ,_desiredPhases(1)
  ,_phaseSwitchRequested(false)
  ,_currentWrittenDuringSwitch(false)
  ,_switchingSupported(false)
  ,_firmwareSupported(false)
  ,_bootCacheValid(false)
  ,_reportedStatus(HEC_STATUS_B1)
  ,_inputRegister(20, 0)
  ,_holdingRegister(10, 0)
  ,_switchDelay(120000)
  ,_master_handle(nullptr)
  ,_masterStarted(false)
  ,_tcpTaskHandle(nullptr)
  ,_serverId(1)
  ,_listenFd(-1)
  ,_tcpServerStarted(false)
  ,_rtuMessageCount(0)
  ,_rtuPendingRequestCount(0)
  ,_rtuErrorCount(0)
  ,_bridgeMessageCount(0)
  ,_bridgeActiveClientCount(0)
  ,_bridgeErrorCount(0)
  ,_safetyFaultCode(SAFETY_FAULT_NONE)
  ,_safetyFaultText("")
  ,_switchOffDeadlineMs(0)
  ,_switchOnDeadlineMs(0)
  ,_wallboxResponseDeadlineMs(0)
  ,_zeroLoadDeadlineMs(0)
  ,_unlockDeadlineMs(0)
  ,_restartRequested(false)
  ,_updateInProgress(false)
  ,_requestedPhases(0)
  ,_lastBridgeRequestMs(0)
  ,_evccTimeout(false)
  ,_lastWallboxOkMs(0)
  ,_wallboxSilent(false)
  ,_wallboxSilentSinceMs(0)
  ,_rtuWindowStartMs(0)
  ,_rtuWindowMsgs(0)
  ,_rtuWindowErrs(0)
  ,_rtuPrevMsgs(0)
  ,_rtuPrevErrs(0)
  ,_rtuPrevValid(false)
  ,_lastRtuError(ESP_OK)
  ,_lastRtuErrorMs(0)
  ,_wallboxOutages(0)
  ,_lastOutageEndMs(0)
  ,_lastOutageMs(0)
  ,_evccTimeouts(0)
  ,_hwMaxCurrent(0)
  ,_hwMinCurrent(0)
  ,_swRevision(0)
  ,_switchCount(0)
  ,_switchStartMs(0)
  ,_lastSwitchDoneMs(0)
  ,_lastSwitchMs(0)
  ,_lastSwitchFrom(0)
  ,_lastSwitchTo(0)
  ,_lastClientAddr(0)
  ,_restartDeadlineMs(0)
{}

void PhaseSwitch::begin(){
  pinModeOutput(PIN_1P_OUT);
  writePin(PIN_1P_OUT, RELAY_OFF);
  pinModeOutput(PIN_3P_OUT);
  writePin(PIN_3P_OUT, RELAY_OFF);
  pinModeInput(PIN_1P_IN);
  pinModeInput(PIN_3P_IN);
  loadBootCache();
}

void PhaseSwitch::loadBootCache(){
  const bool valid = esp_reset_reason() == ESP_RST_SW
    && s_bootCache.magic == BOOT_CACHE_MAGIC
    && s_bootCache.crc == bootCacheCrc(s_bootCache);
  if (valid) {
    {
      std::lock_guard<std::mutex> lock(_cacheMutex);
      _inputRegister.assign(s_bootCache.input, s_bootCache.input + 20);
      _holdingRegister.assign(s_bootCache.holding, s_bootCache.holding + 10);
      _reportedStatus = s_bootCache.reportedStatus;
    }
    // Boot with the phases from before the restart, as evcc expects them.
    if (s_bootCache.desiredPhases == 1 || s_bootCache.desiredPhases == 3) {
      _desiredPhases = s_bootCache.desiredPhases;
    }
    _switchCount = s_bootCache.switchCount;
    _wallboxOutages = s_bootCache.wallboxOutages;
    _evccTimeouts = s_bootCache.evccTimeouts;
    _bootCacheValid = true;
    ESP_LOGI(TAG, "restored register cache from before the restart, status %u, %up",
             s_bootCache.reportedStatus, _desiredPhases.load());
  }
  s_bootCache.magic = 0;
}

void PhaseSwitch::saveBootCache(){
  if (!_firmwareSupported && !_bootCacheValid) return;
  {
    std::lock_guard<std::mutex> lock(_cacheMutex);
    std::copy(_inputRegister.begin(), _inputRegister.end(), s_bootCache.input);
    std::copy(_holdingRegister.begin(), _holdingRegister.end(), s_bootCache.holding);
    s_bootCache.reportedStatus = _reportedStatus;
  }
  s_bootCache.desiredPhases = reportedPhases(); // a pending request wins
  s_bootCache.switchCount = _switchCount;
  s_bootCache.wallboxOutages = _wallboxOutages;
  s_bootCache.evccTimeouts = _evccTimeouts;
  s_bootCache.magic = BOOT_CACHE_MAGIC;
  s_bootCache.crc = bootCacheCrc(s_bootCache);
}

void PhaseSwitch::restartNow(){
  saveBootCache();
  esp_restart();
}

// Remember whether a car was connected before leaving Running. Outside Running
// the wallbox is locked or off and does not tell; reporting the old state
// avoids a phantom "car connected" in evcc. Errors and "locked" keep the
// previous guess.
void PhaseSwitch::rememberCarState(){
  std::lock_guard<std::mutex> lock(_cacheMutex);
  const uint16_t status = _inputRegister[5];
  if (status == 2 || status == 3) {
    _reportedStatus = HEC_STATUS_A1;
  } else if (status >= 4 && status <= 8) {
    _reportedStatus = HEC_STATUS_B1;
  }
}

// Outside Running, evcc is served from the cache: during a phase switch, a
// restart, and after a software restart until the wallbox answers. Also
// during an OTA upload: writing the flash stalls the Modbus RTU master.
bool PhaseSwitch::servesFromCache(){
  return (_firmwareSupported || _bootCacheValid)
    && (_state != State::Running || _updateInProgress);
}

void PhaseSwitch::setUpdateInProgress(bool inProgress){
  _updateInProgress = inProgress;
}

// After a power-on there is no cache yet. Until the wallbox answers, report it
// as unreachable instead of passing through errors or values of a wallbox
// that is still booting.
bool PhaseSwitch::waitingForWallbox(){
  return _switchingSupported && !_firmwareSupported && !_bootCacheValid;
}

void PhaseSwitch::beginModbus(){
    ESP_LOGI(TAG, "beginModbus: Starting Modbus stack...");
    
    // 1. Initialize Master (RTU)
    // We must disable console logging before starting Modbus on UART0
    ESP_LOGI(TAG, "Reactivating RS485 on UART0. Console logging will be disabled.");
    vTaskDelay(pdMS_TO_TICKS(100)); // Flush remaining logs
    debugDisableConsoleLog();

    mb_communication_info_t master_comm = {};
    master_comm.ser_opts.port = UART_NUM_0;
    master_comm.ser_opts.mode = MB_RTU;
    master_comm.ser_opts.baudrate = 19200;
    master_comm.ser_opts.parity = UART_PARITY_EVEN;
    master_comm.ser_opts.data_bits = UART_DATA_8_BITS;
    master_comm.ser_opts.stop_bits = UART_STOP_BITS_1;
    
    // Only raw requests are sent, but mbc_master_start() refuses to start
    // without a parameter table. esp-modbus does not zero its size, so without
    // this the start failed at random, depending on the heap contents.
    static const mb_parameter_descriptor_t unusedTable[] = {
        { .cid = 0, .param_key = "status", .param_units = "", .mb_slave_addr = 1,
          .mb_param_type = MB_PARAM_INPUT, .mb_reg_start = 5, .mb_size = 1,
          .param_offset = 0, .param_type = PARAM_TYPE_U16, .param_size = PARAM_SIZE_U16,
          .param_opts = {}, .access = PAR_PERMS_READ },
    };

    if (mbc_master_create_serial(&master_comm, &_master_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create Modbus master");
    } else {
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, PIN_RS485_DE, UART_PIN_NO_CHANGE);
        uart_set_mode(UART_NUM_0, UART_MODE_RS485_HALF_DUPLEX);
        if (mbc_master_set_descriptor(_master_handle, unusedTable, 1) != ESP_OK
            || mbc_master_start(_master_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Modbus master");
        } else {
            _masterStarted = true;
            ESP_LOGI(TAG, "Modbus RTU master started on UART0");
        }
    }
    startTcpBridge();
}

esp_err_t PhaseSwitch::masterReadInput(uint16_t addr, uint16_t count, uint16_t* dest) {
    if (!_master_handle) return ESP_ERR_INVALID_STATE;
    mb_param_request_t req = {};
    req.slave_addr = _serverId;
    req.command = 4; // Read Input Registers
    req.reg_start = addr;
    req.reg_size = count;
    _rtuPendingRequestCount++;
    esp_err_t err;
    {
        std::lock_guard<std::mutex> lock(_rtuMutex);
        err = mbc_master_send_request(_master_handle, &req, dest);
    }
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
    noteRtuResult(err);
    if (err != ESP_OK) {
        _rtuErrorCount++;
        ESP_LOGE(TAG, "masterReadInput(addr=%u, count=%u) failed: 0x%x (%s)", addr, count, err, esp_err_to_name(err));
    }
    return err;
}

esp_err_t PhaseSwitch::masterReadHolding(uint16_t addr, uint16_t count, uint16_t* dest) {
    if (!_master_handle) return ESP_ERR_INVALID_STATE;
    mb_param_request_t req = {};
    req.slave_addr = _serverId;
    req.command = 3; // Read Holding Registers
    req.reg_start = addr;
    req.reg_size = count;
    _rtuPendingRequestCount++;
    esp_err_t err;
    {
        std::lock_guard<std::mutex> lock(_rtuMutex);
        err = mbc_master_send_request(_master_handle, &req, dest);
    }
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
    noteRtuResult(err);
    if (err != ESP_OK) {
        _rtuErrorCount++;
        ESP_LOGE(TAG, "masterReadHolding(addr=%u, count=%u) failed: 0x%x (%s)", addr, count, err, esp_err_to_name(err));
    }
    return err;
}

esp_err_t PhaseSwitch::masterWriteHolding(uint16_t addr, uint16_t value) {
    if (!_master_handle) return ESP_ERR_INVALID_STATE;
    mb_param_request_t req = {};
    req.slave_addr = _serverId;
    req.command = 6; // Write Single Holding Register
    req.reg_start = addr;
    req.reg_size = 1;
    _rtuPendingRequestCount++;
    esp_err_t err;
    {
        std::lock_guard<std::mutex> lock(_rtuMutex);
        err = mbc_master_send_request(_master_handle, &req, &value);
    }
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
    noteRtuResult(err);
    if (err != ESP_OK) {
        _rtuErrorCount++;
        ESP_LOGE(TAG, "masterWriteHolding(addr=%u, val=%u) failed: 0x%x (%s)", addr, value, err, esp_err_to_name(err));
    }
    return err;
}

esp_err_t PhaseSwitch::masterWriteMultipleHolding(uint16_t addr, uint16_t count, const uint16_t* values) {
    if (!_master_handle) return ESP_ERR_INVALID_STATE;
    if (!values || count == 0) return ESP_ERR_INVALID_ARG;
    mb_param_request_t req = {};
    req.slave_addr = _serverId;
    req.command = FC_WRITE_MULTIPLE;
    req.reg_start = addr;
    req.reg_size = count;
    _rtuPendingRequestCount++;
    esp_err_t err;
    {
        std::lock_guard<std::mutex> lock(_rtuMutex);
        err = mbc_master_send_request(_master_handle, &req, const_cast<uint16_t*>(values));
    }
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
    noteRtuResult(err);
    if (err != ESP_OK) {
        _rtuErrorCount++;
        ESP_LOGE(TAG, "masterWriteMultipleHolding(addr=%u, count=%u) failed: 0x%x (%s)", addr, count, err, esp_err_to_name(err));
    }
    return err;
}

void PhaseSwitch::loop(){
  std::lock_guard<std::mutex> lock(_stateMutex);
  if (_restartRequested) {
    handleRestart();
    return;
  }

  if (!_tcpServerStarted) {
      return;
  }

  const bool oneFeedback = (readPin(PIN_1P_IN) == 1);
  const bool threeFeedback = (readPin(PIN_3P_IN) == 1);
  
  if (hasSafetyFault()) {
    return;
  }

  if (oneFeedback && threeFeedback) {
    enterSafetyFault(SAFETY_FAULT_FEEDBACK_BOTH_ACTIVE, "both contactor feedback inputs active");
    return;
  }

  if (_delay > 0){
    if (nowMs() - _previous < _delay){
      return;
    }
    _delay = 0;
  }
  
  if (_state == State::WaitingForOff){
    if (readPin(PIN_1P_IN) == 0 && readPin(PIN_3P_IN) == 0){
      ESP_LOGI(TAG, "confirmed off");
      _switchingSupported = true;
      _switchOffDeadlineMs = 0;
      _state = State::ConfirmedOff;
      _previous = nowMs();
      _delay = 2000;
      return;
    }
    // A stuck contactor: both relays are off, but its feedback stays on.
    // Also at boot, where the board starts in this state.
    if (_switchOffDeadlineMs == 0) {
      _switchOffDeadlineMs = nowMs() + SWITCH_CONFIRM_TIMEOUT_MS;
    } else if (pastDeadline(_switchOffDeadlineMs)) {
      enterSafetyFault(SAFETY_FAULT_SWITCH_OFF_TIMEOUT, "contactor feedback still on after switching off");
    }
    return;
  }
  else if (_state == State::ConfirmedOff){
    const uint32_t now = nowMs();
    if (_desiredPhases == 3){
      ESP_LOGI(TAG, "switching on 3p");
      writePin(PIN_3P_OUT, RELAY_ON);
    }
    else{
      ESP_LOGI(TAG, "switching on 1p");
      writePin(PIN_1P_OUT, RELAY_ON);
    }
    _switchOnDeadlineMs = now + SWITCH_POWER_ON_SETTLE_MS + SWITCH_CONFIRM_TIMEOUT_MS;
    _wallboxResponseDeadlineMs = now + WALLBOX_RESPONSE_TIMEOUT_MS;
    _previous = now;
    _delay = SWITCH_POWER_ON_SETTLE_MS;
    _state = State::SwitchedOn;
    return;
  }

  if (!_master_handle || !validateSetup()) {
    // The switch-on confirmation below needs a responding wallbox. Without
    // this check a wallbox that never answers after power-on keeps us waiting
    // forever with the contactor closed.
    if (_state == State::SwitchedOn && pastDeadline(_wallboxResponseDeadlineMs)) {
      enterSafetyFault(SAFETY_FAULT_WALLBOX_NOT_RESPONDING, "wallbox not responding after power-on");
      return;
    }
    _previous = nowMs();
    _delay = 5000; // Wait 5s before retrying validation
    return;
  }

  if (_state == State::Running) {
    if ((!oneFeedback && !threeFeedback) || (oneFeedback && threeFeedback)) {
      enterSafetyFault(SAFETY_FAULT_RUNNING_FEEDBACK_INVALID, "invalid contactor feedback while running");
      return;
    }

    // A phase request that came in while not Running.
    const uint8_t requested = _requestedPhases.exchange(0);
    if (requested == 1 && _desiredPhases != 1) {
      ESP_LOGI(TAG, "running the pending switch to 1p");
      startSwitch(1);
      return;
    }
    if (requested == 3 && _desiredPhases != 3) {
      ESP_LOGI(TAG, "running the pending switch to 3p");
      startSwitch(3);
      return;
    }
    
    checkEvccWatchdog();
    checkWallboxSilent();

    // Update registers every 2 seconds
    static uint32_t last_update = 0;
    if (nowMs() - last_update > 2000) {
        updateCachedRegisters();
        last_update = nowMs();
    }
    return;
  }
  else if (_state == State::SwitchPhases){
    if (masterWriteHolding(HEC_REG_MAX_CURRENT, 0) != ESP_OK
        || masterWriteHolding(HEC_REG_REMOTE_LOCK, 0) != ESP_OK) {
      _previous = nowMs();
      _delay = 500;
      return;
    }

    ESP_LOGI(TAG, "phase switch started");
    _zeroLoadDeadlineMs = nowMs() + ZERO_LOAD_TIMEOUT_MS;
    _state = State::WaitingForZero;
    return;
  }
  else if (_state == State::WaitingForZero){
    uint16_t data[4];
    const bool stopped = masterReadInput(5, 4, data) == ESP_OK
      && data[0] == 10 && data[1] == 0 && data[2] == 0 && data[3] == 0;
    if (stopped) {
      ESP_LOGI(TAG, "zero load confirmed");
      _state = State::ConfirmedZero;
      return;
    }
    if (pastDeadline(_zeroLoadDeadlineMs)) {
      ESP_LOGW(TAG, "timeout waiting for lock and zero current, writing them again");
      masterWriteHolding(HEC_REG_MAX_CURRENT, 0);
      masterWriteHolding(HEC_REG_REMOTE_LOCK, 0);
      _zeroLoadDeadlineMs = nowMs() + ZERO_LOAD_TIMEOUT_MS;
    }
    // poll the wallbox every 500 ms, not every pass
    _previous = nowMs();
    _delay = 500;
    return;
  }
  else if (_state == State::ConfirmedZero){
    writePin(PIN_1P_OUT, RELAY_OFF);
    writePin(PIN_3P_OUT, RELAY_OFF);
    _switchOffDeadlineMs = nowMs() + SWITCH_CONFIRM_TIMEOUT_MS;
    _state = State::WaitingForOff;
    ESP_LOGI(TAG, "switched off");
    return;
  }
  else if (_state == State::SwitchedOn){
    if (_desiredPhases == 3){
      if (readPin(PIN_3P_IN) != 1) {
        if (pastDeadline(_switchOnDeadlineMs)) {
          enterSafetyFault(SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT, "timeout waiting for 3P feedback input");
          return;
        }
        return;
      }
      if (getActivePhases() != 3) {
        if (pastDeadline(_switchOnDeadlineMs)) {
          enterSafetyFault(SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT, "timeout waiting for 3P confirmation");
          return;
        }
        _previous = nowMs();
        _delay = 1000;
        return;
      }
      ESP_LOGI(TAG, "confirmed 3p");
    }
    else {
      if (readPin(PIN_1P_IN) != 1) {
        if (pastDeadline(_switchOnDeadlineMs)) {
          enterSafetyFault(SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT, "timeout waiting for 1P feedback input");
          return;
        }
        return;
      }
      if (getActivePhases() != 1) {
        if (pastDeadline(_switchOnDeadlineMs)) {
          enterSafetyFault(SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT, "timeout waiting for 1P confirmation");
          return;
        }
        _previous = nowMs();
        _delay = 1000;
        return;
      }
      ESP_LOGI(TAG, "confirmed 1p");
    }
    _state = State::Delay;
    _previous = nowMs();
    _delay = _switchDelay;
    return;
  }
  else if (_state == State::Delay){
    // After boot, never enable charging on our own: evcc notices the disabled
    // charger and enables it again with its own current.
    // After a phase switch, use the current evcc wrote during the switch: it is
    // already computed for the new phase count, and evcc does not write it
    // again while its target stays the same. A current from before the switch
    // is capped to the minimum: it was meant for the old phase count.
    uint16_t restartCurrent = 0;
    {
      std::lock_guard<std::mutex> cacheLock(_cacheMutex);
      if (_phaseSwitchRequested) {
        uint16_t cached = _holdingRegister[HEC_REG_MAX_CURRENT - HOLDING_REG_OFFSET];
        if (_currentWrittenDuringSwitch) {
          restartCurrent = cached;
        } else {
          restartCurrent = cached < HEC_MIN_CURRENT ? cached : HEC_MIN_CURRENT;
        }
      }
    }
    if (masterWriteHolding(HEC_REG_MAX_CURRENT, restartCurrent) != ESP_OK) return;
    cacheHolding(HEC_REG_MAX_CURRENT, &restartCurrent, 1);
    // Release the remote lock only once the restart current is in place.
    if (masterWriteHolding(HEC_REG_REMOTE_LOCK, 1) != ESP_OK) return;

    {
      std::lock_guard<std::mutex> cacheLock(_cacheMutex);
      _phaseSwitchRequested = false;
      _currentWrittenDuringSwitch = false;
    }
    _state = State::Unlocking;
    _unlockDeadlineMs = nowMs() + UNLOCK_CONFIRM_TIMEOUT_MS;
    ESP_LOGI(TAG, "restored registers, max current %u.%u A", restartCurrent / 10, restartCurrent % 10);
    return;
  }
  else if (_state == State::Unlocking){
    // The Heidelberg reports "locked" for a few seconds after the unlock.
    // Keep serving the cache meanwhile, evcc rejects status 10.
    uint16_t status;
    if (masterReadInput(5, 1, &status) == ESP_OK && status != 10) {
      ESP_LOGI(TAG, "unlock confirmed");
      _lastWallboxOkMs = nowMs();
      finishSwitchStats();
      _state = State::Running;
      return;
    }
    if (pastDeadline(_unlockDeadlineMs)) {
      ESP_LOGW(TAG, "timeout waiting for unlock, writing it again");
      masterWriteHolding(HEC_REG_REMOTE_LOCK, 1);
      _lastWallboxOkMs = nowMs();
      finishSwitchStats();
      _state = State::Running;
      return;
    }
    _previous = nowMs();
    _delay = 500;
    return;
  }
}

void PhaseSwitch::requestRestart(){
  _restartRequested = true;
}

// Restarting drops the contactors. Stop charging first so they never open
// under load; a Heidelberg cut off while charging may not answer afterwards.
// While Restarting, Modbus writes from evcc only go to the cache.
void PhaseSwitch::handleRestart(){
  if (_state != State::Restarting) {
    // Outside Running and Unlocking the current is already 0 or the wallbox is off.
    const bool mayCharge = _state == State::Running || _state == State::Unlocking;
    if (_state == State::Running) rememberCarState();
    if (!mayCharge || !_master_handle || hasSafetyFault()) {
      ESP_LOGI(TAG, "restarting");
      restartNow();
    }
    _state = State::Restarting;
    _restartDeadlineMs = nowMs() + RESTART_STOP_TIMEOUT_MS;
    _previous = nowMs();
    _delay = 0;
    if (masterWriteHolding(HEC_REG_MAX_CURRENT, 0) != ESP_OK) {
      ESP_LOGW(TAG, "restarting, could not stop charging");
      restartNow();
    }
    // Lock the wallbox like a phase switch does: the Heidelberg keeps its
    // energy counter across the power loss only once it is locked.
    if (masterWriteHolding(HEC_REG_REMOTE_LOCK, 0) != ESP_OK) {
      ESP_LOGW(TAG, "restarting, could not lock the wallbox");
    }
    ESP_LOGI(TAG, "restart requested, waiting for lock and zero current");
    return;
  }

  if (nowMs() - _previous < 500) return;
  _previous = nowMs();

  uint16_t data[4];
  const bool stopped = masterReadInput(5, 4, data) == ESP_OK
    && data[0] == 10 && data[1] == 0 && data[2] == 0 && data[3] == 0;
  if (stopped) {
    ESP_LOGI(TAG, "lock and zero current confirmed, restarting");
    restartNow();
  }
  if (pastDeadline(_restartDeadlineMs)) {
    ESP_LOGW(TAG, "timeout waiting for lock and zero current, restarting");
    restartNow();
  }
}

// Callers hold _stateMutex.
void PhaseSwitch::startSwitch(uint8_t phases){
  if (phases == 1 ? !canSwitchTo1P() : !canSwitchTo3P()) return;
  rememberCarState();
  _lastSwitchFrom = _desiredPhases.load();
  _lastSwitchTo = phases;
  _switchStartMs = nowMs();
  _switchCount++;
  _desiredPhases = phases;
  {
    std::lock_guard<std::mutex> cacheLock(_cacheMutex);
    _phaseSwitchRequested = true;
    _currentWrittenDuringSwitch = false;
  }
  _state = State::SwitchPhases;
}

void PhaseSwitch::switchTo1P(){
  std::lock_guard<std::mutex> lock(_stateMutex);
  startSwitch(1);
}

void PhaseSwitch::switchTo3P(){
  std::lock_guard<std::mutex> lock(_stateMutex);
  startSwitch(3);
}

// Phase request from evcc (register 4). Outside Running the request is kept
// and run once the board is back in Running, so evcc's request is never
// dropped and reading register 4 returns what evcc asked for.
void PhaseSwitch::requestPhases(uint8_t phases){
  std::lock_guard<std::mutex> lock(_stateMutex);
  if (_state == State::Running && _firmwareSupported) {
    _requestedPhases = 0;
    startSwitch(phases);
    return;
  }
  _requestedPhases = phases;
  ESP_LOGI(TAG, "keeping the switch to %up until Running", phases);
}

uint8_t PhaseSwitch::reportedPhases(){
  const uint8_t requested = _requestedPhases;
  return requested ? requested : _desiredPhases.load();
}

// The ESP32 polls the wallbox every 2 s and so keeps the Heidelberg's own
// watchdog (register 257) alive even when evcc is gone. Take over that
// watchdog for evcc: without a Modbus TCP request for the register 257 time,
// apply the failsafe current (register 262), as the wallbox would. evcc sets
// its own current again when it comes back.
void PhaseSwitch::checkEvccWatchdog(){
  std::lock_guard<std::mutex> lock(_evccMutex);
  const uint32_t timeoutMs = getHoldingRegister(HEC_REG_WATCHDOG);
  // Before evcc's first request the current is 0 A from the boot anyway.
  if (timeoutMs == 0 || _evccTimeout || _lastBridgeRequestMs == 0) return;
  if (msSince(_lastBridgeRequestMs) <= timeoutMs) return;

  uint16_t failsafe = getHoldingRegister(HEC_REG_FAILSAFE_CURRENT);
  if (failsafe < HEC_MIN_CURRENT) failsafe = 0; // 1..59 means 0 A for the Heidelberg
  if (masterWriteHolding(HEC_REG_MAX_CURRENT, failsafe) != ESP_OK) return;
  cacheHolding(HEC_REG_MAX_CURRENT, &failsafe, 1);
  _evccTimeout = true;
  _evccTimeouts++;
  ESP_LOGW(TAG, "no request from evcc for %u ms, failsafe current %u.%u A",
           (unsigned)timeoutMs, failsafe / 10, failsafe % 10);
}

void PhaseSwitch::noteRtuResult(esp_err_t err){
  std::lock_guard<std::mutex> lock(_rtuStatsMutex);
  const uint32_t now = nowMs();
  // error rate per minute: the last complete minute is shown
  if (now - _rtuWindowStartMs >= 60000) {
    _rtuPrevMsgs = _rtuWindowMsgs.load();
    _rtuPrevErrs = _rtuWindowErrs.load();
    _rtuPrevValid = _rtuWindowStartMs != 0;
    _rtuWindowMsgs = 0;
    _rtuWindowErrs = 0;
    _rtuWindowStartMs = now;
  }
  _rtuWindowMsgs++;
  if (err != ESP_OK) {
    _rtuWindowErrs++;
    _lastRtuError = err;
    _lastRtuErrorMs = now;
    return;
  }
  _lastWallboxOkMs = now;
  if (_wallboxSilent.exchange(false)) {
    _lastOutageMs = now - _wallboxSilentSinceMs;
    _lastOutageEndMs = now;
    ESP_LOGI(TAG, "wallbox answers again");
  }
}

// If the wallbox stops answering in Running (RS485 cut, wallbox hung), its
// own watchdog stops charging after register 257 ms. No safety fault: report
// it, answer evcc with "gateway target failed", and carry on as soon as the
// wallbox answers again.
void PhaseSwitch::checkWallboxSilent(){
  std::lock_guard<std::mutex> lock(_rtuStatsMutex);
  if (_wallboxSilent) return;
  if (msSince(_lastWallboxOkMs) <= WALLBOX_SILENT_TIMEOUT_MS) return;
  _wallboxSilentSinceMs = _lastWallboxOkMs.load();
  _wallboxOutages++;
  _wallboxSilent = true;
  ESP_LOGW(TAG, "wallbox not responding for %u s", (unsigned)(WALLBOX_SILENT_TIMEOUT_MS / 1000));
}

bool PhaseSwitch::isWallboxSilent(){ return _wallboxSilent; }

uint32_t PhaseSwitch::wallboxSilentForMs(){
  return _wallboxSilent ? msSince(_wallboxSilentSinceMs) : 0;
}

// "1.2 s", "14 min", "3.5 h"
static std::string formatDuration(uint32_t ms)
{
  char buf[24];
  if (ms < 60000) snprintf(buf, sizeof(buf), "%u.%u s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
  else if (ms < 3600000) snprintf(buf, sizeof(buf), "%u min", (unsigned)(ms / 60000));
  else snprintf(buf, sizeof(buf), "%u.%u h", (unsigned)(ms / 3600000), (unsigned)(ms % 3600000 / 360000));
  return buf;
}

static const char *rtuErrorName(esp_err_t err)
{
  switch (err) {
    case ESP_ERR_TIMEOUT: return "timeout";
    case ESP_ERR_INVALID_RESPONSE: return "invalid response";
    case ESP_ERR_INVALID_CRC: return "CRC error";
    default: return esp_err_to_name(err);
  }
}

std::string PhaseSwitch::wallboxStatusText(){
  if (!_masterStarted) {
    return _master_handle ? "RS485 master not started" : "RS485 not started yet";
  }
  if (_wallboxSilent) {
    return "not responding for " + formatDuration(msSince(_wallboxSilentSinceMs))
      + " (" + rtuErrorName(_lastRtuError) + ")";
  }
  if (_lastWallboxOkMs == 0) return "not detected yet";
  std::string text = "ok, last answer " + formatDuration(msSince(_lastWallboxOkMs)) + " ago";
  const bool prev = _rtuPrevValid;
  const uint32_t msgs = prev ? _rtuPrevMsgs : _rtuWindowMsgs;
  const uint32_t errs = prev ? _rtuPrevErrs : _rtuWindowErrs;
  text += ", " + std::to_string(errs) + "/" + std::to_string(msgs) + " errors ("
    + (prev ? "last minute" : "since " + formatDuration(msSince(_rtuWindowStartMs))) + ")";
  return text;
}

std::string PhaseSwitch::wallboxHistoryText(){
  std::string text;
  const uint32_t outages = _wallboxOutages;
  if (outages == 0) {
    text = "no outages since power-on";
  } else {
    text = std::to_string(outages) + "x not responding since power-on";
    if (_lastOutageEndMs) {
      text += ", last " + formatDuration(msSince(_lastOutageEndMs)) + " ago for "
        + formatDuration(_lastOutageMs);
    }
  }
  if (_lastRtuErrorMs) {
    text += "; last error: " + std::string(rtuErrorName(_lastRtuError)) + " "
      + formatDuration(msSince(_lastRtuErrorMs)) + " ago";
  }
  return text;
}

std::string PhaseSwitch::evccStatusText(){
  const uint32_t last = _lastBridgeRequestMs;
  if (last == 0) return "no request yet";
  std::string text = "last request " + formatDuration(msSince(_lastBridgeRequestMs)) + " ago, "
    + std::to_string(_evccTimeouts) + " timeouts since power-on";
  if (_evccTimeout) text += ", failsafe current active";
  return text;
}

void PhaseSwitch::finishSwitchStats(){
  const uint32_t start = _switchStartMs;
  if (start == 0) return;
  const uint32_t done = nowMs();
  _lastSwitchMs = done - start;
  _lastSwitchDoneMs = done;
  _switchStartMs = 0;
}

std::string PhaseSwitch::switchStatusText(){
  std::string text = std::to_string(_switchCount) + " since power-on";
  const uint8_t to = _lastSwitchTo;
  if (to) {
    text += ", last " + std::to_string(_lastSwitchFrom) + "p to " + std::to_string(to) + "p ";
    const uint32_t start = _switchStartMs;
    if (start) {
      text += "running for " + formatDuration(nowMs() - start);
    } else {
      text += formatDuration(msSince(_lastSwitchDoneMs)) + " ago, took " + formatDuration(_lastSwitchMs);
    }
  }
  const uint8_t requested = _requestedPhases;
  if (requested) text += ", pending: " + std::to_string(requested) + "p";
  return text;
}

std::string PhaseSwitch::contactorText(){
  auto onOff = [](int level, int on) { return level == on ? "on" : "off"; };
  return std::string("1p relay ") + onOff(readPin(PIN_1P_OUT), RELAY_ON)
    + " / feedback " + onOff(readPin(PIN_1P_IN), 1)
    + ", 3p relay " + onOff(readPin(PIN_3P_OUT), RELAY_ON)
    + " / feedback " + onOff(readPin(PIN_3P_IN), 1);
}

std::string PhaseSwitch::evccClientText(){
  const uint32_t addr = _lastClientAddr;
  if (addr == 0) return "none yet";
  char buf[48];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u, %u connected",
           (unsigned)(addr & 0xFF), (unsigned)(addr >> 8 & 0xFF),
           (unsigned)(addr >> 16 & 0xFF), (unsigned)(addr >> 24),
           (unsigned)_bridgeActiveClientCount);
  return buf;
}

bool PhaseSwitch::bootCacheRestored(){ return _bootCacheValid; }

std::string PhaseSwitch::wallboxInfoText(){
  if (_hwMaxCurrent == 0) return "unknown";
  std::string text = "hardware current " + std::to_string(_hwMinCurrent) + "-"
    + std::to_string(_hwMaxCurrent) + " A";
  if (_swRevision) text += ", software revision " + std::to_string(_swRevision);
  return text;
}

bool PhaseSwitch::canSwitchTo1P(){
  return _switchingSupported
    && _firmwareSupported
    && _state == State::Running
    && _desiredPhases == 3;
}

bool PhaseSwitch::canSwitchTo3P(){
  return _switchingSupported
    && _firmwareSupported
    && _state == State::Running
    && _desiredPhases == 1;
}

void PhaseSwitch::setSwitchDelay(uint32_t delayMs){
  _switchDelay = delayMs;
}

uint32_t PhaseSwitch::getRtuMessageCount(){ return _rtuMessageCount; }
uint32_t PhaseSwitch::getRtuPendingRequestCount(){ return _rtuPendingRequestCount; }
uint32_t PhaseSwitch::getRtuErrorCount(){ return _rtuErrorCount; }
uint32_t PhaseSwitch::getBridgeMessageCount(){ return _bridgeMessageCount; }
uint32_t PhaseSwitch::getBridgeActiveClientCount(){ return _bridgeActiveClientCount; }
uint32_t PhaseSwitch::getBridgeErrorCount(){ return _bridgeErrorCount; }

void PhaseSwitch::enterSafetyFault(uint16_t code, const char *text){
  if (_safetyFaultCode != SAFETY_FAULT_NONE) return;

  _safetyFaultText = text ? text : "unspecified safety fault";
  _safetyFaultCode = code;
  _delay = 0;
  _switchOnDeadlineMs = 0;
  writePin(PIN_1P_OUT, RELAY_OFF);
  writePin(PIN_3P_OUT, RELAY_OFF);
  _state = State::Fault;

  ESP_LOGE(TAG, "SAFETY FAULT 0x%04X: %s", code, _safetyFaultText.load());
}

bool PhaseSwitch::hasSafetyFault(){
  return _safetyFaultCode != SAFETY_FAULT_NONE;
}

std::string PhaseSwitch::getState(){
  std::string result;
  if (readPin(PIN_1P_IN) == 1) result = "1P ";
  else if (readPin(PIN_3P_IN) == 1) result = "3P ";
  else result = "~P ";

  switch(_state){
    case State::Running: result += "Running"; break;
    case State::SwitchPhases: result += "SwitchPhases"; break;
    case State::WaitingForZero: result += "WaitingForZero"; break;
    case State::ConfirmedZero: result += "ConfirmedZero"; break;
    case State::WaitingForOff: result += "WaitingForOff"; break;
    case State::ConfirmedOff: result += "ConfirmedOff"; break;
    case State::SwitchedOn: result += "SwitchedOn"; break;
    case State::Delay: result += "Delay"; break;
    case State::Fault: result += "Fault"; break;
    case State::Unlocking: result += "Unlocking"; break;
    case State::Restarting: result += "Restarting"; break;
    default: result += "undefined"; break;
  }
  if (_safetyFaultCode != SAFETY_FAULT_NONE){
    char hex[16];
    snprintf(hex, sizeof(hex), " [0x%04X]", _safetyFaultCode.load());
    result += hex;
  }
  if (_wallboxSilent){
    result += " [wallbox not responding]";
  }
  if (_evccTimeout){
    result += " [evcc timeout]";
  }
  if (_delay > 0){
    auto passed = nowMs() - _previous;
    auto remaining = (passed < _delay) ? (_delay - passed) : 0;
    result += " (delayed for " + std::to_string(remaining) + "ms)";
  }
  return result;
}

uint16_t PhaseSwitch::getSafetyFaultCode(){ return _safetyFaultCode; }
std::string PhaseSwitch::getSafetyFaultText(){
  return (_safetyFaultCode == SAFETY_FAULT_NONE) ? "none" : _safetyFaultText.load();
}

uint16_t PhaseSwitch::getHoldingRegister(size_t reg){
  std::lock_guard<std::mutex> lock(_cacheMutex);
  if (reg < HOLDING_REG_OFFSET) return 0xffff;
  size_t idx = reg - HOLDING_REG_OFFSET;
  if (idx >= _holdingRegister.size()) return 0xffff;
  return _holdingRegister[idx];
}

uint16_t PhaseSwitch::getInputRegister(size_t reg){
  std::lock_guard<std::mutex> lock(_cacheMutex);
  if (reg >= _inputRegister.size()) return 0xffff;
  return _inputRegister[reg];
}

bool PhaseSwitch::updateCachedRegisters(){
  if (!_master_handle) return false;
  uint16_t input[15];
  esp_err_t err = masterReadInput(4, 15, input);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "masterReadInput failed: 0x%x (%s)", err, esp_err_to_name(err));
      return false;
  }
  cacheInput(4, input, 15);
  uint16_t holding[6];
  err = masterReadHolding(257, 6, holding);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "masterReadHolding failed: 0x%x (%s)", err, esp_err_to_name(err));
      return false;
  }
  cacheHolding(257, holding, 6);
  return true;
}

bool PhaseSwitch::validateSetup(){
  if (_switchingSupported && _firmwareSupported) return true;
  if (!_master_handle) return false;
  if (_switchingSupported){
    uint16_t layout;
    if (masterReadInput(4, 1, &layout) == ESP_OK){
      if (layout == 0x108){
        if (updateCachedRegisters()) {
          _firmwareSupported = true;
          // A wallbox that was not locked before losing power reports the
          // real car state right after power-on.
          rememberCarState();
          // Static wallbox data for the status page; optional registers.
          uint16_t hw[2];
          if (masterReadInput(100, 2, hw) == ESP_OK) {
            _hwMaxCurrent = hw[0];
            _hwMinCurrent = hw[1];
          }
          uint16_t revision;
          if (masterReadInput(203, 1, &revision) == ESP_OK) _swRevision = revision;
        }
      }
    }
  }
  return _switchingSupported && _firmwareSupported;
}

uint8_t PhaseSwitch::getActivePhases(){
  if (!_master_handle) return 0;
  uint16_t l[3];
  if (masterReadInput(10, 3, l) == ESP_OK){
    if (l[0] >= 208 && l[1] < 208 && l[2] < 208) return 1;
    if (l[0] >= 208 && l[1] >= 208 && l[2] >= 208) return 3;
  }
  return 0;
}

static uint16_t readU16be(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void appendU16be(std::vector<uint8_t> &out, uint16_t value)
{
  out.push_back((uint8_t)(value >> 8));
  out.push_back((uint8_t)(value & 0xff));
}

static bool recvExact(int fd, uint8_t *buf, size_t len)
{
  size_t done = 0;
  while (done < len) {
    int rc = recv(fd, buf + done, len - done, 0);
    if (rc == 0) return false;
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += (size_t)rc;
  }
  return true;
}

void PhaseSwitch::startTcpBridge()
{
  if (_tcpServerStarted) {
    return;
  }
  BaseType_t ok = xTaskCreate(
      [](void *arg) { static_cast<PhaseSwitch *>(arg)->tcpBridgeTask(); },
      "modbus_tcp_bridge",
      6144,
      this,
      8,
      &_tcpTaskHandle);
  if (ok != pdPASS) {
    _bridgeErrorCount++;
    ESP_LOGE(TAG, "failed to create Modbus TCP bridge task");
    return;
  }
  _tcpServerStarted = true;
}

void PhaseSwitch::tcpBridgeTask()
{
  _listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (_listenFd < 0) {
    _bridgeErrorCount++;
    ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
    vTaskDelete(nullptr);
    return;
  }

  int yes = 1;
  setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(MODBUS_TCP_PORT);

  if (bind(_listenFd, (sockaddr *)&addr, sizeof(addr)) != 0) {
    _bridgeErrorCount++;
    ESP_LOGE(TAG, "bind(%u) failed: errno=%d", MODBUS_TCP_PORT, errno);
    close(_listenFd);
    _listenFd = -1;
    vTaskDelete(nullptr);
    return;
  }

  if (listen(_listenFd, 5) != 0) {
    _bridgeErrorCount++;
    ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
    close(_listenFd);
    _listenFd = -1;
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "Modbus TCP bridge listening on port %u", MODBUS_TCP_PORT);

  while (true) {
    sockaddr_in remote = {};
    socklen_t remoteLen = sizeof(remote);
    int clientFd = accept(_listenFd, (sockaddr *)&remote, &remoteLen);
    if (clientFd < 0) {
      if (errno != EINTR) {
        _bridgeErrorCount++;
      }
      continue;
    }
    if (_bridgeActiveClientCount >= MAX_MODBUS_TCP_CLIENTS) {
      _bridgeErrorCount++;
      close(clientFd);
      continue;
    }
    _bridgeActiveClientCount++;
    _lastClientAddr = remote.sin_addr.s_addr;
    auto *ctx = new (std::nothrow) TcpClientContext{this, clientFd};
    if (!ctx) {
      _bridgeActiveClientCount--;
      _bridgeErrorCount++;
      close(clientFd);
      continue;
    }
    BaseType_t ok = xTaskCreate(
        [](void *arg) {
          auto *ctx = static_cast<TcpClientContext *>(arg);
          ctx->phaseSwitch->handleTcpClient(ctx->fd);
          close(ctx->fd);
          ctx->phaseSwitch->_bridgeActiveClientCount--;
          delete ctx;
          vTaskDelete(nullptr);
        },
        "modbus_tcp_client",
        6144,
        ctx,
        7,
        nullptr);
    if (ok != pdPASS) {
      _bridgeActiveClientCount--;
      _bridgeErrorCount++;
      close(clientFd);
      delete ctx;
    }
  }
}

void PhaseSwitch::handleTcpClient(int clientFd)
{
  timeval tv = {};
  tv.tv_sec = MODBUS_TCP_CLIENT_TIMEOUT_SEC;
  setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  while (true) {
    uint8_t header[7];
    if (!recvExact(clientFd, header, sizeof(header))) {
      return;
    }

    const uint16_t txId = readU16be(&header[0]);
    const uint16_t protoId = readU16be(&header[2]);
    const uint16_t length = readU16be(&header[4]);
    const uint8_t unitId = header[6];

    if (protoId != 0 || length < 2 || length > (MODBUS_TCP_MAX_PDU + 1)) {
      _bridgeErrorCount++;
      return;
    }

    std::vector<uint8_t> pdu(length - 1);
    if (!recvExact(clientFd, pdu.data(), pdu.size())) {
      return;
    }

    std::vector<uint8_t> responsePdu;
    if (!handleModbusRequest(unitId, pdu.data(), pdu.size(), responsePdu)) {
      _bridgeErrorCount++;
      return;
    }

    std::vector<uint8_t> out;
    out.reserve(7 + responsePdu.size());
    appendU16be(out, txId);
    appendU16be(out, 0);
    appendU16be(out, (uint16_t)(responsePdu.size() + 1));
    out.push_back(unitId);
    out.insert(out.end(), responsePdu.begin(), responsePdu.end());

    if (send(clientFd, out.data(), out.size(), 0) != (int)out.size()) {
      _bridgeErrorCount++;
      return;
    }
    _bridgeMessageCount++;
  }
}

bool PhaseSwitch::handleModbusRequest(uint8_t unitId, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& responsePdu)
{
  if (!pdu || pduLen < 1) {
    return false;
  }

  const uint8_t fc = pdu[0];
  if (unitId != _serverId) {
    buildException(fc, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(_evccMutex);
    _lastBridgeRequestMs = nowMs();
    if (_evccTimeout) {
      _evccTimeout = false;
      ESP_LOGI(TAG, "evcc is back");
    }
  }

  switch (fc) {
    case FC_READ_HOLDING:
    case FC_READ_INPUT: {
      if (pduLen != 5) {
        buildException(fc, EX_ILLEGAL_DATA_VALUE, responsePdu);
        return true;
      }
      const uint16_t addr = readU16be(&pdu[1]);
      const uint16_t count = readU16be(&pdu[3]);
      if (count == 0 || count > 125) {
        buildException(fc, EX_ILLEGAL_DATA_VALUE, responsePdu);
        return true;
      }
      return (fc == FC_READ_HOLDING)
        ? handleReadHolding(unitId, addr, count, responsePdu)
        : handleReadInput(unitId, addr, count, responsePdu);
    }
    case FC_WRITE_HOLDING: {
      if (pduLen != 5) {
        buildException(fc, EX_ILLEGAL_DATA_VALUE, responsePdu);
        return true;
      }
      return handleWriteHolding(unitId, readU16be(&pdu[1]), readU16be(&pdu[3]), responsePdu);
    }
    case FC_WRITE_MULTIPLE: {
      if (pduLen < 6) {
        buildException(fc, EX_ILLEGAL_DATA_VALUE, responsePdu);
        return true;
      }
      const uint16_t addr = readU16be(&pdu[1]);
      const uint16_t count = readU16be(&pdu[3]);
      const uint8_t byteCount = pdu[5];
      if (count == 0 || count > 123 || byteCount != count * 2 || pduLen != (size_t)(6 + byteCount)) {
        buildException(fc, EX_ILLEGAL_DATA_VALUE, responsePdu);
        return true;
      }
      std::vector<uint16_t> values(count);
      for (uint16_t i = 0; i < count; i++) {
        values[i] = readU16be(&pdu[6 + i * 2]);
      }
      return handleWriteMultiple(unitId, addr, count, values.data(), responsePdu);
    }
    default:
      buildException(fc, EX_ILLEGAL_FUNCTION, responsePdu);
      return true;
  }
}

bool PhaseSwitch::handleReadHolding(uint8_t, uint16_t addr, uint16_t count, std::vector<uint8_t>& responsePdu)
{
  if (addr == 4 && count == 1) {
    uint16_t phases = reportedPhases();
    buildReadResponse(FC_READ_HOLDING, &phases, 1, responsePdu);
    return true;
  }

  if (waitingForWallbox()) {
    buildException(FC_READ_HOLDING, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }
  if (servesFromCache()) {
    std::vector<uint16_t> values;
    if (!readCachedHolding(addr, count, values)) {
      buildException(FC_READ_HOLDING, EX_ILLEGAL_DATA_ADDRESS, responsePdu);
      return true;
    }
    buildReadResponse(FC_READ_HOLDING, values.data(), count, responsePdu);
    return true;
  }

  if (_wallboxSilent) {
    buildException(FC_READ_HOLDING, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }
  std::vector<uint16_t> values(count);
  if (masterReadHolding(addr, count, values.data()) != ESP_OK) {
    buildException(FC_READ_HOLDING, EX_SERVER_FAILURE, responsePdu);
    return true;
  }
  cacheHolding(addr, values.data(), count);
  buildReadResponse(FC_READ_HOLDING, values.data(), count, responsePdu);
  return true;
}

bool PhaseSwitch::handleReadInput(uint8_t, uint16_t addr, uint16_t count, std::vector<uint8_t>& responsePdu)
{
  if (waitingForWallbox()) {
    buildException(FC_READ_INPUT, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }
  if (servesFromCache()) {
    std::vector<uint16_t> values;
    if (!readCachedInput(addr, count, values)) {
      buildException(FC_READ_INPUT, EX_ILLEGAL_DATA_ADDRESS, responsePdu);
      return true;
    }
    buildReadResponse(FC_READ_INPUT, values.data(), count, responsePdu);
    return true;
  }

  if (_wallboxSilent) {
    buildException(FC_READ_INPUT, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }
  std::vector<uint16_t> values(count);
  if (masterReadInput(addr, count, values.data()) != ESP_OK) {
    buildException(FC_READ_INPUT, EX_SERVER_FAILURE, responsePdu);
    return true;
  }
  cacheInput(addr, values.data(), count);
  buildReadResponse(FC_READ_INPUT, values.data(), count, responsePdu);
  return true;
}

bool PhaseSwitch::handleWriteHolding(uint8_t, uint16_t addr, uint16_t value, std::vector<uint8_t>& responsePdu)
{
  if (_switchingSupported && addr == 4) {
    if (value == 1 || value == 3) {
      requestPhases((uint8_t)value);
    } else {
      buildException(FC_WRITE_HOLDING, EX_ILLEGAL_DATA_VALUE, responsePdu);
      return true;
    }
    responsePdu = {FC_WRITE_HOLDING};
    appendU16be(responsePdu, addr);
    appendU16be(responsePdu, value);
    return true;
  }

  if (waitingForWallbox()) {
    buildException(FC_WRITE_HOLDING, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }
  // While Unlocking the wallbox answers again; only reads still come from the cache.
  if (servesFromCache() && _state != State::Unlocking) {
    std::lock_guard<std::mutex> lock(_cacheMutex);
    if (addr < HOLDING_REG_OFFSET || (addr - HOLDING_REG_OFFSET) >= _holdingRegister.size()) {
      buildException(FC_WRITE_HOLDING, EX_ILLEGAL_DATA_ADDRESS, responsePdu);
      return true;
    }
    _holdingRegister[addr - HOLDING_REG_OFFSET] = value;
    if (_phaseSwitchRequested && addr == HEC_REG_MAX_CURRENT) {
      _currentWrittenDuringSwitch = true;
    }
  } else if (_wallboxSilent) {
    buildException(FC_WRITE_HOLDING, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  } else {
    if (masterWriteHolding(addr, value) != ESP_OK) {
      buildException(FC_WRITE_HOLDING, EX_SERVER_FAILURE, responsePdu);
      return true;
    }
    cacheHolding(addr, &value, 1);
  }

  responsePdu = {FC_WRITE_HOLDING};
  appendU16be(responsePdu, addr);
  appendU16be(responsePdu, value);
  return true;
}

bool PhaseSwitch::handleWriteMultiple(uint8_t, uint16_t addr, uint16_t count, const uint16_t* values, std::vector<uint8_t>& responsePdu)
{
  if (waitingForWallbox()) {
    buildException(FC_WRITE_MULTIPLE, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  }
  // While Unlocking the wallbox answers again; only reads still come from the cache.
  if (servesFromCache() && _state != State::Unlocking) {
    std::lock_guard<std::mutex> lock(_cacheMutex);
    if (addr < HOLDING_REG_OFFSET || (addr - HOLDING_REG_OFFSET + count) > _holdingRegister.size()) {
      buildException(FC_WRITE_MULTIPLE, EX_ILLEGAL_DATA_ADDRESS, responsePdu);
      return true;
    }
    size_t idx = addr - HOLDING_REG_OFFSET;
    for (uint16_t i = 0; i < count; i++) {
      _holdingRegister[idx + i] = values[i];
    }
    if (_phaseSwitchRequested && addr <= HEC_REG_MAX_CURRENT && HEC_REG_MAX_CURRENT < addr + count) {
      _currentWrittenDuringSwitch = true;
    }
  } else if (_wallboxSilent) {
    buildException(FC_WRITE_MULTIPLE, EX_GATEWAY_TARGET_FAILED, responsePdu);
    return true;
  } else {
    if (masterWriteMultipleHolding(addr, count, values) != ESP_OK) {
      buildException(FC_WRITE_MULTIPLE, EX_SERVER_FAILURE, responsePdu);
      return true;
    }
    cacheHolding(addr, values, count);
  }

  responsePdu = {FC_WRITE_MULTIPLE};
  appendU16be(responsePdu, addr);
  appendU16be(responsePdu, count);
  return true;
}

void PhaseSwitch::buildException(uint8_t functionCode, uint8_t exceptionCode, std::vector<uint8_t>& responsePdu)
{
  responsePdu.clear();
  responsePdu.push_back((uint8_t)(functionCode | 0x80));
  responsePdu.push_back(exceptionCode);
}

void PhaseSwitch::buildReadResponse(uint8_t functionCode, const uint16_t* values, uint16_t count, std::vector<uint8_t>& responsePdu)
{
  responsePdu.clear();
  responsePdu.reserve(2 + count * 2);
  responsePdu.push_back(functionCode);
  responsePdu.push_back((uint8_t)(count * 2));
  for (uint16_t i = 0; i < count; i++) {
    appendU16be(responsePdu, values[i]);
  }
}

void PhaseSwitch::cacheHolding(uint16_t addr, const uint16_t* values, uint16_t count)
{
  if (!values || addr < HOLDING_REG_OFFSET) return;
  std::lock_guard<std::mutex> lock(_cacheMutex);
  size_t idx = addr - HOLDING_REG_OFFSET;
  if (idx + count > _holdingRegister.size()) return;
  for (uint16_t i = 0; i < count; i++) {
    _holdingRegister[idx + i] = values[i];
  }
}

void PhaseSwitch::cacheInput(uint16_t addr, const uint16_t* values, uint16_t count)
{
  if (!values) return;
  std::lock_guard<std::mutex> lock(_cacheMutex);
  if ((size_t)addr + count > _inputRegister.size()) return;
  for (uint16_t i = 0; i < count; i++) {
    _inputRegister[addr + i] = values[i];
  }
}

bool PhaseSwitch::readCachedHolding(uint16_t addr, uint16_t count, std::vector<uint16_t>& values)
{
  if (addr < HOLDING_REG_OFFSET) return false;
  std::lock_guard<std::mutex> lock(_cacheMutex);
  size_t idx = addr - HOLDING_REG_OFFSET;
  if (idx + count > _holdingRegister.size()) return false;

  values.resize(count);
  if ((HEC_REG_REMOTE_LOCK - HOLDING_REG_OFFSET) < _holdingRegister.size()) {
    _holdingRegister[HEC_REG_REMOTE_LOCK - HOLDING_REG_OFFSET] = 1;
  }
  for (uint16_t i = 0; i < count; i++) {
    values[i] = _holdingRegister[idx + i];
  }
  return true;
}

bool PhaseSwitch::readCachedInput(uint16_t addr, uint16_t count, std::vector<uint16_t>& values)
{
  std::lock_guard<std::mutex> lock(_cacheMutex);
  if (addr < 4 || (size_t)addr + count > _inputRegister.size()) return false;

  values.resize(count);
  // During an OTA upload in Running the car still charges: report the last
  // values as they are.
  if (_state == State::Running) {
    for (uint16_t i = 0; i < count; i++) {
      values[i] = _inputRegister[addr + i];
    }
    return true;
  }
  for (uint16_t i = 0; i < count; i++) {
    uint16_t reg = addr + i;
    switch (reg) {
      case 5:
        values[i] = _reportedStatus; // A1 or B1: not charging, car as before the interruption.
        break;
      case 6: case 7: case 8:    // L1-L3 current
      case 10: case 11: case 12: // L1-L3 voltage
      case 14:                   // power
        values[i] = 0; // cached values predate the interruption; the wallbox is locked or off now.
        break;
      default:
        values[i] = _inputRegister[reg];
        break;
    }
  }
  return true;
}
