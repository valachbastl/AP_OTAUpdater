idf_component_register(
    SRCS "AP_OTAUpdater.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client app_update esp_timer mbedtls
)
