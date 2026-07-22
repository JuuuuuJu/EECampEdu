#pragma once

// TFLite Micro gesture model configuration.
// The default Mini ResNet deploy model uses grayscale 96x96x1 tensors. The firmware can
// receive a larger grayscale frame, crop the hand region on-device, resize it
// to the model input size, then quantize with the model-provided input scale/zero-point.

enum class RuntimeMode {
    kTestUartFrame,
    kPhotoFlashTest,
    kCameraFlash,
    kInputOutputSelfTest,
    kCameraUsbMsc,
};

// Integration default: run OV2640 + USB CDC/MSC so the ImGui app can show live preview.
// Switch to kTestUartFrame for deploy benchmark without OV2640.
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;

constexpr int INPUT_HEIGHT = 96;
constexpr int INPUT_WIDTH = 96;
constexpr int INPUT_CHANNEL = 1;

constexpr int FRAME_HEIGHT = 160;
constexpr int FRAME_WIDTH = 160;
constexpr int FRAME_CHANNEL = 1;

constexpr bool ENABLE_HAND_CROP = true;
constexpr int HAND_CROP_MARGIN_PERCENT = 18;
constexpr int HAND_CROP_MIN_AREA_PERCENT = 1;
constexpr int HAND_CROP_DARK_DELTA = 25;
constexpr int HAND_CROP_MIN_THRESHOLD = 70;
constexpr int HAND_CROP_MAX_THRESHOLD = 205;

// Optional rotary encoder / push-button input from the input-interface team.
// PCB allocation: GPIO21/47/48 are reserved for the rotary encoder.
constexpr bool ENABLE_INPUT_CONTROLS = true;
constexpr int INPUT_ENCODER_CLK_GPIO = 21;
constexpr int INPUT_ENCODER_DT_GPIO = 47;
constexpr int INPUT_ENCODER_BUTTON_GPIO = 48;
constexpr int INPUT_BUTTON2_GPIO = -1;
constexpr int INPUT_DEBOUNCE_MS = 60;
// Mechanical detents usually take 4 quadrature steps; ignore extras shortly after a detent.
constexpr int INPUT_ENCODER_STEPS_PER_DETENT = 2;
constexpr int INPUT_ENCODER_DETENT_DEBOUNCE_MS = 40;

// Input/output unit-test mode. This mode does not require a flashed TFLite model.
constexpr int IO_SELF_TEST_INTERVAL_MS = 1000;

// Robot arm output is now owned by control board over a second USB serial link.
// main board only reports inference results to the PC; the PC forwards gestures to control board.

// Integration default: keep the tensor arena on PSRAM so all supported model
// candidates use the same memory path. Set true only for small-model speed tests.
constexpr bool PREFER_INTERNAL_TENSOR_ARENA = false;
constexpr int TENSOR_ARENA_SIZE = 1536 * 1024;
constexpr int FALLBACK_TENSOR_ARENA_SIZE = 256 * 1024;
constexpr const char *MODEL_PARTITION_LABEL = "model";
constexpr const char *STORAGE_PARTITION_LABEL = "storage";
constexpr const char *PHOTOS_PARTITION_LABEL = "storage";
constexpr const char *USB_MSC_MOUNT_PATH = "/usb";

// Camera + USB CDC/MSC integration mode. CDC can stream base64 frames to the
// PC camera controller, and MSC exposes the FAT /usb storage partition. The
// current camera_usb storage command writes latest.raw/latest.meta/latest.bmp.
constexpr bool CAMERA_USB_CONTINUOUS_CAPTURE = true;
constexpr bool CAMERA_USB_KEEP_SEQUENCE = true;
constexpr int CAMERA_USB_CAPTURE_INTERVAL_MS = 250;
constexpr int CAMERA_USB_DEFAULT_PIXEL_FORMAT = 3; // 0=grayscale, 1=RGB565, 2=YUV422, 3=JPEG
constexpr int CAMERA_USB_DEFAULT_FRAME_SIZE = 3;   // 0=96x96, 1=QQVGA, 2=QVGA, 3=VGA
