#include "ethernet_jl1101.h"

#include <cstdio>
#include "config.h"
#include "debug.h"
#include "driver/gpio.h"

extern "C" {
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
}

#include "esp_eth_phy_jl1101.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef BOARD_DINGTIAN

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static volatile bool s_eth_got_ip = false;
static volatile bool s_eth_link_up = false;

static uint32_t nowMs()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void sleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool parseIpv4(const char *text, esp_ip4_addr_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    ip4_addr_t parsed;
    if (!ip4addr_aton(text, &parsed)) {
        return false;
    }
    out->addr = parsed.addr;
    return true;
}

static void onEthEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void) arg;
    (void) event_base;
    (void) event_data;

    switch (event_id) {
        case ETHERNET_EVENT_START:
            break;
        case ETHERNET_EVENT_STOP:
            s_eth_link_up = false;
            s_eth_got_ip = false;
            break;
        case ETHERNET_EVENT_CONNECTED: {
            s_eth_link_up = true;
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            s_eth_link_up = false;
            s_eth_got_ip = false;
            break;
        default:
            break;
    }
}

static void onGotIp(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void) arg;
    (void) event_base;
    (void) event_id;
    s_eth_got_ip = true;
}

bool ethernetHasIp()
{
    return s_eth_got_ip;
}

bool ethernetHasLink()
{
    return s_eth_link_up;
}

bool ethernetWaitForIp(uint32_t timeout_ms)
{
    const uint32_t start = nowMs();
    while (!s_eth_got_ip && (nowMs() - start) < timeout_ms) {
        sleepMs(50);
    }
    return s_eth_got_ip;
}

std::string ethernetGetIpString()
{
    if (!s_eth_link_up || !s_eth_got_ip) {
        return std::string();
    }
    if (s_eth_netif == NULL) {
        return std::string();
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_eth_netif, &ip_info) != ESP_OK) {
        return std::string();
    }
    if (ip_info.ip.addr == 0) {
        return std::string();
    }
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    return std::string(ip_str);
}

std::string ethernetGetMacString()
{
    if (s_eth_handle == NULL) {
        return std::string();
    }
    uint8_t mac[6] = {0};
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) != ESP_OK) {
        return std::string();
    }
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

bool ethernetSetHostname(const char *hostname)
{
    if (s_eth_netif == NULL || hostname == NULL || hostname[0] == '\0') {
        return false;
    }
    return esp_netif_set_hostname(s_eth_netif, hostname) == ESP_OK;
}

bool ethernetConfigureDhcp()
{
    if (s_eth_netif == NULL) {
        return false;
    }
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_err_t err = esp_netif_dhcpc_start(s_eth_netif);
    return err == ESP_OK;
}

bool ethernetConfigureStatic(const char *ip, const char *gw, const char *mask, const char *dns1, const char *dns2)
{
    if (s_eth_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    if (!parseIpv4(ip, &ip_info.ip) || !parseIpv4(gw, &ip_info.gw) || !parseIpv4(mask, &ip_info.netmask)) {
        return false;
    }

    esp_netif_dhcpc_stop(s_eth_netif);
    if (esp_netif_set_ip_info(s_eth_netif, &ip_info) != ESP_OK) {
        return false;
    }

    esp_netif_dns_info_t dns_info = {};
    if (dns1 != NULL && dns1[0] != '\0' && parseIpv4(dns1, &dns_info.ip.u_addr.ip4)) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    if (dns2 != NULL && dns2[0] != '\0' && parseIpv4(dns2, &dns_info.ip.u_addr.ip4)) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_BACKUP, &dns_info);
    }

    return true;
}

bool setupEthernet()
{
    if (s_eth_handle != NULL) {
        return true;
    }

    s_eth_got_ip = false;
    s_eth_link_up = false;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        dbgln("[eth] esp_netif_init failed");
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        dbgln("[eth] event loop create failed");
        return false;
    }

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &onEthEvent, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &onGotIp, NULL);

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (s_eth_netif == NULL) {
        dbgln("[eth] netif create failed");
        return false;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    // JL1101 on Dingtian boards often sits at PHY addr 0
    phy_config.phy_addr = 0;
    phy_config.reset_gpio_num = PIN_ETH_PWR;

    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    eth_esp32_emac_config_t emac_config = {
        .smi_gpio = {
            .mdc_num = PIN_ETH_MDC,
            .mdio_num = PIN_ETH_MDIO,
        },
        .interface = EMAC_DATA_INTERFACE_RMII,
        .clock_config = {
            .rmii = {
                .clock_mode = EMAC_CLK_OUT,
                .clock_gpio = (emac_rmii_clock_gpio_t)EMAC_CLK_OUT_180_GPIO,
            }
        },
        .intr_priority = 0,
    };
    #else
    mac_config.smi_mdc_gpio_num = PIN_ETH_MDC;
    mac_config.smi_mdio_gpio_num = PIN_ETH_MDIO;
    mac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    mac_config.clock_config.rmii.clock_gpio = EMAC_CLK_OUT_180_GPIO;
    #endif

    // Ensure PHY power/reset line is asserted before init
    gpio_reset_pin((gpio_num_t)PIN_ETH_PWR);
    gpio_set_direction((gpio_num_t)PIN_ETH_PWR, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_ETH_PWR, 1);
    sleepMs(200);

    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    #else
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
    #endif
    esp_eth_phy_t *phy = esp_eth_phy_new_jl1101(&phy_config);
    if (mac == NULL || phy == NULL) {
        dbgln("[eth] mac/phy create failed");
        return false;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&config, &s_eth_handle);
    if (err != ESP_OK) {
        dbgln("[eth] driver install failed");
        return false;
    }

    err = esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));
    if (err != ESP_OK) {
        dbgln("[eth] netif attach failed");
        return false;
    }

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        dbgln("[eth] start failed");
        return false;
    }

    return true;
}

#else

bool setupEthernet()
{
    return false;
}

bool ethernetHasIp()
{
    return false;
}

bool ethernetHasLink()
{
    return false;
}

bool ethernetWaitForIp(uint32_t)
{
    return false;
}

std::string ethernetGetIpString()
{
    return std::string();
}

std::string ethernetGetMacString()
{
    return std::string();
}

bool ethernetSetHostname(const char *)
{
    return false;
}

bool ethernetConfigureDhcp()
{
    return false;
}

bool ethernetConfigureStatic(const char *, const char *, const char *, const char *, const char *)
{
    return false;
}

#endif
