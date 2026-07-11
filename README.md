# EECampEdu

This repository is split into three integration areas:

```text
EECampEdu/
  model_finetune/   Training, fine-tuning, datasets, and source `.keras` models.
  firmware/         ESP32-S3 firmware, deploy quantization, flashing, and benchmark tools.
  apps/             PC desktop UI and integration demos.
```

All paths are relative to the repository root. Do not hardcode local drive letters.

## Workflow

```text
model_finetune/models/*.keras
  -> deploy-side int8 quantization with representative calibration images
  -> firmware/pc/artifacts/models/*_int8.tflite
  -> flash ESP32-S3 model partition
  -> run UART benchmark or camera/USB integration mode
  -> optionally control camera/input through apps/esp32_cam_input_app
```

## Team Integration

- `model_finetune/`: trains or fine-tunes gesture models and hands off `.keras` files.
- `firmware/pc/tools/quantize_keras_model.py`: performs deploy-side calibration and int8 TFLite export.
- `firmware/esp`: builds/flashes ESP32-S3 firmware, runs inference, captures OV2640 frames, exposes USB MSC storage, and sends output actions.
- `apps/esp32_cam_input_app`: PC-side Dear ImGui + SDL3 demo for input controls, camera commands, USB CDC monitoring, and ImGui component prototyping.

Camera USB mode stores captured frames on the ESP32-S3 storage partition and can expose that partition as USB MSC. The PC UI can also use CDC commands for capture/stream/control. This is frame-by-frame refresh, not true UVC video streaming.

## Windows PC Setup

Use PowerShell with conda:

```powershell
cd EECampEdu
python scripts\setup_env.py
conda activate eecampedu
```

The setup script installs the Python deploy stack plus native desktop-app build packages (`cmake`, `ninja`, `sdl3`). Dear ImGui is not bundled; the desktop app fetches it during CMake configure.

## Quantize A Keras Model

Default inputs:

```text
model_finetune/models/<model_name>.keras
model_finetune/dataset/train/
```

Command:

```powershell
python firmware\pc\tools\quantize_keras_model.py --model-name Separable_CNN
```

Generated deploy artifacts:

```text
firmware/pc/artifacts/models/Separable_CNN_int8.tflite
firmware/pc/artifacts/reports/Separable_CNN_quantization_report.json
```

## Build Firmware

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

## Flash Model Only

From repository root:

```powershell
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\Separable_CNN_int8.tflite" -p COM6
```

## Benchmark

Firmware must run `RuntimeMode::kTestUartFrame`.

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Separable_CNN_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

## ImGui Input Demo

Firmware should run `RuntimeMode::kCameraUsbMsc` for CDC/MSC testing.

Build the PC UI from the repository root:

```powershell
conda activate eecampedu
python scripts\build_input_app.py --clean
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

The input demo must be built with a 64-bit Windows C++ compiler. The build script finds Visual Studio / Build Tools `vcvars64.bat` and prevents CMake from accidentally using old `C:\MinGW\bin\c++.exe`. This matters because the conda `SDL3` package is 64-bit; using the old MinGW compiler can make `find_package(SDL3)` fail with an incompatible 64-bit package message.

If the script cannot find a compiler, install Visual Studio Build Tools with `Desktop development with C++`.

See `firmware/docs/test_plan.md` for unit-test and full-integration procedures.
