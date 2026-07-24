#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

// UVC advertised stream geometry. Kept modest for reliable MJPEG over the
// ESP32-S3 full-speed (12 Mbps) USB. The camera is locked to this size while a
// UVC host is streaming; tune here if you need a different preview resolution.
#define UVC_FRAME_WIDTH   320
#define UVC_FRAME_HEIGHT  240
#define UVC_FRAME_RATE    15

// Endpoint addresses for the CDC + UVC composite.
//   CDC:  0x81 notification (interrupt IN), 0x02 data OUT, 0x82 data IN
//   UVC:  0x83 video (isochronous IN)
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_UVC_IN      0x83

// Interface layout of the composite configuration.
enum {
  ITF_NUM_CDC = 0,          // CDC-ACM communications interface
  ITF_NUM_CDC_DATA,         // CDC-ACM data interface
  ITF_NUM_VIDEO_CONTROL,    // UVC VideoControl interface
  ITF_NUM_VIDEO_STREAMING,  // UVC VideoStreaming interface
  ITF_NUM_TOTAL
};

// String descriptor indices.
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC,
  STRID_UVC_CONTROL,
  STRID_UVC_STREAMING,
};

// Exposed so usb_composite.cpp can hand them to tinyusb_driver_install().
extern const tusb_desc_device_t model_finetune_device_descriptor;
extern const uint8_t *model_finetune_fs_config_descriptor;
extern const char *model_finetune_string_descriptors[];
extern const int model_finetune_string_descriptor_count;

#ifdef __cplusplus
}
#endif
