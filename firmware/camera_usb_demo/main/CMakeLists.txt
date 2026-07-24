idf_build_get_property(target IDF_TARGET)

idf_component_register(
    SRCS
        "src/app_main.cpp"
        "src/camera_capture_ov2640.cpp"
        "src/input_controls.cpp"
        "src/photo_storage.cpp"
        "src/usb_composite.cpp"
        "src/usb_descriptors.c"
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
        fatfs
        wear_levelling
)

# The TinyUSB video (UVC) class is present in the TinyUSB sources but esp_tinyusb
# never enables it (no Kconfig option, no CFG_TUD_VIDEO in its tusb_config.h).
# Define the class knobs so the video class compiles in. They are #ifndef-guarded
# in tusb_option.h, so defining them here wins. They must be visible both to
# TinyUSB's own sources and to ours (which include tusb.h / video_device.h).
set(uvc_defs
    CFG_TUD_VIDEO=1
    CFG_TUD_VIDEO_STREAMING=1
    CFG_TUD_VIDEO_STREAMING_BULK=0
    CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=512
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${uvc_defs})

idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_definitions(${tusb_lib} PUBLIC ${uvc_defs})
