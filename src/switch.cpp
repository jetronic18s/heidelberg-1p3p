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

static constexpr uint16_t SAFETY_FAULT_NONE = 0x0000;
static constexpr uint16_t SAFETY_FAULT_FEEDBACK_BOTH_ACTIVE = 0xE101;
static constexpr uint16_t SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT = 0xE102;
static constexpr uint16_t SAFETY_FAULT_RUNNING_FEEDBACK_INVALID = 0xE103;
static constexpr uint16_t SAFETY_FAULT_WALLBOX_NOT_RESPONDING = 0xE104;
static constexpr uint32_t SWITCH_CONFIRM_TIMEOUT_MS = 8000;
static constexpr uint32_t SWITCH_POWER_ON_SETTLE_MS = 7000;
// The Heidelberg answers about 20 s after power-on.
static constexpr uint32_t WALLBOX_RESPONSE_TIMEOUT_MS = 60000;
static constexpr uint32_t RESTART_STOP_TIMEOUT_MS = 10000;
static constexpr uint32_t UNLOCK_CONFIRM_TIMEOUT_MS = 15000;
static constexpr uint16_t MODBUS_TCP_PORT = 502;
static constexpr uint16_t MODBUS_TCP_MAX_PDU = 253;
static constexpr uint32_t MODBUS_TCP_CLIENT_TIMEOUT_SEC = 5;
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
static constexpr uint32_t BOOT_CACHE_MAGIC = 0x48454332; // "HEC2"
struct BootCache {
  uint32_t magic;
  uint16_t input[20];
  uint16_t holding[10];
  uint16_t reportedStatus;
  uint8_t desiredPhases;
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
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
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
  ,_switchOnDeadlineMs(0)
  ,_wallboxResponseDeadlineMs(0)
  ,_unlockDeadlineMs(0)
  ,_restartRequested(false)
  ,_updateInProgress(false)
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
    _inputRegister.assign(s_bootCache.input, s_bootCache.input + 20);
    _holdingRegister.assign(s_bootCache.holding, s_bootCache.holding + 10);
    _reportedStatus = s_bootCache.reportedStatus;
    // Boot with the phases from before the restart, as evcc expects them.
    if (s_bootCache.desiredPhases == 1 || s_bootCache.desiredPhases == 3) {
      _desiredPhases = s_bootCache.desiredPhases;
    }
    _bootCacheValid = true;
    ESP_LOGI(TAG, "restored register cache from before the restart, status %u, %up",
             _reportedStatus, _desiredPhases);
  }
  s_bootCache.magic = 0;
}

void PhaseSwitch::saveBootCache(){
  if (!_firmwareSupported && !_bootCacheValid) return;
  std::copy(_inputRegister.begin(), _inputRegister.end(), s_bootCache.input);
  std::copy(_holdingRegister.begin(), _holdingRegister.end(), s_bootCache.holding);
  s_bootCache.reportedStatus = _reportedStatus;
  s_bootCache.desiredPhases = _desiredPhases;
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
    
    if (mbc_master_create_serial(&master_comm, &_master_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create Modbus master");
    } else {
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, PIN_RS485_DE, UART_PIN_NO_CHANGE);
        uart_set_mode(UART_NUM_0, UART_MODE_RS485_HALF_DUPLEX);
        if (mbc_master_start(_master_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Modbus master");
        } else {
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
    esp_err_t err = mbc_master_send_request(_master_handle, &req, dest);
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
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
    esp_err_t err = mbc_master_send_request(_master_handle, &req, dest);
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
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
    esp_err_t err = mbc_master_send_request(_master_handle, &req, &value);
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
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
    esp_err_t err = mbc_master_send_request(_master_handle, &req, const_cast<uint16_t*>(values));
    _rtuPendingRequestCount--;
    _rtuMessageCount++;
    if (err != ESP_OK) {
        _rtuErrorCount++;
        ESP_LOGE(TAG, "masterWriteMultipleHolding(addr=%u, count=%u) failed: 0x%x (%s)", addr, count, err, esp_err_to_name(err));
    }
    return err;
}

void PhaseSwitch::loop(){
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
      _state = State::ConfirmedOff;
      _previous = nowMs();
      _delay = 2000;
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
    if (_state == State::SwitchedOn && nowMs() > _wallboxResponseDeadlineMs) {
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
    
    // Update registers every 2 seconds
    static uint32_t last_update = 0;
    if (nowMs() - last_update > 2000) {
        updateCachedRegisters();
        last_update = nowMs();
    }
    return;
  }
  else if (_state == State::SwitchPhases){
    if (masterWriteHolding(HEC_REG_MAX_CURRENT, 0) != ESP_OK) return;
    if (masterWriteHolding(HEC_REG_REMOTE_LOCK, 0) != ESP_OK) return;
    
    ESP_LOGI(TAG, "phase switch started");
    _state = State::WaitingForZero;
    return;
  }
  else if (_state == State::WaitingForZero){
    uint16_t data[4]; 
    if (masterReadInput(5, 4, data) != ESP_OK) return;
    
    uint16_t state = data[0];
    uint16_t l1 = data[1];
    uint16_t l2 = data[2];
    uint16_t l3 = data[3];
    
    if (state != 10) return;
    if (l1 > 0 || l2 > 0 || l3 > 0) return;
    
    ESP_LOGI(TAG, "zero load confirmed");
    _state = State::ConfirmedZero;
    return;
  }
  else if (_state == State::ConfirmedZero){
    writePin(PIN_1P_OUT, RELAY_OFF);
    writePin(PIN_3P_OUT, RELAY_OFF);
    _state = State::WaitingForOff;
    ESP_LOGI(TAG, "switched off");
    return;
  }
  else if (_state == State::SwitchedOn){
    if (_desiredPhases == 3){
      if (readPin(PIN_3P_IN) != 1) {
        if (nowMs() > _switchOnDeadlineMs) {
          enterSafetyFault(SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT, "timeout waiting for 3P feedback input");
          return;
        }
        return;
      }
      if (getActivePhases() != 3) {
        if (nowMs() > _switchOnDeadlineMs) {
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
        if (nowMs() > _switchOnDeadlineMs) {
          enterSafetyFault(SAFETY_FAULT_SWITCH_CONFIRM_TIMEOUT, "timeout waiting for 1P feedback input");
          return;
        }
        return;
      }
      if (getActivePhases() != 1) {
        if (nowMs() > _switchOnDeadlineMs) {
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
    if (_phaseSwitchRequested) {
      uint16_t cached = _holdingRegister[HEC_REG_MAX_CURRENT - HOLDING_REG_OFFSET];
      if (_currentWrittenDuringSwitch) {
        restartCurrent = cached;
      } else {
        restartCurrent = cached < HEC_MIN_CURRENT ? cached : HEC_MIN_CURRENT;
      }
    }
    if (masterWriteHolding(HEC_REG_MAX_CURRENT, restartCurrent) != ESP_OK) return;
    cacheHolding(HEC_REG_MAX_CURRENT, &restartCurrent, 1);
    // Release the remote lock only once the restart current is in place.
    if (masterWriteHolding(HEC_REG_REMOTE_LOCK, 1) != ESP_OK) return;

    _phaseSwitchRequested = false;
    _currentWrittenDuringSwitch = false;
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
      _state = State::Running;
      return;
    }
    if (nowMs() > _unlockDeadlineMs) {
      ESP_LOGW(TAG, "timeout waiting for unlock, writing it again");
      masterWriteHolding(HEC_REG_REMOTE_LOCK, 1);
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
  if ((int32_t)(nowMs() - _restartDeadlineMs) > 0) {
    ESP_LOGW(TAG, "timeout waiting for lock and zero current, restarting");
    restartNow();
  }
}

void PhaseSwitch::switchTo1P(){
  if (!canSwitchTo1P()) return;
  rememberCarState();
  _desiredPhases = 1;
  _phaseSwitchRequested = true;
  _currentWrittenDuringSwitch = false;
  _state = State::SwitchPhases;
}

void PhaseSwitch::switchTo3P(){
  if (!canSwitchTo3P()) return;
  rememberCarState();
  _desiredPhases = 3;
  _phaseSwitchRequested = true;
  _currentWrittenDuringSwitch = false;
  _state = State::SwitchPhases;
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

  _safetyFaultCode = code;
  _safetyFaultText = text ? std::string(text) : std::string("unspecified safety fault");
  _delay = 0;
  _switchOnDeadlineMs = 0;
  writePin(PIN_1P_OUT, RELAY_OFF);
  writePin(PIN_3P_OUT, RELAY_OFF);
  _state = State::Fault;

  ESP_LOGE(TAG, "SAFETY FAULT 0x%04X: %s", code, _safetyFaultText.c_str());
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
    snprintf(hex, sizeof(hex), " [0x%04X]", _safetyFaultCode);
    result += hex;
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
  return (_safetyFaultCode == SAFETY_FAULT_NONE) ? "none" : _safetyFaultText;
}

uint16_t PhaseSwitch::getHoldingRegister(size_t reg){
  if (reg < HOLDING_REG_OFFSET) return 0xffff;
  size_t idx = reg - HOLDING_REG_OFFSET;
  if (idx >= _holdingRegister.size()) return 0xffff;
  return _holdingRegister[idx];
}

uint16_t PhaseSwitch::getInputRegister(size_t reg){
  if (reg >= _inputRegister.size()) return 0xffff;
  return _inputRegister[reg];
}

bool PhaseSwitch::updateCachedRegisters(){
  if (!_master_handle) return false;
  esp_err_t err = masterReadInput(4, 15, &_inputRegister[4]);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "masterReadInput failed: 0x%x (%s)", err, esp_err_to_name(err));
      return false;
  }
  err = masterReadHolding(257, 6, &_holdingRegister[257 - HOLDING_REG_OFFSET]);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "masterReadHolding failed: 0x%x (%s)", err, esp_err_to_name(err));
      return false;
  }
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
    _bridgeActiveClientCount++;
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
    uint16_t phases = _desiredPhases;
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
  if (_switchingSupported && _firmwareSupported && addr == 4) {
    if (value == 1) {
      switchTo1P();
    } else if (value == 3) {
      switchTo3P();
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
    if (addr < HOLDING_REG_OFFSET || (addr - HOLDING_REG_OFFSET) >= _holdingRegister.size()) {
      buildException(FC_WRITE_HOLDING, EX_ILLEGAL_DATA_ADDRESS, responsePdu);
      return true;
    }
    cacheHolding(addr, &value, 1);
    if (_phaseSwitchRequested && addr == HEC_REG_MAX_CURRENT) {
      _currentWrittenDuringSwitch = true;
    }
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
    if (addr < HOLDING_REG_OFFSET || (addr - HOLDING_REG_OFFSET + count) > _holdingRegister.size()) {
      buildException(FC_WRITE_MULTIPLE, EX_ILLEGAL_DATA_ADDRESS, responsePdu);
      return true;
    }
    cacheHolding(addr, values, count);
    if (_phaseSwitchRequested && addr <= HEC_REG_MAX_CURRENT && HEC_REG_MAX_CURRENT < addr + count) {
      _currentWrittenDuringSwitch = true;
    }
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
  size_t idx = addr - HOLDING_REG_OFFSET;
  if (idx + count > _holdingRegister.size()) return;
  for (uint16_t i = 0; i < count; i++) {
    _holdingRegister[idx + i] = values[i];
  }
}

void PhaseSwitch::cacheInput(uint16_t addr, const uint16_t* values, uint16_t count)
{
  if (!values || (size_t)addr + count > _inputRegister.size()) return;
  for (uint16_t i = 0; i < count; i++) {
    _inputRegister[addr + i] = values[i];
  }
}

bool PhaseSwitch::readCachedHolding(uint16_t addr, uint16_t count, std::vector<uint16_t>& values)
{
  if (addr < HOLDING_REG_OFFSET) return false;
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
