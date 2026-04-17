idf_component_register(
    SRCS "main.c" "scale_ble_client.c"
         "http_webhook.c" "nvs_config.c" "wifi_manager.c"
         "captive_portal.c" "web_config.c"
    INCLUDE_DIRS "."
)
