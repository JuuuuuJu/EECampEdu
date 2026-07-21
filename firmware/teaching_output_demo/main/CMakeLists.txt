idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_rmt esp_driver_usb_serial_jtag
)
