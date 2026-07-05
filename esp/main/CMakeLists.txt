idf_build_get_property(target IDF_TARGET)

idf_component_register(
    SRCS
        "src/app_main.cpp"
        "src/camera_capture_stub.cpp"
        "src/photo_storage.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp_common
        esp_partition
        esp_timer
        esp_system
        freertos
        spi_flash
        esp-tflite-micro
)
