idf_component_register(SRCS "receiver.cpp"
                    PRIV_REQUIRES esp_wifi nvs_flash mqtt esp_event esp_http_client esp_pm json
                    INCLUDE_DIRS "."
)
