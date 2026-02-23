#include "modbus_tcp_bridge.h"

#include <vector>

#include <lwip/sockets.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {
struct ClientTaskCtx {
  ModbusTcpBridge *bridge;
  int fd;
};
}  // namespace

ModbusTcpBridge::ModbusTcpBridge()
    : _workerLock(xSemaphoreCreateMutex()),
      _running(false),
      _messageCount(0),
      _activeClients(0),
      _errorCount(0),
      _listenFd(-1),
      _port(0),
      _maxClients(0),
      _timeoutMs(0),
      _acceptTaskHandle(nullptr) {}

ModbusTcpBridge::~ModbusTcpBridge() {
  stop();
  if (_workerLock != nullptr) {
    vSemaphoreDelete(_workerLock);
    _workerLock = nullptr;
  }
}

bool ModbusTcpBridge::registerWorker(uint8_t serverId, uint8_t functionCode, Worker worker) {
  if (_workerLock == nullptr || !worker) {
    return false;
  }
  xSemaphoreTake(_workerLock, portMAX_DELAY);
  _workers[serverId][functionCode] = std::move(worker);
  xSemaphoreGive(_workerLock);
  return true;
}

bool ModbusTcpBridge::start(uint16_t port, uint8_t maxClients, uint32_t timeoutMs) {
  if (_running.load()) {
    return true;
  }

  _listenFd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (_listenFd < 0) {
    _errorCount++;
    return false;
  }

  int yes = 1;
  lwip_setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (lwip_bind(_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    _errorCount++;
    closeSocket(_listenFd);
    _listenFd = -1;
    return false;
  }

  if (lwip_listen(_listenFd, maxClients > 0 ? maxClients : 1) != 0) {
    _errorCount++;
    closeSocket(_listenFd);
    _listenFd = -1;
    return false;
  }

  _port = port;
  _maxClients = maxClients > 0 ? maxClients : 1;
  _timeoutMs = timeoutMs;
  _running = true;

  BaseType_t rc = xTaskCreate(&ModbusTcpBridge::acceptTaskThunk, "mb_tcp_accept", 4096, this, 4, &_acceptTaskHandle);
  if (rc != pdPASS) {
    _running = false;
    _errorCount++;
    closeSocket(_listenFd);
    _listenFd = -1;
    return false;
  }
  return true;
}

bool ModbusTcpBridge::stop() {
  bool wasRunning = _running.exchange(false);
  if (_listenFd >= 0) {
    closeSocket(_listenFd);
    _listenFd = -1;
  }
  return wasRunning;
}

bool ModbusTcpBridge::isRunning() const { return _running.load(); }

uint32_t ModbusTcpBridge::getMessageCount() const { return _messageCount.load(); }

uint32_t ModbusTcpBridge::activeClients() const { return _activeClients.load(); }

uint32_t ModbusTcpBridge::getErrorCount() const { return _errorCount.load(); }

void ModbusTcpBridge::acceptTaskThunk(void *arg) {
  static_cast<ModbusTcpBridge *>(arg)->acceptTask();
}

void ModbusTcpBridge::clientTaskThunk(void *arg) {
  ClientTaskCtx *ctx = static_cast<ClientTaskCtx *>(arg);
  ModbusTcpBridge *bridge = ctx->bridge;
  int fd = ctx->fd;
  delete ctx;
  bridge->clientTask(fd);
}

void ModbusTcpBridge::acceptTask() {
  while (_running.load()) {
    sockaddr_in clientAddr = {};
    socklen_t addrLen = sizeof(clientAddr);
    int clientFd = lwip_accept(_listenFd, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
    if (clientFd < 0) {
      if (_running.load()) {
        _errorCount++;
      }
      continue;
    }

    if (_activeClients.load() >= _maxClients) {
      _errorCount++;
      closeSocket(clientFd);
      continue;
    }

    ClientTaskCtx *ctx = new ClientTaskCtx{this, clientFd};
    if (ctx == nullptr) {
      _errorCount++;
      closeSocket(clientFd);
      continue;
    }

    TaskHandle_t task = nullptr;
    BaseType_t rc = xTaskCreate(&ModbusTcpBridge::clientTaskThunk, "mb_tcp_client", 6144, ctx, 4, &task);
    if (rc != pdPASS) {
      delete ctx;
      _errorCount++;
      closeSocket(clientFd);
      continue;
    }
  }

  _acceptTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void ModbusTcpBridge::clientTask(int clientFd) {
  _activeClients++;

  timeval tv = {};
  tv.tv_sec = static_cast<long>(_timeoutMs / 1000U);
  tv.tv_usec = static_cast<long>((_timeoutMs % 1000U) * 1000U);
  lwip_setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  lwip_setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  while (_running.load()) {
    uint8_t mbap[7];
    if (!recvAll(clientFd, mbap, sizeof(mbap))) {
      break;
    }

    const uint16_t transactionId = (static_cast<uint16_t>(mbap[0]) << 8) | mbap[1];
    const uint16_t protocolId = (static_cast<uint16_t>(mbap[2]) << 8) | mbap[3];
    const uint16_t length = (static_cast<uint16_t>(mbap[4]) << 8) | mbap[5];
    const uint8_t unitId = mbap[6];

    if (protocolId != 0 || length < 2) {
      _errorCount++;
      break;
    }

    const size_t pduLen = static_cast<size_t>(length - 1);
    std::vector<uint8_t> pdu(pduLen);
    if (!recvAll(clientFd, pdu.data(), pdu.size())) {
      break;
    }

    ModbusMessage request;
    request.add(unitId);
    request.add(pdu.data(), static_cast<uint16_t>(pdu.size()));
    _messageCount++;

    ModbusMessage response = dispatch(request);
    if (response.size() < 2) {
      response.setError(request.getServerID(), request.getFunctionCode(), Modbus::SERVER_DEVICE_FAILURE);
      _errorCount++;
    }

    uint16_t responseLen = response.size();
    uint8_t outHdr[7] = {
        static_cast<uint8_t>(transactionId >> 8), static_cast<uint8_t>(transactionId & 0xFF),
        0, 0,
        static_cast<uint8_t>(responseLen >> 8), static_cast<uint8_t>(responseLen & 0xFF),
        response[0]};

    if (!sendAll(clientFd, outHdr, sizeof(outHdr))) {
      _errorCount++;
      break;
    }
    if (responseLen > 1 && !sendAll(clientFd, response.data() + 1, responseLen - 1)) {
      _errorCount++;
      break;
    }
  }

  closeSocket(clientFd);
  if (_activeClients.load() > 0) {
    _activeClients--;
  }
  vTaskDelete(nullptr);
}

ModbusMessage ModbusTcpBridge::dispatch(const ModbusMessage &request) {
  Worker worker;
  if (_workerLock == nullptr) {
    ModbusMessage error;
    error.setError(request.getServerID(), request.getFunctionCode(), Modbus::SERVER_DEVICE_FAILURE);
    return error;
  }

  xSemaphoreTake(_workerLock, portMAX_DELAY);
  auto serverIt = _workers.find(request.getServerID());
  if (serverIt != _workers.end()) {
    auto fcIt = serverIt->second.find(request.getFunctionCode());
    if (fcIt != serverIt->second.end()) {
      worker = fcIt->second;
    } else {
      auto anyIt = serverIt->second.find(Modbus::ANY_FUNCTION_CODE);
      if (anyIt != serverIt->second.end()) {
        worker = anyIt->second;
      }
    }
  }
  xSemaphoreGive(_workerLock);

  if (!worker) {
    ModbusMessage error;
    if (serverIt == _workers.end()) {
      error.setError(request.getServerID(), request.getFunctionCode(), Modbus::INVALID_SERVER);
    } else {
      error.setError(request.getServerID(), request.getFunctionCode(), Modbus::ILLEGAL_FUNCTION);
    }
    return error;
  }
  return worker(request);
}

bool ModbusTcpBridge::recvAll(int fd, uint8_t *buf, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    int rc = lwip_recv(fd, buf + pos, len - pos, 0);
    if (rc <= 0) {
      return false;
    }
    pos += static_cast<size_t>(rc);
  }
  return true;
}

bool ModbusTcpBridge::sendAll(int fd, const uint8_t *buf, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    int rc = lwip_send(fd, buf + pos, len - pos, 0);
    if (rc <= 0) {
      return false;
    }
    pos += static_cast<size_t>(rc);
  }
  return true;
}

void ModbusTcpBridge::closeSocket(int fd) {
  if (fd >= 0) {
    lwip_shutdown(fd, SHUT_RDWR);
    lwip_close(fd);
  }
}
