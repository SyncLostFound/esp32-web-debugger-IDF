idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_adc esp_timer nvs_flash esp_netif esp_wifi esp_event spiffs driver
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE HTTPD_WS_SUPPORT=1)
