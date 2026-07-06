#pragma once

// TFLite Micro gesture model configuration.
// The model team trains with grayscale 96x96x1 tensors. The firmware can
// receive a larger grayscale frame, crop the hand region on-device, resize it
// to 96x96, then quantize with the model-provided input scale/zero-point.

enum class RuntimeMode {
    kTestUartFrame,
    kPhotoFlashTest,
    kCameraFlash,
};

// Keep TEST mode as the default integration mode. It lets the deploy pipeline
// run without an OV2640 by sending grayscale frames from the PC benchmark tool.
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;

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
constexpr bool ENABLE_INPUT_CONTROLS = false;
constexpr int INPUT_ENCODER_CLK_GPIO = 5;
constexpr int INPUT_ENCODER_DT_GPIO = 4;
constexpr int INPUT_ENCODER_BUTTON_GPIO = 21;
constexpr int INPUT_BUTTON2_GPIO = 18;
constexpr int INPUT_DEBOUNCE_MS = 60;

// Separable_CNN fits in internal RAM and runs much faster there. Set this false
// for larger models that require the full PSRAM tensor arena.
constexpr bool PREFER_INTERNAL_TENSOR_ARENA = false;
constexpr int TENSOR_ARENA_SIZE = 800 * 1024;
constexpr int FALLBACK_TENSOR_ARENA_SIZE = 256 * 1024;
constexpr const char *MODEL_PARTITION_LABEL = "model";
constexpr const char *PHOTOS_PARTITION_LABEL = "photos";
