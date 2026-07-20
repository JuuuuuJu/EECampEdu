# Firmware

`firmware/` contains all deploy-side code for main board, control board, and PC tooling.

```text
firmware/
  main_board/  main board ESP-IDF project: ESP32-S3 camera, USB, input, TFLite Micro inference.
  control_board/ control board ESP-IDF project: normal ESP32 robotic-arm servo controller.
  pc/          Quantization, flashing helpers, benchmark, camera controller, control board bridge tools.
  external/    Reference code from other teams, kept only as source context.
```

Deploy contract:

```text
Source framework can be PyTorch or TensorFlow.
Deploy target is TensorFlow Lite for main board ESP32-S3 TFLite Micro. Full int8 TFLite is the default/recommended target.
Servo output target is control board ESP-IDF firmware, controlled by PC serial commands.
```

## main board Firmware

Build main board:

```powershell
cd firmware\main_board
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

No `git clone --recursive` is required. ESP-IDF managed components are declared in `esp/main/idf_component.yml`.

main board runtime config:

```text
firmware/main_board/main/include/model_config.hpp
```

Important settings:

- `RUNTIME_MODE`: selects benchmark, camera, USB, or self-test behavior.
- `ENABLE_INPUT_CONTROLS`: enables rotary encoder / button GPIO input on main board.
- `TENSOR_ARENA_SIZE`: TFLite Micro tensor arena size. Current integration default is `1536 * 1024` bytes so float32 MobileNetV2 can allocate tensors from PSRAM.
- `MODEL_PARTITION_LABEL`: flash partition containing the selected TFLite model.
- `STORAGE_PARTITION_LABEL`: FAT storage partition used by camera/USB.

main board does not drive robotic-arm servo GPIO. It only prints inference results such as:

```text
RESULT,<class>,<model_us>,<preprocess_us>,<device_us>,<score0>,<score1>,...
```

## Control Board Output Firmware

Build control board:

```powershell
cd firmware\control_board
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

control board receives line-based serial commands from the PC:

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
python firmware\pc\tools\send_control_board_gesture.py --port COM7 up
python firmware\pc\tools\send_control_board_gesture.py --port COM7 P100
```

## Model Deploy

Default source model (recommended):

```text
model_finetune/models/tf/MobileNetV2_finetuned.keras
```

Export source model into deployable TFLite. Default/recommended format is full int8:

```powershell
python firmware\pc\tools\quantize_keras_model.py --quant-format int8 --quant-granularity per-channel
```

The quantization script:

- loads a `.keras` source model from `model_finetune/models/tf/` or `model_finetune/models/pytorch/`
- reads calibration images from `model_finetune/dataset/train/`
- applies grayscale 96x96 preprocessing
- runs TensorFlow Lite representative calibration for integer formats
- exports one of the supported ESP deploy formats: `int8`, `int16`, or `float32`
- writes a quantization report with input/output shape, dtype, scale, zero point, and class order
Supported deploy formats:

| Format | Calibration | Notes |
| --- | --- | --- |
| `int8` | Required | Recommended main board format. Full int8 input/output and weights. |
| `int16` | Required | Experimental int16 activations with int8 weights. Currently supports `per-channel` only and requires per-model TFLite Micro verification. |
| `float32` | Not required | Reference export without quantization. |

`float16` is not listed because this ESP TFLite Micro build cannot prepare float16-weight `DEQUANTIZE`; use `float32` for the floating-point reference path.

`--quant-granularity per-channel` is recommended for integer formats. `per-tensor` is available for int8 as a simpler shared tensor scale mode; int16 currently uses per-channel only.

Generated files:

```text
firmware/pc/artifacts/models/MobileNetV2_finetuned_int8_per-channel.tflite
firmware/pc/artifacts/reports/MobileNetV2_finetuned_int8_per-channel_quantization_report.json
```

Flash only main board model partition:

```powershell
python firmware\main_board\flash_tflite_model.py "firmware\pc\artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" -p COM6
```

The model partition is independent from the firmware app partition. Current main board partition table reserves `4M` for `model` at `0x310000`, then starts FAT `storage` at `0x710000`.

## Benchmark

Set main board firmware mode:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Inference benchmark:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

Inference benchmark plus control board output forwarding:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6 --control-board-port COM7
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
$env:MAIN_BOARD_PORT="COM6"
$env:CONTROL_BOARD_PORT="COM7"
python firmware\pc\tools\camera_controller.py
```

When `camera_controller.py` receives `RESULT,...` from main board, it forwards `GESTURE,<index>,<name>` to control board.

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
- main board inference result logs

MSC exposes the FAT storage partition to the PC as a USB drive. This is frame-by-frame preview over CDC, not UVC.

## Camera Storage Files

Current flash-storage output:

```text
/usb/latest.raw
/usb/latest.meta
```

`latest.bmp` is intentionally not generated in the firmware hot path. Raw payload and metadata are enough for firmware tests, and avoiding BMP conversion keeps storage writes deterministic.

