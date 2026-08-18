set(PRIV_REQUIRES_LIST
    nvs_flash
    driver
    esp_timer
    esp_event
    esp_app_format
    bt
    esp_matter
    chip
    esp_adc
    mbedtls
    device
    led_driver
    esp_https_ota
    esp_http_client
    esp_http_server
    app_update
)

set(SRC_FILES
    app_main.cpp
    diagnostics.cpp
    door_controller.cpp
    comm_layer.cpp
    hal_gpio.cpp
    status_led.cpp
)

idf_component_register(
    SRCS ${SRC_FILES}
    PRIV_REQUIRES ${PRIV_REQUIRES_LIST}
    INCLUDE_DIRS "."
)
