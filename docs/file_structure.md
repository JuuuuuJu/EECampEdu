# File Structure

Legend:

| Tag | Meaning |
| --- | --- |
| `COMMON` | Shared production path; used by both test and real-camera integration |
| `TEST` | Test-only path for development without OV2640 hardware |
| `CAMERA` | Real-camera integration path; used when OV2640 is available |
| `MODEL_HANDOFF` | Contract files for the model-finetune team |
| `REFERENCE` | External reference only; do not edit original source folders directly |
| `LOCAL` | Local/generated files; usually not committed |

## Root

| Path | Tag | Purpose |
| --- | --- | --- |
| `README.md` | `COMMON` | Main setup, build, flash, benchmark, and runtime-mode SOP. |
| `.gitignore` | `COMMON` | Ignores generated build outputs, caches, venvs, and local artifacts. |

## Documentation

| Path | Tag | Purpose |
| --- | --- | --- |
| `docs/system_overview.md` | `COMMON` | System flow, team boundaries, and staged integration plan. |
| `docs/integration_contract.md` | `COMMON` | Cross-team model/image/flash/result contracts. |
| `docs/file_structure.md` | `COMMON` | This file; explains each file and whether it is test-only or camera-ready. |
| `interfaces/usb_protocol.md` | `COMMON` | USB/serial command draft for input-interface integration. |

## ESP Firmware

| Path | Tag | Purpose |
| --- | --- | --- |
| `esp/CMakeLists.txt` | `COMMON` | ESP-IDF project root CMake. |
| `esp/partitions.csv` | `COMMON` | Flash layout: firmware app, `model`, `photos`, and `storage`. |
| `esp/sdkconfig` | `COMMON` | Current ESP32-S3 build config. |
| `esp/sdkconfig.defaults` | `COMMON` | Default ESP32-S3 flash/PSRAM settings. |
| `esp/sdkconfig.defaults.esp32s3` | `COMMON` | Target-specific default ESP32-S3 settings. |
| `esp/dependencies.lock` | `COMMON` | ESP-IDF managed dependency lock file. |
| `esp/flash_tflite_model.py` | `COMMON` | Flashes a `.tflite` model into the `model` partition. |
| `esp/flash_photo.py` | `TEST` | Converts an image to a `160 x 160` grayscale photo blob and flashes it into `photos` to simulate camera capture. |
| `esp/README.md` | `COMMON` | ESP firmware-specific usage notes. |

## ESP Main Component

| Path | Tag | Purpose |
| --- | --- | --- |
| `esp/main/CMakeLists.txt` | `COMMON` | Registers firmware source files and include directory. |
| `esp/main/idf_component.yml` | `COMMON` | Pulls ESP-IDF managed dependencies such as TFLite Micro. |
| `esp/main/Separable_CNN_int8.tflite` | `COMMON` | Default stable model used for deploy testing. |
| `esp/main/Mini_ResNet_int8.tflite` | `COMMON` | Candidate compatible model. |
| `esp/main/MobileNetV1_0.25_int8.tflite` | `COMMON` | Candidate compatible model. |
| `esp/main/MobileNetV2_0.35_int8.tflite` | `COMMON` | Candidate compatible model; heavier than the default model. |

## ESP Headers

| Path | Tag | Purpose |
| --- | --- | --- |
| `esp/main/include/model_config.hpp` | `COMMON` | Central config: runtime mode, frame size, crop settings, tensor arena, partition labels. |
| `esp/main/include/camera_capture.hpp` | `CAMERA` | Camera capture interface implemented by the OV2640 module. |
| `esp/main/include/photo_storage.hpp` | `COMMON` | Photo partition read/write interface. Used by both fake-photo test and real camera flow. |

## ESP Source

| Path | Tag | Purpose |
| --- | --- | --- |
| `esp/main/src/app_main.cpp` | `COMMON` | TFLite model loading, preprocessing, inference, runtime-mode dispatch, UART test path, photo-flash test path, camera-flash path. |
| `esp/main/src/camera_capture_ov2640.cpp` | `CAMERA` | ESP-IDF OV2640 implementation ported from the provided Arduino `.ino` reference. |
| `esp/main/src/camera_capture_stub.cpp` | `TEST` | Buildable placeholder kept as fallback/reference; not compiled by current CMake. |
| `esp/main/src/photo_storage.cpp` | `COMMON` | Implements `photos` partition read/write. Used by `PHOTO_FLASH_TEST_MODE` and future camera capture. |

## Runtime Modes

| Mode | Tag | Hardware | Purpose |
| --- | --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | `TEST` | No camera | PC sends grayscale frames over UART; fastest deploy benchmark loop. |
| `RuntimeMode::kPhotoFlashTest` | `TEST` | No camera | Preload image into flash to simulate `camera -> flash -> inference`. |
| `RuntimeMode::kCameraFlash` | `CAMERA` | OV2640 required | Capture grayscale frame from OV2640, store to `photos`, resize into deploy frame, then run inference. |

## PC Tools

| Path | Tag | Purpose |
| --- | --- | --- |
| `pc/README.md` | `COMMON` | PC tools overview. |
| `pc/requirements.txt` | `COMMON` | Modern Python dependencies for deploy benchmark, image conversion, and flashing tools. |
| `pc/requirements-legacy-onnx.txt` | `TEST` | Legacy Python 3.7-3.10 ONNX debug dependencies. |
| `pc/benchmark/run_benchmark_png.py` | `TEST` | Sends dataset images to ESP in UART test mode and compares PC TFLite reference output. |
| `pc/benchmark/quant_sweep.py` | `MODEL_HANDOFF` | Quantization sweep / validation helper. |
| `pc/tools/*.py` | `TEST` | Debug tools for ONNX/TFLite/ESP output comparison. |

## Model Pipeline

| Path | Tag | Purpose |
| --- | --- | --- |
| `pc/model_pipeline/README.md` | `MODEL_HANDOFF` | Defines what model-finetune must export to deploy. |
| `pc/model_pipeline/class_mapping.example.json` | `MODEL_HANDOFF` | Expected class/action mapping format. |
| `pc/model_pipeline/preprocess_config.example.json` | `MODEL_HANDOFF` | Expected preprocessing contract format. |
| `pc/model_pipeline/quantization_report.example.json` | `MODEL_HANDOFF` | Expected quantization report format. |
| `pc/artifacts/models/` | `LOCAL` | Local `.tflite` and model metadata output. |
| `pc/artifacts/datasets/` | `LOCAL` | Local test/fine-tune datasets. |
| `pc/artifacts/reports/` | `LOCAL` | Local benchmark and quantization reports. |

## External References

| Path | Tag | Purpose |
| --- | --- | --- |
| `external/README.md` | `REFERENCE` | Lists source folders from other teams. |
| `external/camera/README.md` | `REFERENCE` | Notes how to port OV2640 code into `camera_capture`. |
| `external/input_interface/README.md` | `REFERENCE` | Notes how to port TinyUSB CDC/MSC command handling. |
| `external/model_finetune/README.md` | `REFERENCE` | Notes model candidates and `D:\tensorshit` source. |

## Scripts

| Path | Tag | Purpose |
| --- | --- | --- |
| `scripts/setup_pc_env.sh` | `COMMON` | Creates Python venv and installs PC requirements. |
| `scripts/build_firmware.sh` | `COMMON` | Runs ESP-IDF build. |
| `scripts/flash_model.sh` | `COMMON` | Flashes `.tflite` into the `model` partition. |
| `scripts/flash_photo.sh` | `TEST` | Flashes a test image into the `photos` partition. |
| `scripts/run_benchmark.sh` | `TEST` | Runs UART-frame benchmark. |
| `scripts/README.md` | `COMMON` | Script summary. |
