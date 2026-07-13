# Firmware

`firmware/` contains all deploy-side code for ESP1, ESP2, and PC tooling.

```text
firmware/
  esp/         ESP1 ESP-IDF project: ESP32-S3 camera, USB, input, TFLite Micro inference.
  esp2_output/ ESP2 ESP-IDF project: normal ESP32 robotic-arm servo controller.
  pc/          Quantization, flashing helpers, benchmark, camera controller, ESP2 bridge tools.
  external/    Reference code from other teams, kept only as source context.
```

Deploy contract:

```text
Source framework can be PyTorch or TensorFlow.
Deploy target is unified as int8 TFLite for ESP1 ESP32-S3 TFLite Micro.
Servo output target is ESP2 ESP-IDF firmware, controlled by PC serial commands.
```

## ESP1 Firmware

Build ESP1:

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

No `git clone --recursive` is required. ESP-IDF managed components are declared in `esp/main/idf_component.yml`.

ESP1 runtime config:

```text
firmware/esp/main/include/model_config.hpp
```

Important settings:

- `RUNTIME_MODE`: selects benchmark, camera, USB, or self-test behavior.
- `ENABLE_INPUT_CONTROLS`: enables rotary encoder / button GPIO input on ESP1.
- `TENSOR_ARENA_SIZE`: TFLite Micro tensor arena size.
- `MODEL_PARTITION_LABEL`: flash partition containing the int8 TFLite model.
- `STORAGE_PARTITION_LABEL`: FAT storage partition used by camera/USB.

ESP1 does not drive robotic-arm servo GPIO. It only prints inference results such as:

```text
RESULT,<class>,<model_us>,<preprocess_us>,<device_us>,<score0>,<score1>,...
```

## ESP2 Output Firmware

Build ESP2:

```powershell
cd firmware\esp2_output
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

ESP2 receives line-based serial commands from the PC:

```text
GESTURE,0,up
GESTURE,1,down
GESTURE,2,right
GESTURE,3,left
GESTURE,4,null
```

It also accepts manual servo angle commands for output unit tests:

```text
B90  base servo
A90  arm servo
P100 pitch servo
C30  claw servo
```

Servo pins match the original output team's `robotic_arm.ino`:

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

Manual PC test:

```powershell
python firmware\pc\tools\send_esp2_gesture.py --port COM7 up
python firmware\pc\tools\send_esp2_gesture.py --port COM7 P100
```

## Model Deploy

Default source model:

```text
model_finetune/models/tf/Mini_ResNet_finetuned_96.keras
```

Quantize source model into int8 TFLite:

```powershell
python firmware\pc\tools\quantize_keras_model.py
```

The quantization script:

- loads a `.keras` source model from `model_finetune/models/tf/` or `model_finetune/models/pytorch/`
- reads calibration images from `model_finetune/dataset/train/`
- applies grayscale 96x96 preprocessing
- runs TensorFlow Lite representative calibration
- exports full int8 input/output TFLite
- writes a quantization report with input/output shape, dtype, scale, zero point, and class order

Generated files:

```text
firmware/pc/artifacts/models/Mini_ResNet_finetuned_96_int8.tflite
firmware/pc/artifacts/reports/Mini_ResNet_finetuned_96_quantization_report.json
```

Flash only ESP1 model partition:

```powershell
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\Mini_ResNet_finetuned_96_int8.tflite" -p COM6
```

The model partition is independent from the firmware app partition.

## Benchmark

Set ESP1 firmware mode:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Inference benchmark:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Mini_ResNet_finetuned_96_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

Inference benchmark plus ESP2 output forwarding:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Mini_ResNet_finetuned_96_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6 --esp2-port COM7
```

Primary deploy metrics:

- `Label Accuracy`
- `Average Model Latency`
- `Average Preprocess Latency`
- `Device Compute Throughput`
- `Top-1 Match`
- `Average Score MAE`
- `Max Score Error`
- `Average Cosine`

## Real Camera Controller Flow

Use this path when testing real camera capture through the Python controller:

```powershell
$env:ESP1_PORT="COM6"
$env:OUTPUT_ESP2_PORT="COM7"
python firmware\pc\tools\camera_controller.py
```

When `camera_controller.py` receives `RESULT,...` from ESP1, it forwards `GESTURE,<index>,<name>` to ESP2.

## Camera Modes

### Camera Flash Mode

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraFlash;
```

This mode owns the camera task, captures grayscale QVGA frames, writes `latest.raw/latest.meta` to `/usb`, resizes to model input, then runs inference.

### Camera USB/MSC Mode

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

This mode starts TinyUSB CDC/MSC plus the camera stream task.

CDC is used for:

- live JPEG preview frames
- camera commands
- input/control debug messages
- ESP1 inference result logs

MSC exposes the FAT storage partition to the PC as a USB drive. This is frame-by-frame preview over CDC, not UVC.

## Camera Storage Files

Current flash-storage output:

```text
/usb/latest.raw
/usb/latest.meta
```

`latest.bmp` is intentionally not generated in the firmware hot path. Raw payload and metadata are enough for firmware tests, and avoiding BMP conversion keeps storage writes deterministic.
