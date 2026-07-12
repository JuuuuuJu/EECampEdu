# Firmware

`firmware/` contains ESP32-S3 firmware plus PC-side deploy tools.

Deploy contract:

```text
Source framework can be PyTorch or TensorFlow.
Deploy target is unified as int8 TFLite for ESP32-S3 TFLite Micro.
```

```text
firmware/
  esp/       ESP-IDF project, camera/input/output/USB/inference firmware
  pc/        Benchmark, model conversion, validation, and camera-control tools
  external/  Reference code from other teams, kept only as source context
```

## Build Firmware

Use ESP-IDF v5.x for ESP32-S3:

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

No `git clone --recursive` is required. ESP-IDF managed components are declared in `esp/main/idf_component.yml`.

## Key Configuration

Edit:

```text
firmware/esp/main/include/model_config.hpp
```

Important settings:

- `RUNTIME_MODE`: selects benchmark, camera, USB, or self-test behavior.
- `ENABLE_INPUT_CONTROLS`: enables rotary encoder / button GPIO input.
- `ENABLE_ROBOT_ARM_OUTPUT`: enables servo output.
- `TENSOR_ARENA_SIZE`: TFLite Micro tensor arena size.
- `MODEL_PARTITION_LABEL`: flash partition containing the int8 TFLite model.
- `STORAGE_PARTITION_LABEL`: FAT storage partition used by camera/USB.

## Deploy Model

Default source model:

```text
model_finetune/models/tf/Mini_ResNet_finetuned_96.keras
```

Quantize the source model into the ESP deploy target:

```powershell
python firmware\pc\tools\quantize_keras_model.py
```

The quantization script:

- loads the `.keras` source model from `model_finetune/models/tf/ or model_finetune/models/pytorch/`
- reads calibration images from `model_finetune/dataset/train/`
- converts images to grayscale float `[0.0, 1.0]`
- runs TensorFlow Lite representative calibration
- exports a full int8 input/output TFLite model
- writes a quantization report with input/output shape, dtype, scale, and zero point

Generated files:

```text
firmware/pc/artifacts/models/Mini_ResNet_finetuned_96_int8.tflite
firmware/pc/artifacts/reports/Mini_ResNet_finetuned_96_quantization_report.json
```

Flash only the model partition:

```powershell
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\Mini_ResNet_finetuned_96_int8.tflite" -p COM6
```

The model partition is independent from the firmware app partition.

Current deploy flow:

```text
.pth / .onnx / .keras source
  -> deploy conversion / calibration
  -> *_int8.tflite
  -> flash model partition
  -> TFLite Micro AllocateTensors()
```

Firmware code keeps the input/output contract stable: input tensor shape, preprocessing, class order, and supported TFLite operators.

## Benchmark

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Run:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Mini_ResNet_finetuned_96_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

Primary deploy metrics:

- `Label Accuracy`
- `Average Model Latency`
- `Device Compute Throughput`
- `Top-1 Match`
- `Average Score MAE`
- `Max Score Error`
- `Average Cosine`

## Camera Flash Mode

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraFlash;
```

This mode owns the camera task, captures grayscale QVGA frames, writes `latest.raw/latest.meta` to `/usb`, resizes to the model input, then runs inference.

The live preview stream task is intentionally not started in this mode. This prevents camera ownership conflicts and avoids mixing JPEG/VGA preview capture with grayscale inference capture.

## Camera USB/MSC Mode

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

This mode starts TinyUSB CDC/MSC plus the camera stream task.

CDC is used for:

- live JPEG preview frames
- camera commands
- input/control debug messages

MSC is used to expose the FAT storage partition to the PC as a USB drive. This is frame-by-frame preview over CDC, not UVC.

## Camera Storage Files

Current flash-storage output:

```text
/usb/latest.raw
/usb/latest.meta
```

`latest.bmp` is intentionally not generated in the firmware hot path. The raw payload and metadata are enough for firmware tests, and avoiding BMP conversion keeps storage writes deterministic.
