#pragma once

// Model-finetune camera firmware configuration.
// This firmware is for OV2640 preview/capture while students create datasets.
// It does not load a TFLite model and does not run inference.

constexpr const char *STORAGE_PARTITION_LABEL = "storage";
constexpr const char *PHOTOS_PARTITION_LABEL = "storage";
constexpr const char *USB_MSC_MOUNT_PATH = "/usb";

// Camera + USB CDC/MSC mode. CDC streams base64 image frames to the browser UI;
// MSC exposes the FAT storage partition when requested.
constexpr bool CAMERA_USB_CONTINUOUS_CAPTURE = true;
constexpr bool CAMERA_USB_KEEP_SEQUENCE = true;
constexpr int CAMERA_USB_CAPTURE_INTERVAL_MS = 250;

// Browser command mapping:
// f0=grayscale, f1=RGB565, f2=YUV422, f3=JPEG
// s0=96x96, s1=QQVGA, s2=QVGA, s3=VGA, s4=SVGA, s5=UXGA
constexpr int CAMERA_USB_DEFAULT_PIXEL_FORMAT = 3;
constexpr int CAMERA_USB_DEFAULT_FRAME_SIZE = 3;

// Optional rotary encoder / push-button input (shared with main_board).
// PCB allocation: GPIO21/47/48 are reserved for the rotary encoder.
// The firmware only *reports* encoder position over CDC (INPUT_CONTROL lines);
// mapping rotation to exposure is done by the browser/PC UI.
constexpr bool ENABLE_INPUT_CONTROLS = true;
constexpr int INPUT_ENCODER_CLK_GPIO = 21;
constexpr int INPUT_ENCODER_DT_GPIO = 47;
constexpr int INPUT_ENCODER_BUTTON_GPIO = 48;
constexpr int INPUT_BUTTON2_GPIO = 39;
constexpr int INPUT_DEBOUNCE_MS = 60;
// Mechanical detents usually take 4 quadrature steps; ignore extras shortly after a detent.
constexpr int INPUT_ENCODER_STEPS_PER_DETENT = 1;
constexpr int INPUT_ENCODER_DETENT_DEBOUNCE_MS = 40;