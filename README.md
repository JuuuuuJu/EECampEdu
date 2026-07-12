# EECampEdu

EECampEdu is the integrated workspace for the gesture-controlled robotic arm system. The project is split by team ownership but keeps one deployable ESP32-S3 firmware path.

Core deploy contract:

```text
Source framework can be PyTorch or TensorFlow.
Deploy target is unified as int8 TFLite for ESP32-S3 TFLite Micro.
```

```text
EECampEdu/
  model_finetune/   Model training, fine-tuning, datasets, and source model handoff.
  firmware/         ESP32-S3 firmware, deploy quantization, flashing, benchmark, camera, USB, input, output.
  apps/             Windows PC UI prototypes and integration tools.
  docs/unit_tests/  Unit-test guides for the six teams.
```

All paths are relative to this repository root. Do not hardcode local drive letters.

## System Flow

```text
Model team
  -> train / fine-tune in PyTorch or TensorFlow
  -> hand off .pth/.onnx/.keras source artifacts under model_finetune/models/

Deploy team
  -> normalize source handoff into int8 TFLite
  -> flash ESP32-S3 model partition
  -> benchmark accuracy, latency, throughput, output similarity

ESP32-S3
  -> OV2640 capture
  -> grayscale / crop / resize / scale
  -> on-device inference
  -> gesture result
  -> robotic arm output

PC App
  -> USB CDC live preview / commands
  -> UI controls for capture, zoom, exposure, ISO, and integration debugging
```

## Team Responsibilities

| Team | Main folder | Main responsibility |
| --- | --- | --- |
| Model | `model_finetune/` | Train/fine-tune gesture models and provide source models. |
| Deploy | `firmware/` | Convert PyTorch/TensorFlow model handoff into int8 TFLite deploy artifacts, flash, benchmark, and validate output similarity. |
| Camera | `firmware/esp/main/src/camera_capture_ov2640.cpp` | OV2640 capture and frame format/resolution control. |
| USB | `firmware/esp/main/src/usb_composite.cpp` | USB CDC command stream and USB MSC storage exposure. |
| Input | `apps/`, `firmware/esp/main/src/input_controls.cpp` | PC UI, rotary encoder, button controls, camera parameters. |
| Output | `firmware/esp/main/src/output_controls.cpp` | Robotic arm servo command mapping and movement. |

## Windows Environment

Use PowerShell with conda:

```powershell
cd EECampEdu
python scripts\setup_env.py
conda activate eecampedu
```

The setup script creates the `eecampedu` conda environment, installs Python deploy dependencies, and installs native packages needed by the ImGui/SDL3 PC app.

ESP-IDF is still installed separately. Use ESP-IDF v5.x with ESP32-S3 support.

## Firmware Build

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

The firmware expects an ESP32-S3 board with 16 MB flash and PSRAM. Runtime mode is selected in:

```text
firmware/esp/main/include/model_config.hpp
```

## Runtime Modes

| Mode | Purpose | Hardware |
| --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | Deploy benchmark from PC dataset over UART. | ESP32-S3 only |
| `RuntimeMode::kPhotoFlashTest` | Preloaded-photo flash test without camera. | ESP32-S3 only |
| `RuntimeMode::kCameraFlash` | OV2640 capture, store to flash, grayscale preprocess, inference. | OV2640 |
| `RuntimeMode::kInputOutputSelfTest` | Input state logging and output servo cycle. | optional input/output hardware |
| `RuntimeMode::kCameraUsbMsc` | USB CDC live preview, USB MSC storage, camera/control integration. | OV2640 + USB |

## Flash Layout

```text
nvs      24K
phy_init 4K
factory  3M
model    1M      int8 TFLite model partition
storage  remaining FAT storage for camera/USB files
```

The model partition is intentionally independent from the firmware app, so the deploy model can be reflashed without rebuilding firmware.

## Unit Tests

Each team has a dedicated guide under `docs/unit_tests/`:

```text
docs/unit_tests/model_unit_test.md
docs/unit_tests/deploy_unit_test.md
docs/unit_tests/camera_unit_test.md
docs/unit_tests/usb_unit_test.md
docs/unit_tests/input_unit_test.md
docs/unit_tests/output_unit_test.md
```

Use one mode at a time for unit tests. Use `RuntimeMode::kCameraUsbMsc` only for full camera + USB + UI integration.
