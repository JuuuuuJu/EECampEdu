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

// Initialize the UVC + CDC composite device and mount the FAT photo-storage
// partition to /usb (locally, for photo_storage). No MSC is exposed.
void usb_composite_init();

// Thread-safe format-printf to the CDC control port.
int usb_cdc_printf(const char *format, ...);

// Thread-safe block-write to the CDC control port with flow control.
void usb_cdc_write(const uint8_t *buf, size_t len);

// Get the handle of the incoming CDC commands queue.
QueueHandle_t usb_cdc_get_queue();

// True while a CDC client asserts DTR/RTS.
bool usb_cdc_is_connected();

// --- UVC video (MJPEG) ---------------------------------------------------
// True once a UVC host has opened (committed) the video stream.
bool uvc_video_ready();

// True when the stream is open AND the previous frame has finished sending, so
// the caller may safely fill its JPEG buffer and submit the next frame.
bool uvc_video_can_send();

// Hand one JPEG frame to the UVC streaming endpoint. Non-blocking: returns
// false if the stream is closed or the previous frame is still in flight.
// The buffer must stay valid until the next uvc_video_ready()/submit cycle;
// callers should submit from a dedicated buffer they own.
bool uvc_video_submit_frame(const uint8_t *jpeg, size_t len);

#ifdef __cplusplus
}
#endif
