idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_common esp_heap_caps esp_partition esp_timer freertos spi_flash esp-tflite-micro
)
