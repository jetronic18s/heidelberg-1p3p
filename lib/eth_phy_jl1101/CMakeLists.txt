idf_component_register(
  SRCS "src/esp_eth_phy_jl1101.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_eth esp_driver_gpio
)
