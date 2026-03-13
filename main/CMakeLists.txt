idf_component_register(
    SRCS "receiver.cpp" "tflm_inference.cpp"
    PRIV_REQUIRES esp_wifi nvs_flash mqtt esp_event esp_http_client esp_pm esp_netif json esp-tflite-micro
    INCLUDE_DIRS "."
)
