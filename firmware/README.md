# EECampEdu Integrated Firmware

This repository is the integration workspace for the gesture-controlled robotic
arm system.

## System Flow

```text
ESP32-S3 camera
  -> FAT storage partition / USB MSC drive
  -> on-device grayscale / crop / resize / scale
  -> int8 TFLite Micro gesture model
  -> gesture result
  -> robotic arm motion
```

PC is used for model fine-tuning, dataset creation, quantization, flashing, and
benchmarking. ESP32-S3 is used for camera capture, local preprocessing, model
inference, USB communication, and final motion output.

This integration branch is Windows-first. PC tools are installed through conda
and run as Python commands from PowerShell.

## Current Integration Decision

- Hand crop is performed on ESP32-S3.
- Class order is fixed: `up`, `down`, `right`, `left`, `null`.
- The stable baseline source model is `../model_finetune/models/Separable_CNN.keras`.
- Deploy quantizes `.keras` source models into flashable int8 TFLite models.
- Model input contract is `96 x 96 x 1 int8`.
- ESP32-S3 CPU frequency must be `240 MHz` for benchmark parity with the
  standalone deploy repo.
- PSRAM is enabled by default. The tensor arena uses PSRAM unless
  `PREFER_INTERNAL_TENSOR_ARENA` is explicitly changed.
- Camera photos are written into one large FAT `storage` partition, which is
  exposed to the PC as a USB MSC drive in camera USB mode.
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

Use Windows PowerShell with conda for benchmark, image flashing tools, model
validation, and future fine-tuning automation.

```powershell
cd EECampEdu
python firmware\scripts\setup_pc_env.py
conda activate eecampedu
```

This installs the single PC dependency file, `pc/requirements.txt`, including
`Pillow`, `pyserial`, `esptool`, and `tensorflow` for PC-side TFLite reference
inference during output-similarity benchmark. The default conda environment uses
Python 3.10 for native Windows TensorFlow compatibility.

### ESP-IDF Environment

Use ESP-IDF v5.x with ESP32-S3 target support.

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
```

The firmware expects a 16 MB ESP32-S3 board. PSRAM is enabled in
`esp/sdkconfig`, `esp/sdkconfig.defaults`, and `esp/sdkconfig.defaults.esp32s3`.
CPU frequency is set to 240 MHz for benchmark parity with the standalone deploy
repo. `PREFER_INTERNAL_TENSOR_ARENA=false` keeps the default tensor arena path
on PSRAM.

No `git clone --recursive` is required. ESP-IDF downloads managed components
from `esp/main/idf_component.yml`, currently `esp32-camera` and
`esp-tflite-micro`.

## Runtime Modes

Runtime mode is selected in `esp/main/include/model_config.hpp`.

| Mode | Purpose | Hardware needed |
| --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | PC sends grayscale frames over UART; current default benchmark path | no camera |
| `RuntimeMode::kPhotoFlashTest` | Legacy raw-photo test path for one preloaded grayscale image | no camera |
| `RuntimeMode::kCameraFlash` | Capture from OV2640, store to flash, then infer | OV2640 required |
| `RuntimeMode::kInputOutputSelfTest` | Logs input state and cycles output actions without model/camera | input/output hardware optional |
| `RuntimeMode::kCameraUsbMsc` | Camera USB CDC live preview plus USB MSC storage | OV2640 + USB |

Full module and integration test procedures are documented in `docs/test_plan.md`.

## Build Firmware

Build, flash in `esp/`.

## Camera + USB CDC/MSC Mode

Set this in `esp/main/include/model_config.hpp`:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

The USB camera path now follows `0711_integration/camera_usb/EECampEdu` as the
source of truth. ESP32-S3 initializes TinyUSB CDC + MSC and creates FreeRTOS
camera / command tasks with task handles.

Supported paths:

```text
CDC live preview  : ESP sends base64 image frames to firmware/pc/tools/camera_controller.py
USB MSC storage   : ESP exposes the FAT storage partition as a USB drive
On-device infer   : grayscale captures can be cropped/resized and sent to TFLite Micro
```

Main CDC commands inherited from the camera_usb integration:

```text
C / c        capture one frame and stream it over CDC
D / d 0|1    disable/enable continuous CDC streaming
W / w        capture and write latest.raw/latest.meta/latest.bmp into /usb
L / l        list files in /usb
K / k        clear latest.raw/latest.meta/latest.bmp
I / i        run inference from the latest stored grayscale frame
F / f <n>    set pixel format: 0 gray, 1 RGB565, 2 YUV422, 3 JPEG
S / s <n>    set frame size: 0 96x96, 1 QQVGA, 2 QVGA, 3 VGA, 4 SVGA, 5 UXGA
usb          expose FAT storage to the host PC
format       format FAT storage and reboot
```

CDC streaming is useful for live preview and UI debugging. USB MSC is useful
when the PC app wants to inspect or load files from ESP storage. This is not a
true UVC webcam protocol.

## Flash TFLite Model Only

```powershell
cd EECampEdu
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\Separable_CNN_int8.tflite" -p COM6
```

The Python flashing helpers call `python -m esptool`, which is more reliable
than invoking `esptool.py` directly on Windows conda environments.

## Simulate Camera Photo In Flash

When OV2640 hardware is not available, the older photo-flash test path can
preload an image into flash. This is now a legacy deploy test path and is
separate from USB MSC camera storage.

```powershell
cd EECampEdu
python firmware\esp\flash_photo.py "path\to\test_image.jpg" -p COM6
```

Then set this in `esp/main/include/model_config.hpp`:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kPhotoFlashTest;
```

Rebuild and flash firmware. On boot, ESP32-S3 reads the preloaded photo from
flash and runs one inference pass.

## Benchmark From PC

```powershell
cd firmware\pc
$env:BENCHMARK_PORT="COM6"
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Separable_CNN_int8.tflite" --dataset "..\..\model_finetune\dataset\validation"
```

This benchmark talks to firmware running in `TEST_MODE_UART_FRAME`: PC sends a
`160 x 160 x 1` grayscale frame, ESP32-S3 performs crop/resize/quantization, and
then returns TFLite Micro inference results.

Baseline reporting fields:

```text
Model              : Separable_CNN_int8.tflite
CPU frequency      : 240 MHz
Tensor arena       : PSRAM by default
Input frame        : 160 x 160 x 1 grayscale
Model input        : 96 x 96 x 1 int8
Primary metrics    : model latency, device compute throughput, label accuracy, output similarity
UART note          : end-to-end UART throughput is dominated by raw frame transfer at 115200 baud
```

If model-only throughput drops to around `5-6 FPS`, check that the firmware was
rebuilt with `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`. A `160 MHz` build produces
roughly the same slowdown ratio.

## Model Fine-Tune Handoff

The model-finetune team can build a foundation-model workflow on the AMD AI PC,
but deploy expects `.keras` source models and generates fixed exported
artifacts:

```text
model_finetune/models/<model_name>.keras
firmware/pc/artifacts/models/<model_name>_int8.tflite
firmware/pc/artifacts/models/<model_name>.class_mapping.json
firmware/pc/artifacts/models/<model_name>.preprocess_config.json
firmware/pc/artifacts/reports/<model_name>.quantization_report.json
```

Generate the deploy model:

```powershell
cd EECampEdu
python firmware\pc\tools\quantize_keras_model.py --model-name Separable_CNN
```

The current deploy pipeline intentionally ignores float32 `.tflite` as a
quantization source.

Examples and the expected handoff contract are in `pc/model_pipeline/`.

See `docs/integration_contract.md` before changing model format, camera format,
USB messages, or flash partition layout.
