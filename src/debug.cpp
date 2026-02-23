#include "debug.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

static bool s_telnet_enabled = false;
static int s_listen_fd = -1;
static int s_client_fd = -1;

static void closeFd(int &fd)
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

static bool setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool ensureServer()
{
    if (s_listen_fd >= 0) {
        return true;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(23);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return false;
    }
    if (!setNonBlocking(fd)) {
        close(fd);
        return false;
    }

    s_listen_fd = fd;
    return true;
}

static void closeTelnet()
{
    closeFd(s_client_fd);
    closeFd(s_listen_fd);
}

static void acceptClient()
{
    if (s_listen_fd < 0) {
        return;
    }
    sockaddr_in remote = {};
    socklen_t remote_len = sizeof(remote);
    int fd = accept(s_listen_fd, reinterpret_cast<sockaddr *>(&remote), &remote_len);
    if (fd < 0) {
        return;
    }
    if (!setNonBlocking(fd)) {
        close(fd);
        return;
    }
    if (s_client_fd >= 0) {
        closeFd(s_client_fd);
    }
    s_client_fd = fd;
}

static void pollClientState()
{
    if (s_client_fd < 0) {
        return;
    }
    char dummy[32];
    int n = recv(s_client_fd, dummy, sizeof(dummy), MSG_DONTWAIT);
    if (n == 0) {
        closeFd(s_client_fd);
        return;
    }
    if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        closeFd(s_client_fd);
    }
}

static void writeBytes(const char *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    fwrite(data, 1, len, stdout);
    fflush(stdout);

    if (!s_telnet_enabled || s_client_fd < 0) {
        return;
    }

    size_t remaining = len;
    const char *ptr = data;
    while (remaining > 0) {
        int n = send(s_client_fd, ptr, remaining, MSG_DONTWAIT);
        if (n > 0) {
            remaining -= static_cast<size_t>(n);
            ptr += n;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            break;
        }
        closeFd(s_client_fd);
        break;
    }
}

static void writeRaw(const char *text)
{
    if (text == nullptr) {
        return;
    }
    writeBytes(text, strlen(text));
}

}

void debugInit()
{
}

void debugLoop()
{
    if (!s_telnet_enabled) {
        if (s_listen_fd >= 0 || s_client_fd >= 0) {
            closeTelnet();
        }
        return;
    }

    if (!ensureServer()) {
        return;
    }

    acceptClient();
    pollClientState();
}

void debugSetTelnetEnabled(bool enabled)
{
    s_telnet_enabled = enabled;
    if (!s_telnet_enabled) {
        closeTelnet();
    }
}

void debugWrite(const char *text)
{
    writeRaw(text);
}

void debugWrite(const std::string &text)
{
    writeRaw(text.c_str());
}

void debugWrite(char value)
{
    writeBytes(&value, 1);
}

void debugWrite(int value)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", value);
    if (n > 0) {
        writeBytes(buf, static_cast<size_t>(n));
    }
}

void debugWrite(unsigned int value)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u", value);
    if (n > 0) {
        writeBytes(buf, static_cast<size_t>(n));
    }
}

void debugWrite(long value)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld", value);
    if (n > 0) {
        writeBytes(buf, static_cast<size_t>(n));
    }
}

void debugWrite(unsigned long value)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lu", value);
    if (n > 0) {
        writeBytes(buf, static_cast<size_t>(n));
    }
}

void debugWriteLine(const char *text)
{
    debugWrite(text);
    debugWrite('\n');
}

void debugWriteLine(const std::string &text)
{
    debugWrite(text);
    debugWrite('\n');
}

void debugWriteLine(char value)
{
    debugWrite(value);
    debugWrite('\n');
}

void debugWriteLine(int value)
{
    debugWrite(value);
    debugWrite('\n');
}

void debugWriteLine(unsigned int value)
{
    debugWrite(value);
    debugWrite('\n');
}

void debugWriteLine(long value)
{
    debugWrite(value);
    debugWrite('\n');
}

void debugWriteLine(unsigned long value)
{
    debugWrite(value);
    debugWrite('\n');
}
