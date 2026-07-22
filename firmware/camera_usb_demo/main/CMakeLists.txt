idf_build_get_property(target IDF_TARGET)

idf_component_register(
    SRCS
        "src/app_main.cpp"
        "src/camera_capture_ov2640.cpp"
        "src/input_controls.cpp"
        "src/photo_storage.cpp"
        "src/usb_composite.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp_common
        esp_partition
        esp_psram
        esp_timer
        esp_system
        freertos
        spi_flash
        esp32-camera
        esp_tinyusb
        esp_driver_uart
        esp_driver_gpio
)
