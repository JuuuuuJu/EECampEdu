# EECampEdu

EECampEdu is the integrated workspace for the gesture-controlled robotic arm system.

Current integration architecture:

```text
Model source: PyTorch or TensorFlow
Deploy target: unified int8 TFLite
Inference board: ESP1, ESP32-S3
Output board: ESP2, normal ESP32
PC role: live preview, benchmark, command bridge, and UI
```

ESP1 no longer drives servos directly. ESP1 runs camera/model inference and prints `RESULT,...`; the PC forwards the predicted gesture to ESP2 over a second USB serial link; ESP2 controls the robotic arm servos.

## Repository Layout

```text
EECampEdu/
  model_finetune/   Model training, fine-tuning, datasets, and source model handoff.
  firmware/         ESP1 firmware, ESP2 output firmware, quantization, flashing, benchmark, camera, USB.
  apps/             Windows Dear ImGui / SDL3 integration app.
  docs/unit_tests/  Unit-test guides for model, deploy, camera, USB, input, and output teams.
```

All paths are relative to this repository root. Do not hardcode local drive letters in committed scripts.

## System Flow

```text
Model team
  -> train / fine-tune in PyTorch or TensorFlow
  -> export source artifacts under model_finetune/models/

Deploy team
  -> quantize/calibrate source model into int8 TFLite
  -> flash ESP1 model partition
  -> validate accuracy, latency, throughput, and output similarity

ESP1 ESP32-S3
  -> OV2640 capture
  -> grayscale / crop / resize / scale
  -> int8 TFLite Micro inference
  -> print RESULT,<class>,<model_us>,<preprocess_us>,<device_us>,<scores...>

PC
  -> receives ESP1 result
  -> benchmark and/or UI live preview
  -> maps each gesture class to a user-selected output action

ESP2 normal ESP32
  -> receives ACTION,up/down/left/right/clamp/release/none over second USB serial
  -> drives robotic arm servos
```

## Team Responsibilities

| Team | Main folder | Main responsibility |
| --- | --- | --- |
| Model | `model_finetune/` | Train/fine-tune gesture models and provide source artifacts. |
| Deploy | `firmware/` | Convert PyTorch/TensorFlow handoff into int8 TFLite, flash, benchmark, and validate ESP behavior. |
| Camera | `firmware/esp/main/src/camera_capture_ov2640.cpp` | OV2640 capture and frame format/resolution control on ESP1. |
| USB | `firmware/esp/main/src/usb_composite.cpp` | ESP1 USB CDC command stream and USB MSC storage exposure. |
| Input | `apps/`, `firmware/esp/main/src/input_controls.cpp` | PC UI, rotary encoder/button controls, camera parameters. |
| Output | `firmware/esp2_output/`, `firmware/pc/tools/send_esp2_gesture.py` | ESP2 ESP-IDF servo control and PC-to-ESP2 gesture bridge. |

## Windows Environment

Use PowerShell with conda:

```powershell
cd EECampEdu
python scripts\setup_env.py
conda activate eecampedu
```

The setup script installs Python deploy dependencies and native packages for the ImGui/SDL3 PC app. ESP-IDF is installed separately and must be available in the terminal where `idf.py` is used.

## Build ESP1 Firmware

ESP1 is the ESP32-S3 board with camera, USB, and TFLite Micro inference.

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

Runtime mode is selected in:

```text
firmware/esp/main/include/model_config.hpp
```

Common modes:

| Mode | Purpose | Hardware |
| --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | PC benchmark sends test frames over UART/CDC. | ESP1 only |
| `RuntimeMode::kCameraFlash` | OV2640 capture, flash storage, preprocessing, inference. | ESP1 + OV2640 |
| `RuntimeMode::kCameraUsbMsc` | CDC live preview, MSC storage, camera/control integration. | ESP1 + OV2640 + PC app |
| `RuntimeMode::kPhotoFlashTest` | Preloaded-photo flash test without camera. | ESP1 only |
| `RuntimeMode::kInputOutputSelfTest` | ESP1 input logging and output-route log only. | ESP1 input hardware |

## Build ESP2 Output Firmware

ESP2 is a separate normal ESP32 board dedicated to robotic arm servo output.

```powershell
cd firmware\esp2_output
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

Manual ESP2 test:

```powershell
python firmware\pc\tools\send_esp2_gesture.py --port COM7 up
python firmware\pc\tools\send_esp2_gesture.py --port COM7 right --repeat 3
python firmware\pc\tools\send_esp2_gesture.py --port COM7 P100
```

## Train Source Model

Train or fine-tune the source model before deploy. The deploy pipeline expects a source model artifact under `model_finetune/models/`.

TensorFlow/Keras training flow:

```powershell
cd model_finetune
python train_mobilenet.py --check-only
python train_mobilenet.py
cd ..
```

Expected TensorFlow/Keras handoff artifact:

```text
model_finetune/models/tf/MobileNetV2_finetuned.keras
```

Mini ResNet remains available for comparison:

```powershell
python train_mini_resnet.py
cd ..
python firmware\pc\tools\quantize_keras_model.py --model-name Mini_ResNet_finetuned
```

PyTorch training flow:

```powershell
cd model_finetune
python pytorch\train_mini_resnet.py
```

PyTorch models should be exported through the documented ONNX / Keras-compatible handoff path before firmware quantization. The ESP32 deploy target is still int8 TFLite.

## Quantize And Flash Model

Default TensorFlow source model (recommended):

```text
model_finetune/models/tf/MobileNetV2_finetuned.keras
```

Quantize with representative calibration images from `model_finetune/dataset/train/`:

```powershell
python firmware\pc\tools\quantize_keras_model.py
```

Generated deploy artifacts:

```text
firmware/pc/artifacts/models/MobileNetV2_finetuned_int8.tflite
firmware/pc/artifacts/reports/MobileNetV2_finetuned_quantization_report.json
```

Flash only the ESP1 model partition:

```powershell
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\MobileNetV2_finetuned_int8.tflite" -p COM6
```

## Benchmark

ESP1 firmware mode:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Benchmark inference only:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

Benchmark plus ESP2 robotic arm output:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6 --esp2-port COM7
```

Port meaning:

```text
--port      ESP1 inference board
--esp2-port ESP2 servo output board
```

## Real Camera / Output Runs

Python camera controller path:

```powershell
$env:ESP1_PORT="COM6"
$env:OUTPUT_ESP2_PORT="COM7"
python firmware\pc\tools\camera_controller.py
```

ImGui app path:

```powershell
python scripts\build_input_app.py
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

In the app:

```text
USB CDC Port      -> ESP1 COM port
ESP2 Output Port  -> ESP2 COM port
Auto-forward RESULT checked
Gesture -> Output Mapping set in the ESP2 Output panel
```

## Unit Tests

Dedicated unit-test guides:

```text
docs/unit_tests/model_unit_test.md
docs/unit_tests/deploy_unit_test.md
docs/unit_tests/camera_unit_test.md
docs/unit_tests/usb_unit_test.md
docs/unit_tests/input_unit_test.md
docs/unit_tests/output_unit_test.md
```

Use one mode at a time for unit tests. Use `RuntimeMode::kCameraUsbMsc` only for full camera + USB + UI integration.

Full integration test order:

```text
1. Train/fine-tune source model in model_finetune/.
2. Quantize/calibrate the source model into int8 TFLite under firmware/pc/artifacts/models/.
3. Build and flash ESP1 firmware.
4. Flash the int8 TFLite model into the ESP1 model partition.
5. Build and flash ESP2 output firmware.
6. Run deploy benchmark with --esp2-port to verify accuracy, output similarity, latency, and servo command forwarding.
7. Run camera + USB + ImGui App integration for live preview and real gesture-to-output behavior.
```
