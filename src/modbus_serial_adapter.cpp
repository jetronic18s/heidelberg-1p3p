#include "modbus_serial_adapter.h"

#include <ModbusClientRTU.h>
#include <Stream.h>

#include "driver/uart.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"

namespace {

class IdfUartStream : public Stream {
 public:
  IdfUartStream(uart_port_t port, int txPin, int rxPin)
      : _port(port), _txPin(txPin), _rxPin(rxPin), _peek(-1), _started(false) {}

  bool begin(uint32_t baudrate) {
    if (_started) {
      return true;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = static_cast<int>(baudrate);
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_EVEN;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.source_clk = UART_SCLK_DEFAULT;
#endif

    if (uart_driver_install(_port, 512, 512, 0, nullptr, 0) != ESP_OK) {
      return false;
    }
    if (uart_param_config(_port, &cfg) != ESP_OK) {
      return false;
    }
    if (uart_set_pin(_port, _txPin, _rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
      return false;
    }
    _started = true;
    return true;
  }

  int available() override {
    size_t len = 0;
    uart_get_buffered_data_len(_port, &len);
    return static_cast<int>(len) + (_peek >= 0 ? 1 : 0);
  }

  int read() override {
    if (_peek >= 0) {
      int v = _peek;
      _peek = -1;
      return v;
    }
    uint8_t b = 0;
    int n = uart_read_bytes(_port, &b, 1, 0);
    return (n == 1) ? static_cast<int>(b) : -1;
  }

  int peek() override {
    if (_peek >= 0) {
      return _peek;
    }
    uint8_t b = 0;
    int n = uart_read_bytes(_port, &b, 1, 0);
    if (n == 1) {
      _peek = static_cast<int>(b);
      return _peek;
    }
    return -1;
  }

  size_t write(uint8_t b) override {
    return write(&b, 1);
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    int n = uart_write_bytes(_port, reinterpret_cast<const char *>(buffer), size);
    return n > 0 ? static_cast<size_t>(n) : 0U;
  }

  void flush() override {
    uart_wait_tx_done(_port, pdMS_TO_TICKS(1000));
  }

 private:
  uart_port_t _port;
  int _txPin;
  int _rxPin;
  int _peek;
  bool _started;
};

static IdfUartStream &modbusSerialPort() {
#ifdef BOARD_DINGTIAN
  static IdfUartStream stream(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#else
  static IdfUartStream stream(UART_NUM_2, GPIO_NUM_17, GPIO_NUM_16);
#endif
  return stream;
}

}

void setupModbusClientSerial(ModbusClientRTU &client)
{
  auto &serial = modbusSerialPort();
  if (!serial.begin(19200)) {
    return;
  }
  client.setTimeout(500);
  client.begin(serial, 19200, 1);
}
