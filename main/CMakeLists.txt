idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver   # ① 要有 json（cJSON 用）
    EMBED_TXTFILES "saudade.site.ca.pem"                               # ② 嵌入证书
)
