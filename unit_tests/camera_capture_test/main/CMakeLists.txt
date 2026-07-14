idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_common esp_timer freertos esp32-camera
)
