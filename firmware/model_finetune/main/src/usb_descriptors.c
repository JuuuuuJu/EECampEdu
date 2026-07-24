// USB descriptors for the model_finetune camera firmware.
//
// Composite device: CDC-ACM (control channel: commands, RESULT/INPUT lines,
// photo download) + UVC MJPEG (live video, so the browser sees a real webcam
// via getUserMedia). MSC is intentionally not present in this firmware.
//
// The UVC portion is adapted from TinyUSB's device/video_capture example,
// switched to MJPEG + isochronous IN, with the endpoint moved to 0x83 and CDC
// interfaces (0/1) placed ahead of the video interfaces (2/3).

#include "usb_descriptors.h"
#include "tusb.h"

// Espressif VID; custom PID for this CDC+UVC composite. A unique PID keeps
// Windows from reusing a cached driver from a different interface layout.
#define USB_VID   0x303A
#define USB_PID   0x4002
#define USB_BCD   0x0200

#define UVC_CLOCK_FREQUENCY  27000000

// Video entity IDs.
#define UVC_ENTITY_CAP_INPUT_TERMINAL   0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL  0x02

//--------------------------------------------------------------------+
// Device descriptor
//--------------------------------------------------------------------+
const tusb_desc_device_t model_finetune_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    // Composite device with IADs -> Miscellaneous / Common / IAD.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,

    .bNumConfigurations = 0x01,
};

//--------------------------------------------------------------------+
// Configuration descriptor: CDC + UVC (MJPEG, isochronous)
//--------------------------------------------------------------------+
typedef struct TU_ATTR_PACKED {
  tusb_desc_interface_t itf;
  tusb_desc_video_control_header_1itf_t header;
  tusb_desc_video_control_camera_terminal_t camera_terminal;
  tusb_desc_video_control_output_terminal_t output_terminal;
} uvc_control_desc_t;

typedef struct TU_ATTR_PACKED {
  tusb_desc_interface_t itf;                              // alt 0 (zero-bandwidth)
  tusb_desc_video_streaming_input_header_1byte_t header;
  tusb_desc_video_format_mjpeg_t format;
  tusb_desc_video_frame_mjpeg_1int_t frame;
  tusb_desc_video_streaming_color_matching_t color;
  tusb_desc_interface_t itf_alt;                          // alt 1 (streaming)
  tusb_desc_endpoint_t ep;                                // isochronous IN
} uvc_streaming_desc_t;

typedef struct TU_ATTR_PACKED {
  tusb_desc_configuration_t config;
  uint8_t cdc[TUD_CDC_DESC_LEN];
  tusb_desc_interface_assoc_t video_iad;
  uvc_control_desc_t video_control;
  uvc_streaming_desc_t video_streaming;
} composite_cfg_desc_t;

static const composite_cfg_desc_t s_fs_config = {
    .config = {
        .bLength             = sizeof(tusb_desc_configuration_t),
        .bDescriptorType     = TUSB_DESC_CONFIGURATION,
        .wTotalLength        = sizeof(composite_cfg_desc_t),
        .bNumInterfaces      = ITF_NUM_TOTAL,
        .bConfigurationValue = 1,
        .iConfiguration      = 0,
        .bmAttributes        = TU_BIT(7),   // bus powered
        .bMaxPower           = 250 / 2,     // 250 mA
    },

    // --- CDC-ACM (interfaces 0 + 1) ---
    .cdc = {
        TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                           EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    },

    // --- UVC (interfaces 2 + 3) ---
    .video_iad = {
        .bLength           = sizeof(tusb_desc_interface_assoc_t),
        .bDescriptorType   = TUSB_DESC_INTERFACE_ASSOCIATION,
        .bFirstInterface   = ITF_NUM_VIDEO_CONTROL,
        .bInterfaceCount   = 2,
        .bFunctionClass    = TUSB_CLASS_VIDEO,
        .bFunctionSubClass = VIDEO_SUBCLASS_INTERFACE_COLLECTION,
        .bFunctionProtocol = VIDEO_ITF_PROTOCOL_UNDEFINED,
        .iFunction         = 0,
    },

    .video_control = {
        .itf = {
            .bLength            = sizeof(tusb_desc_interface_t),
            .bDescriptorType    = TUSB_DESC_INTERFACE,
            .bInterfaceNumber   = ITF_NUM_VIDEO_CONTROL,
            .bAlternateSetting  = 0,
            .bNumEndpoints      = 0,
            .bInterfaceClass    = TUSB_CLASS_VIDEO,
            .bInterfaceSubClass = VIDEO_SUBCLASS_CONTROL,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15,
            .iInterface         = STRID_UVC_CONTROL,
        },
        .header = {
            .bLength            = sizeof(tusb_desc_video_control_header_1itf_t),
            .bDescriptorType    = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_HEADER,
            .bcdUVC             = VIDEO_BCD_1_50,
            .wTotalLength       = sizeof(uvc_control_desc_t) - sizeof(tusb_desc_interface_t),
            .dwClockFrequency   = UVC_CLOCK_FREQUENCY,
            .bInCollection      = 1,
            .baInterfaceNr      = { ITF_NUM_VIDEO_STREAMING },
        },
        .camera_terminal = {
            .bLength                   = sizeof(tusb_desc_video_control_camera_terminal_t),
            .bDescriptorType           = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType        = VIDEO_CS_ITF_VC_INPUT_TERMINAL,
            .bTerminalID               = UVC_ENTITY_CAP_INPUT_TERMINAL,
            .wTerminalType             = VIDEO_ITT_CAMERA,
            .bAssocTerminal            = 0,
            .iTerminal                 = 0,
            .wObjectiveFocalLengthMin  = 0,
            .wObjectiveFocalLengthMax  = 0,
            .wOcularFocalLength        = 0,
            .bControlSize              = 3,
            .bmControls                = { 0, 0, 0 },
        },
        .output_terminal = {
            .bLength            = sizeof(tusb_desc_video_control_output_terminal_t),
            .bDescriptorType    = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_OUTPUT_TERMINAL,
            .bTerminalID        = UVC_ENTITY_CAP_OUTPUT_TERMINAL,
            .wTerminalType      = VIDEO_TT_STREAMING,
            .bAssocTerminal     = 0,
            .bSourceID          = UVC_ENTITY_CAP_INPUT_TERMINAL,
            .iTerminal          = 0,
        },
    },

    .video_streaming = {
        .itf = {
            .bLength            = sizeof(tusb_desc_interface_t),
            .bDescriptorType    = TUSB_DESC_INTERFACE,
            .bInterfaceNumber   = ITF_NUM_VIDEO_STREAMING,
            .bAlternateSetting  = 0,
            .bNumEndpoints      = 0,   // iso: zero-bandwidth alt setting
            .bInterfaceClass    = TUSB_CLASS_VIDEO,
            .bInterfaceSubClass = VIDEO_SUBCLASS_STREAMING,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15,
            .iInterface         = STRID_UVC_STREAMING,
        },
        .header = {
            .bLength            = sizeof(tusb_desc_video_streaming_input_header_1byte_t),
            .bDescriptorType    = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_INPUT_HEADER,
            .bNumFormats        = 1,
            .wTotalLength       = sizeof(uvc_streaming_desc_t)
                                  - sizeof(tusb_desc_interface_t)   // itf (alt 0)
                                  - sizeof(tusb_desc_interface_t)   // itf_alt (alt 1)
                                  - sizeof(tusb_desc_endpoint_t),   // ep
            .bEndpointAddress   = EPNUM_UVC_IN,
            .bmInfo             = 0,
            .bTerminalLink      = UVC_ENTITY_CAP_OUTPUT_TERMINAL,
            .bStillCaptureMethod = 0,
            .bTriggerSupport    = 0,
            .bTriggerUsage      = 0,
            .bControlSize       = 1,
            .bmaControls        = { 0 },
        },
        .format = {
            .bLength              = sizeof(tusb_desc_video_format_mjpeg_t),
            .bDescriptorType      = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType   = VIDEO_CS_ITF_VS_FORMAT_MJPEG,
            .bFormatIndex         = 1,
            .bNumFrameDescriptors = 1,
            .bmFlags              = 0,
            .bDefaultFrameIndex   = 1,
            .bAspectRatioX        = 0,
            .bAspectRatioY        = 0,
            .bmInterlaceFlags     = 0,
            .bCopyProtect         = 0,
        },
        .frame = {
            .bLength                   = sizeof(tusb_desc_video_frame_mjpeg_1int_t),
            .bDescriptorType           = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType        = VIDEO_CS_ITF_VS_FRAME_MJPEG,
            .bFrameIndex               = 1,
            .bmCapabilities            = 0,
            .wWidth                    = UVC_FRAME_WIDTH,
            .wHeight                   = UVC_FRAME_HEIGHT,
            .dwMinBitRate              = UVC_FRAME_WIDTH * UVC_FRAME_HEIGHT * 8 * UVC_FRAME_RATE,
            .dwMaxBitRate              = UVC_FRAME_WIDTH * UVC_FRAME_HEIGHT * 16 * UVC_FRAME_RATE,
            .dwMaxVideoFrameBufferSize = UVC_FRAME_WIDTH * UVC_FRAME_HEIGHT * 2,
            .dwDefaultFrameInterval    = 10000000 / UVC_FRAME_RATE,
            // A single DISCRETE interval. Do not switch back to a continuous
            // range (bFrameIntervalType = 0) without checking that
            //   (dwMaxFrameInterval - dwMinFrameInterval) % dwFrameIntervalStep == 0
            // holds exactly: Windows' usbvideo.sys enforces it and fails the
            // whole configuration with Code 10 otherwise. 10000000/15 truncates
            // to 666666, which leaves a remainder of 10 against a 10000000 max.
            .bFrameIntervalType        = 1,   // one discrete interval
            .dwFrameInterval           = {
                10000000 / UVC_FRAME_RATE,
            },
        },
        .color = {
            .bLength                  = sizeof(tusb_desc_video_streaming_color_matching_t),
            .bDescriptorType          = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType       = VIDEO_CS_ITF_VS_COLORFORMAT,
            .bColorPrimaries          = VIDEO_COLOR_PRIMARIES_BT709,
            .bTransferCharacteristics = VIDEO_COLOR_XFER_CH_BT709,
            .bMatrixCoefficients      = VIDEO_COLOR_COEF_SMPTE170M,
        },
        .itf_alt = {
            .bLength            = sizeof(tusb_desc_interface_t),
            .bDescriptorType    = TUSB_DESC_INTERFACE,
            .bInterfaceNumber   = ITF_NUM_VIDEO_STREAMING,
            .bAlternateSetting  = 1,
            .bNumEndpoints      = 1,
            .bInterfaceClass    = TUSB_CLASS_VIDEO,
            .bInterfaceSubClass = VIDEO_SUBCLASS_STREAMING,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15,
            .iInterface         = STRID_UVC_STREAMING,
        },
        .ep = {
            .bLength          = sizeof(tusb_desc_endpoint_t),
            .bDescriptorType  = TUSB_DESC_ENDPOINT,
            .bEndpointAddress = EPNUM_UVC_IN,
            .bmAttributes     = {
                .xfer = TUSB_XFER_ISOCHRONOUS,
                .sync = 1,   // asynchronous
            },
            .wMaxPacketSize   = CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE,
            .bInterval        = 1,
        },
    },
};

const uint8_t *model_finetune_fs_config_descriptor = (const uint8_t *)&s_fs_config;

//--------------------------------------------------------------------+
// String descriptors
//--------------------------------------------------------------------+
const char *model_finetune_string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },   // 0: English (0x0409)
    "Espressif",                    // 1: Manufacturer
    "EECamp Model Finetune Camera", // 2: Product
    "123456",                       // 3: Serial
    "EECamp Control",               // 4: CDC
    "EECamp UVC Control",           // 5: UVC VideoControl
    "EECamp UVC Streaming",         // 6: UVC VideoStreaming
};

const int model_finetune_string_descriptor_count =
    (int)(sizeof(model_finetune_string_descriptors) / sizeof(model_finetune_string_descriptors[0]));
