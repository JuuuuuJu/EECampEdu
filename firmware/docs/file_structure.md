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
| `docs/test_plan.md` | `COMMON` | Unit-test and full-integration test SOP, expected logs, and pass criteria. |
| `interfaces/usb_protocol.md` | `COMMON` | USB/serial command draft for input-interface integration. |

## ESP Firmware

| Path | Tag | Purpose |
| --- | --- | --- |
| `esp/CMakeLists.txt` | `COMMON` | ESP-IDF project root CMake. |
| `esp/partitions.csv` | `COMMON` | Flash layout: firmware app, `model`, and one large FAT `storage` partition for USB MSC camera files. |
| `esp/sdkconfig` | `COMMON` | Current ESP32-S3 build config. |
| `esp/sdkconfig.defaults` | `COMMON` | Default ESP32-S3 flash/PSRAM settings. |
| `esp/sdkconfig.defaults.esp32s3` | `COMMON` | Target-specific default ESP32-S3 settings. |
| `esp/dependencies.lock` | `COMMON` | ESP-IDF managed dependency lock file. |
| `esp/flash_tflite_model.py` | `COMMON` | Flashes a `.tflite` model into the `model` partition. |
| `esp/flash_photo.py` | `TEST` | Legacy helper for the old raw photo-flash path. USB MSC camera mode writes JPEG files directly into FAT storage instead. |
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
| `esp/main/include/input_controls.hpp` | `COMMON` | Optional rotary encoder / push-button input interface from the input-interface team. Disabled by default until GPIO conflicts are resolved. |
| `esp/main/include/output_controls.hpp` | `COMMON` | Optional robotic-arm output interface ported from `robotic_arm.ino`; disabled until servo GPIO conflicts are resolved. |
| `esp/main/include/photo_storage.hpp` | `TEST` | Legacy raw photo test interface. Not used by USB MSC camera mode. |
| `esp/main/include/usb_composite.hpp` | `COMMON` | TinyUSB CDC + MSC interface. CDC supports commands and base64 image streaming; MSC exposes FAT storage to the PC UI. |

## ESP Source

| Path | Tag | Purpose |
| --- | --- | --- |
| `esp/main/src/app_main.cpp` | `COMMON` | TFLite model loading, preprocessing, inference, runtime-mode dispatch, UART test path, photo-flash test path, camera-flash path. |
| `esp/main/src/camera_capture_ov2640.cpp` | `CAMERA` | ESP-IDF OV2640 implementation ported from the provided Arduino `.ino` reference. |
| `esp/main/src/camera_capture_stub.cpp` | `TEST` | Buildable placeholder kept as fallback/reference; not compiled by current CMake. |
| `esp/main/src/input_controls.cpp` | `COMMON` | ESP-IDF GPIO/interrupt implementation for the rotary encoder and push button prototype. Current prototype pins conflict with OV2640, so this is compiled but disabled by config. |
| `esp/main/src/output_controls.cpp` | `COMMON` | ESP-IDF LEDC servo output module for robotic-arm movement; compiled but disabled by config until pins are finalized. |
| `esp/main/src/photo_storage.cpp` | `TEST` | Stores camera_usb latest.raw/latest.meta/latest.bmp files under the mounted FAT `/usb` path. |
| `esp/main/src/usb_composite.cpp` | `COMMON` | TinyUSB composite device implementation for CDC commands and USB MSC storage. |

## Runtime Modes

| Mode | Tag | Hardware | Purpose |
| --- | --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | `TEST` | No camera | PC sends grayscale frames over UART; fastest deploy benchmark loop. |
| `RuntimeMode::kPhotoFlashTest` | `TEST` | No camera | Preload image into flash to simulate `camera -> flash -> inference`. |
| `RuntimeMode::kCameraFlash` | `CAMERA` | OV2640 required | Legacy camera-to-flash inference path for grayscale frames. |
| `RuntimeMode::kInputOutputSelfTest` | `TEST` | Input/output hardware optional | Logs input snapshot and cycles output actions without model/camera. |
| `RuntimeMode::kCameraUsbMsc` | `CAMERA` | OV2640 + USB required | Run the camera_usb CDC live-preview / MSC storage integration path. |

## PC Tools

| Path | Tag | Purpose |
| --- | --- | --- |
| `pc/README.md` | `COMMON` | PC tools overview. |
| `pc/requirements.txt` | `COMMON` | Single Python dependency file for deploy benchmark, image conversion, flashing tools, and PC TFLite reference inference. |
| `apps/esp32_cam_input_app/` | `COMMON` | Input-interface Dear ImGui + SDL3 desktop app skeleton for camera preview and control UI. |
| `pc/benchmark/run_benchmark_png.py` | `TEST` | Sends dataset images to ESP in UART test mode and compares PC TFLite reference output. |
| `pc/benchmark/quant_sweep.py` | `MODEL_HANDOFF` | Quantization sweep / validation helper. |
| `pc/tools/quantize_keras_model.py` | `MODEL_HANDOFF` | Reads `.keras` source models from `../model_finetune/models`, runs representative calibration, and exports int8 deploy `.tflite` plus a quantization report. |
| `pc/tools/camera_controller.py` | `CAMERA` | PC-side controller from the camera_usb integration; receives CDC base64 image frames and sends camera commands. |
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
| `external/input_interface/README.md` | `REFERENCE` | Notes input-interface sources, including the Dear ImGui + SDL3 app and TinyUSB CDC command target. |
| `external/model_finetune/README.md` | `REFERENCE` | Notes model candidates and the model-finetune source contract. |
| `external/output/robotic_arm.ino` | `REFERENCE` | Original Arduino robotic-arm servo control sketch. |

## Scripts

| Path | Tag | Purpose |
| --- | --- | --- |
| `scripts/setup_pc_env.py` | `COMMON` | Creates/updates the Windows conda environment and installs PC requirements. |
| `esp/flash_tflite_model.py` | `COMMON` | Flashes `.tflite` into the `model` partition. |
| `esp/flash_photo.py` | `TEST` | Legacy raw photo flash helper. |
| `pc/benchmark/run_benchmark_png.py` | `TEST` | Runs UART-frame benchmark over a Windows COM port. |
| `scripts/README.md` | `COMMON` | Script summary. |


