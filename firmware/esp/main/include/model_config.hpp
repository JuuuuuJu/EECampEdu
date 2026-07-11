#pragma once

// TFLite Micro gesture model configuration.
// The model team trains with grayscale 96x96x1 tensors. The firmware can
// receive a larger grayscale frame, crop the hand region on-device, resize it
// to 96x96, then quantize with the model-provided input scale/zero-point.

enum class RuntimeMode {
    kTestUartFrame,
    kPhotoFlashTest,
    kCameraFlash,
    kInputOutputSelfTest,
    kCameraUsbMsc,
};

// Keep TEST mode as the default integration mode. It lets the deploy pipeline
// run without an OV2640 by sending grayscale frames from the PC benchmark tool.
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kInputOutputSelfTest;

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
// Disabled by default because the prototype GPIOs from sketch_dec17a.ino
// conflict with the current OV2640 wiring: 4=SDA, 5=SCL, 15=XCLK, 18=Y7.
constexpr bool ENABLE_INPUT_CONTROLS = true;
constexpr int INPUT_ENCODER_CLK_GPIO = 5;
constexpr int INPUT_ENCODER_DT_GPIO = 4;
constexpr int INPUT_ENCODER_BUTTON_GPIO = 21;
constexpr int INPUT_BUTTON2_GPIO = 18;
constexpr int INPUT_DEBOUNCE_MS = 60;

// Input/output unit-test mode. This mode does not require a flashed TFLite model.
constexpr int IO_SELF_TEST_INTERVAL_MS = 1000;

// Robot arm output from robotic_arm.ino, ported to ESP-IDF LEDC. Disabled by
// default because ROBOT_ARM_BASE_GPIO=18 conflicts with OV2640 Y7 on the
// current camera wiring.
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = false;
constexpr int ROBOT_ARM_BASE_GPIO = 18;
constexpr int ROBOT_ARM_ARM_GPIO = 19;
constexpr int ROBOT_ARM_PITCH_GPIO = 22;
constexpr int ROBOT_ARM_CLAW_GPIO = 21;
constexpr int ROBOT_ARM_BASE_INITIAL_DEG = 90;
constexpr int ROBOT_ARM_ARM_INITIAL_DEG = 90;
constexpr int ROBOT_ARM_PITCH_INITIAL_DEG = 90;
constexpr int ROBOT_ARM_CLAW_INITIAL_DEG = 30;
constexpr int ROBOT_ARM_PITCH_MIN_DEG = 30;
constexpr int ROBOT_ARM_PITCH_MAX_DEG = 125;
constexpr int ROBOT_ARM_STEP_DEG = 5;
constexpr int ROBOT_ARM_SERVO_MIN_US = 500;
constexpr int ROBOT_ARM_SERVO_MAX_US = 2500;
constexpr int ROBOT_ARM_SERVO_FREQ_HZ = 50;
constexpr int ROBOT_ARM_SERVO_DUTY_BITS = 16;

// Integration default: keep the tensor arena on PSRAM so all supported model
// candidates use the same memory path. Set true only for small-model speed tests.
constexpr bool PREFER_INTERNAL_TENSOR_ARENA = false;
constexpr int TENSOR_ARENA_SIZE = 800 * 1024;
constexpr int FALLBACK_TENSOR_ARENA_SIZE = 256 * 1024;
constexpr const char *MODEL_PARTITION_LABEL = "model";
constexpr const char *STORAGE_PARTITION_LABEL = "storage";
constexpr const char *USB_MSC_MOUNT_PATH = "/usb";

// Camera + USB CDC/MSC integration mode. CDC can stream base64 frames to the
// PC camera controller, and MSC exposes the FAT /usb storage partition. The
// current camera_usb storage command writes latest.raw/latest.meta/latest.bmp.
constexpr bool CAMERA_USB_CONTINUOUS_CAPTURE = true;
constexpr bool CAMERA_USB_KEEP_SEQUENCE = true;
constexpr int CAMERA_USB_CAPTURE_INTERVAL_MS = 250;
