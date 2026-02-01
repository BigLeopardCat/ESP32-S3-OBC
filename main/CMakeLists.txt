idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash nvs_flash esp_wifi esp_event esp_http_client esp_https_ota app_update esp_partition bt driver esp_phy mbedtls
                       INCLUDE_DIRS "")
