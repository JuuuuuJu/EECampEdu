#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

// The five MJPEG modes exposed to the browser UVC driver. The host may request
// 1..30 fps for every mode; actual throughput is reported by the firmware.
// 96x96 is intentionally absent: the sensor's live preview at that crop does not
// work. The 96x96 grayscale capture path for model inputs is separate and still
// reconfigures the sensor to FRAMESIZE_96X96 on demand.
// Keep these in sync with USB_CAM_MODES in apps/training_portal/templates/index.html.
#define UVC_MODE_COUNT 5
#define UVC_MAX_FPS 30
#define UVC_DEFAULT_MODE 1
#define UVC_DEFAULT_FPS 15
#define UVC_MAX_FRAME_BYTES (1600 * 1200 * 2)

// Endpoint addresses for the CDC + UVC + MSC composite.
//   CDC:  0x81 notification (interrupt IN), 0x02 data OUT, 0x82 data IN
//   UVC:  0x83 video (isochronous IN)
//   MSC:  0x04 data OUT, 0x84 data IN
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_UVC_IN      0x83
#define EPNUM_MSC_OUT     0x04
#define EPNUM_MSC_IN      0x84

// Interface layout of the composite configuration.
enum {
  ITF_NUM_CDC = 0,          // CDC-ACM communications interface
  ITF_NUM_CDC_DATA,         // CDC-ACM data interface
  ITF_NUM_VIDEO_CONTROL,    // UVC VideoControl interface
  ITF_NUM_VIDEO_STREAMING,  // UVC VideoStreaming interface
  ITF_NUM_MSC,              // Mass-storage interface
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
  STRID_MSC,
};

// Exposed so usb_composite.cpp can hand them to tinyusb_driver_install().
extern const tusb_desc_device_t camusb_device_descriptor;
extern const uint8_t *camusb_fs_config_descriptor;
extern const char *camusb_string_descriptors[];
extern const int camusb_string_descriptor_count;

#ifdef __cplusplus
}
#endif
