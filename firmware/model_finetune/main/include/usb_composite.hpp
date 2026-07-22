#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Message structure matching the TinyUSB CDC RX events
typedef struct {
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t buf_len;
    uint8_t itf;
} usb_cdc_msg_t;

// Initialize the TinyUSB CDC + MSC composite device and mount wear-levelling FATFS
void usb_composite_init();

// Thread-safe format-printf to CDC port
int usb_cdc_printf(const char *format, ...);

// Thread-safe block-write to CDC port with flow control
void usb_cdc_write(const uint8_t *buf, size_t len);

// Thread-safe base64 block writer for CDC port
void usb_cdc_write_base64(const uint8_t *data, size_t length);

// Get the handle of the incoming CDC commands queue
QueueHandle_t usb_cdc_get_queue();

// Reclaim storage back to APP side (mounts FATFS locally)
esp_err_t usb_msc_mount_to_app();

// Expose storage to host PC side (Windows USB mount)
esp_err_t usb_msc_mount_to_pc();

// Check if CDC client is connected (monitoring DTR/RTS)
bool usb_cdc_is_connected();

#ifdef __cplusplus
}
#endif
