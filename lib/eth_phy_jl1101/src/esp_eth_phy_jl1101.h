#pragma once

#include "esp_eth.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new JL1101 PHY driver instance.
 *
 * @param[in] config: configuration for the PHY driver
 *
 * @return
 *      - instance: on success
 *      - NULL: on failure
 */
esp_eth_phy_t *esp_eth_phy_new_jl1101(const eth_phy_config_t *config);

#ifdef __cplusplus
}
#endif
