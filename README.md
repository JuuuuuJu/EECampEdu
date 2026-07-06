# EECampEdu Integrated Firmware

This repository is the integration workspace for the gesture-controlled robotic
arm system.

## System Flow

```text
ESP32-S3 camera
  -> photo flash partition
  -> on-device grayscale / crop / resize / scale
  -> int8 TFLite Micro gesture model
  -> gesture result
  -> robotic arm motion
```

PC is used for model fine-tuning, dataset creation, quantization, flashing, and
benchmarking. ESP32-S3 is used for camera capture, local preprocessing, model
inference, USB communication, and final motion output.

## Current Integration Decision

- Hand crop is performed on ESP32-S3.
- Class order is fixed: `up`, `down`, `right`, `left`, `null`.
- The stable baseline model is `Separable_CNN_int8.tflite`.
- Model input contract is `96 x 96 x 1 int8`.
- ESP32-S3 CPU frequency must be `240 MHz` for benchmark parity with the
  standalone deploy repo.
- The default Separable CNN keeps tensor arena and frame buffers in internal RAM
  for speed. Larger models can switch to PSRAM by setting
  `PREFER_INTERNAL_TENSOR_ARENA=false`.
- The camera team code supports `GRAYSCALE`, `RGB565`, `YUV422`, and `JPEG`.
- The deploy path should first support grayscale capture, then add JPEG decode
  if higher-resolution camera storage is needed.
- Default firmware mode is `TEST_MODE_UART_FRAME`, so deployment can be tested
  without an OV2640 camera.
- OV2640 integration is available in `esp/main/src/camera_capture_ov2640.cpp`;
  the current real-camera path captures grayscale QQVGA and resizes it before
  inference.
- Rotary encoder / push-button input from the input-interface team is ported as
  an optional ESP-IDF GPIO module. It is disabled by default because the
  prototype pins conflict with the current OV2640 wiring.

## Repository Layout

```text
docs/             Integration notes and architecture decisions
esp/              ESP-IDF firmware and model flashing tools
interfaces/       Cross-team contracts and command protocols
pc/               PC-side benchmark, validation, and setup tools
external/         References to external team code
scripts/          Convenience scripts for setup/build/flash/test
```

For a per-file explanation with `TEST`, `CAMERA`, `COMMON`, and
`MODEL_HANDOFF` tags, see `docs/file_structure.md`.

## Environment Setup

### PC Python Environment

Use the AI PC or WSL environment for benchmark, image flashing tools, model
validation, and future fine-tuning automation.

```bash
cd /mnt/d/EECampEdu
bash scripts/setup_pc_env.sh
source .venv/bin/activate
```

This installs the single PC dependency file, `pc/requirements.txt`, including
`Pillow`, `pyserial`, `esptool`, and `tensorflow` for PC-side TFLite reference
inference during output-similarity benchmark.

### ESP-IDF Environment

Use ESP-IDF v5.x with ESP32-S3 target support.

```bash
cd /mnt/d/EECampEdu/esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
```

The firmware expects a 16 MB ESP32-S3 board. PSRAM is enabled in
`esp/sdkconfig`, `esp/sdkconfig.defaults`, and `esp/sdkconfig.defaults.esp32s3`.
CPU frequency is set to 240 MHz for benchmark parity with the standalone deploy
repo. The default Separable CNN keeps its tensor arena and frame buffers in
internal RAM for speed; set `PREFER_INTERNAL_TENSOR_ARENA=false` in
`esp/main/include/model_config.hpp` when testing larger models that require the
full PSRAM tensor arena.

No `git clone --recursive` is required. ESP-IDF downloads managed components
from `esp/main/idf_component.yml`, currently `esp32-camera` and
`esp-tflite-micro`.

## Runtime Modes

Runtime mode is selected in `esp/main/include/model_config.hpp`.

| Mode | Purpose | Hardware needed |
| --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | PC sends grayscale frames over UART; current default benchmark path | no camera |
| `RuntimeMode::kPhotoFlashTest` | Preload one image into `photos` flash partition and run inference from flash | no camera |
| `RuntimeMode::kCameraFlash` | Capture from OV2640, store to flash, then infer | OV2640 required |

## Build Firmware

```bash
cd esp
idf.py fullclean
idf.py build flash monitor
```

## Flash TFLite Model Only

```bash
cd esp
python flash_tflite_model.py ../pc/artifacts/models/Separable_CNN_int8.tflite -p COM6
```

## Simulate Camera Photo In Flash

When OV2640 hardware is not available, preload an image into the `photos`
partition. The tool converts the image to `160 x 160 x 1` grayscale and writes
the same photo header format used by firmware.

```bash
cd esp
python flash_photo.py path/to/test_image.jpg -p COM6
```

Then set this in `esp/main/include/model_config.hpp`:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kPhotoFlashTest;
```

Rebuild and flash firmware. On boot, ESP32-S3 reads the preloaded photo from
flash and runs one inference pass.

## Benchmark From PC

```bash
cd pc
python benchmark/run_benchmark_png.py --model artifacts/models/Separable_CNN_int8.tflite --dataset artifacts/datasets
```

This benchmark talks to firmware running in `TEST_MODE_UART_FRAME`: PC sends a
`160 x 160 x 1` grayscale frame, ESP32-S3 performs crop/resize/quantization, and
then returns TFLite Micro inference results.

Expected baseline after the performance fix:

```text
Model              : Separable_CNN_int8.tflite
CPU frequency      : 240 MHz
Tensor arena       : internal RAM preferred
Input frame        : 160 x 160 x 1 grayscale
Model input        : 96 x 96 x 1 int8
Model-only speed   : about 9 FPS
Device compute     : about 8 FPS including ESP-side crop/resize/quantization
End-to-end UART    : about 0.4 FPS because raw frames are streamed over 115200 baud UART
```

If model-only throughput drops to around `5-6 FPS`, check that the firmware was
rebuilt with `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`. A `160 MHz` build produces
roughly the same slowdown ratio.

## Model Fine-Tune Handoff

The model-finetune team can build a foundation-model workflow on the AMD AI PC,
but deploy expects fixed exported artifacts:

```text
pc/artifacts/models/<model_name>.tflite
pc/artifacts/models/<model_name>.class_mapping.json
pc/artifacts/models/<model_name>.preprocess_config.json
pc/artifacts/reports/<model_name>.quantization_report.json
```

Examples and the expected handoff contract are in `pc/model_pipeline/`.

See `docs/integration_contract.md` before changing model format, camera format,
USB messages, or flash partition layout.
