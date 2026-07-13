#pragma once

#include <stdint.h>
#include <string>

bool setupEthernet();
bool ethernetHasIp();
bool ethernetHasLink();
bool ethernetWaitForIp(uint32_t timeout_ms);
std::string ethernetGetIpString();
std::string ethernetGetMacString();
bool ethernetSetHostname(const char *hostname);
bool ethernetConfigureDhcp();
bool ethernetConfigureStatic(const char *ip, const char *gw, const char *mask, const char *dns1, const char *dns2);
