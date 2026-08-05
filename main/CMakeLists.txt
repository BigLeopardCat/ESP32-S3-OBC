idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver \
             esp_http_client esp_https_ota esp_timer    # OTA + 定时器
    EMBED_TXTFILES "saudade.site.ca.pem"                # 嵌入服务器证书链
)
