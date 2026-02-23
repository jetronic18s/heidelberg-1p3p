#ifndef MODBUS_TCP_BRIDGE_H
#define MODBUS_TCP_BRIDGE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>

#include <ModbusMessage.h>
#include <ModbusTypeDefs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class ModbusTcpBridge {
 public:
  using Worker = std::function<ModbusMessage(ModbusMessage)>;

  ModbusTcpBridge();
  ~ModbusTcpBridge();

  bool registerWorker(uint8_t serverId, uint8_t functionCode, Worker worker);
  bool start(uint16_t port, uint8_t maxClients, uint32_t timeoutMs);
  bool stop();
  bool isRunning() const;

  uint32_t getMessageCount() const;
  uint32_t activeClients() const;
  uint32_t getErrorCount() const;

 private:
  static void acceptTaskThunk(void *arg);
  static void clientTaskThunk(void *arg);
  void acceptTask();
  void clientTask(int clientFd);

  ModbusMessage dispatch(const ModbusMessage &request);
  bool recvAll(int fd, uint8_t *buf, size_t len);
  bool sendAll(int fd, const uint8_t *buf, size_t len);
  static void closeSocket(int fd);

  std::map<uint8_t, std::map<uint8_t, Worker>> _workers;
  SemaphoreHandle_t _workerLock;

  std::atomic<bool> _running;
  std::atomic<uint32_t> _messageCount;
  std::atomic<uint32_t> _activeClients;
  std::atomic<uint32_t> _errorCount;

  int _listenFd;
  uint16_t _port;
  uint8_t _maxClients;
  uint32_t _timeoutMs;
  TaskHandle_t _acceptTaskHandle;
};

#endif
