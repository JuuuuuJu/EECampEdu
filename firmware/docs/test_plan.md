# Test Plan

This test plan separates module-level checks from full system integration.
All commands assume Windows PowerShell and the repository root:

```powershell
cd D:\0711_integration\EECampEdu
conda activate eecampedu
```

## Common Setup

Required software:

| Item | Purpose |
| --- | --- |
| Windows + PowerShell | Main integration environment |
| Conda env `eecampedu` | PC benchmark, model flashing, image tools |
| ESP-IDF v5.x PowerShell | Firmware build / flash / monitor |
| Python PC requirements | Installed by `python firmware\scripts\setup_pc_env.py` |

Required hardware for full test:

| Item | Purpose |
| --- | --- |
| ESP32-S3, 16 MB flash, 8 MB PSRAM | Main controller |
| USB data cable | Flash, UART/CDC, USB MSC |
| OV2640 camera module | Real camera capture |
| Rotary encoder + push buttons | Input-interface test |
| Servo / robotic arm output board | Output test |
| Stable 5 V power for servos | Avoid brownout during output test |

Build baseline:

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

Flash active model:

```powershell
cd D:\0711_integration\EECampEdu
python firmware\esp\flash_tflite_model.py firmware\pc\artifacts\models\Separable_CNN_int8.tflite -p COM6
```

## Unit Test 1: Model / Deploy

Purpose: verify model partition, TFLite Micro allocation, ESP preprocessing, inference result, latency, and PC output similarity.

Config in `firmware/esp/main/include/model_config.hpp`:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
constexpr bool ENABLE_INPUT_CONTROLS = false;
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = false;
```

Run:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model artifacts\models\Separable_CNN_int8.tflite --dataset ..\..\model_finetune\dataset\validation --port COM6
```

Expected result:

```text
INPUT_TENSOR,type=9,dims=[1 96 96 1]
OUTPUT_TENSOR,type=9,dims=[1 5]
CLASS_MAP,0=up,1=down,2=right,3=left,4=null
RESULT,<prediction>,<model_us>,<preprocess_us>,<device_us>,<score0>...
--- Benchmark Summary ---
Label Accuracy   : ...
Output Similarity Summary
Top-1 Match      : near 100% when PC and ESP use same model/preprocess
```

Pass criteria:

- Firmware does not reset or halt.
- `RESULT` appears for every test image.
- PC reference is enabled, not skipped.
- Score similarity is close to previous baseline: low MAE and cosine close to 1.

## Unit Test 2: Camera

Purpose: verify OV2640 capture and camera reconfiguration without depending on model accuracy.

Config:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
constexpr bool ENABLE_INPUT_CONTROLS = false;
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = false;
```

Model note: this mode now continues even if TFLite initialization fails, so camera/USB can be tested without a valid model partition.

Build / flash firmware, then run monitor. Expected boot log:

```text
Runtime mode: CAMERA_USB_MSC_MODE
Camera stream task started.
USB Composite initialization DONE
```

Use CDC camera controller:

```powershell
cd firmware\pc
python tools\camera_controller.py
```

Expected result:

- `C` / capture sends one frame over CDC.
- `D 1` starts CDC live frame streaming.
- `D 0` stops streaming.
- `F 0`, `F 3` switch grayscale / JPEG.
- `S 0`, `S 2`, `S 3` switch 96x96 / QVGA / VGA where supported.
- CDC stream contains frame delimiters:

```text
---START_IMAGE:<format>:<width>:<height>:<bytes>---
...
---END_IMAGE---
```

Pass criteria:

- No camera init failure.
- Frame dimensions match selected resolution.
- JPEG or QVGA+ logs should use PSRAM frame buffer when PSRAM is enabled.

## Unit Test 3: USB CDC / MSC

Purpose: verify TinyUSB composite device and FAT storage partition.

Config:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

Commands over CDC:

```text
L        list /usb files
W        capture and write latest.raw/latest.meta/latest.bmp
K        clear latest files
usb      expose FAT storage to host PC
format   format FAT storage and reboot
```

Expected result:

```text
--- LOCAL FLASH FILE LIST ---
/usb/latest.raw
/usb/latest.meta
/usb/latest.bmp
```

After `usb`, Windows should show the ESP32-S3 FAT partition as a USB drive.

Pass criteria:

- CDC commands respond.
- `/usb` mount works on app side.
- MSC mount works on PC side.
- FAT files are visible and file sizes are non-zero after `W`.

## Unit Test 4: Input

Purpose: verify rotary encoder and push-button GPIO interrupt handling.

Config:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kInputOutputSelfTest;
constexpr bool ENABLE_INPUT_CONTROLS = true;
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = false;
```

Important: current prototype pins conflict with OV2640 pins. Run this test without the camera attached unless final GPIOs are reassigned.

Expected log every `IO_SELF_TEST_INTERVAL_MS`:

```text
SELFTEST_INPUT,pos=<encoder_position>,enc_btn=<count>,btn2=<count>,enc_level=<0/1>,btn2_level=<0/1>
SELFTEST_OUTPUT,action=<0..4>,enabled=0
```

Pass criteria:

- Rotating encoder changes `pos`.
- Encoder push increases `enc_btn`.
- Second button increases `btn2`.
- No false rapid increments while idle.

## Unit Test 5: Output

Purpose: verify robotic-arm servo output mapping without camera/model dependency.

Config:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kInputOutputSelfTest;
constexpr bool ENABLE_INPUT_CONTROLS = false;
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = true;
```

Hardware warning: servo GPIOs currently conflict with OV2640 defaults. Run this test without the camera attached unless pins are reassigned.

Expected log:

```text
SELFTEST_OUTPUT,action=0,enabled=1
OUTPUT_ARM,action=0,base=90,arm=90,pitch=95,claw=30
SELFTEST_OUTPUT,action=1,enabled=1
OUTPUT_ARM,action=1,...
```

Expected motion:

| Action | Meaning |
| --- | --- |
| `0` | up: pitch increases |
| `1` | down: pitch decreases |
| `2` | right: base increases |
| `3` | left: base decreases |
| `4` | null: no movement |

Pass criteria:

- Servo moves in the expected direction.
- ESP32-S3 does not brown out.
- `OUTPUT_ARM` log follows every enabled self-test action.


## Unit Test 6: PC Input UI App

Purpose: verify the input-interface Dear ImGui + SDL3 desktop app builds and opens independently of ESP hardware.

Source folder:

```text
apps/esp32_cam_input_app
```

Run:

```powershell
cd apps\\esp32_cam_input_app
cmake -S . -B build
cmake --build build --config Release
.\build\Release\eecampedu_input_demo.exe
```

If using a single-config generator such as Ninja, the executable may be under:

```powershell
.\build\eecampedu_input_demo.exe
```

Expected result:

- A desktop window titled `EECampEdu ESP32 Input Demo` opens.
- `Camera / USB Monitor`, `Controls`, and the Dear ImGui demo window are visible.
- `Controls` contains USB CDC connect/disconnect, capture, stream, save-to-storage, list-storage, expose-USB-drive, format, resolution, exposure, gain/ISO, and vertical-flip controls.
- Without ESP hardware, the UI still opens and reports a COM-port connection failure cleanly.
- With firmware `RuntimeMode::kCameraUsbMsc`, pressing `Connect` and then `Capture once` sends the `C` command over the selected Windows COM port.
- It does not yet send TinyUSB CDC commands to ESP32-S3.
- Future command binding should map UI actions to firmware commands such as `C`, `D 1`, `D 0`, `F <n>`, `S <n>`, exposure/gain commands, `usb`, and `format`.

## Full Integration Test

Purpose: verify end-to-end system from camera/input to model inference and output action.

Recommended config after GPIO conflicts are resolved:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
constexpr bool ENABLE_INPUT_CONTROLS = true;
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = true;
```

For the current pin-conflict prototype, test camera and output separately first. Do not attach OV2640 and servo/input hardware to conflicting GPIOs at the same time.

Procedure:

1. Quantize or select model.

```powershell
python firmware\pc\tools\quantize_keras_model.py --model-name Separable_CNN
```

2. Build and flash firmware.

```powershell
cd firmware\esp
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

3. Flash model partition.

```powershell
cd D:\0711_integration\EECampEdu
python firmware\esp\flash_tflite_model.py firmware\pc\artifacts\models\Separable_CNN_int8.tflite -p COM6
```

4. Verify boot.

Expected:

```text
Runtime mode: CAMERA_USB_MSC_MODE
INPUT_TENSOR,type=9,dims=[1 96 96 1]
OUTPUT_TENSOR,type=9,dims=[1 5]
USB Composite initialization DONE
Camera stream task started.
```

If model is missing but camera/USB is the target, expected warning:

```text
TFLite Micro unavailable; continuing camera/USB test without inference.
```

5. Start PC camera controller.

```powershell
cd firmware\pc
python tools\camera_controller.py
```

6. Send camera commands.

```text
F 0      grayscale for inference path
S 0      96x96 raw frame, safest inference/capture test
C        capture one frame and stream to PC
D 1      continuous preview
D 0      stop preview
W        save latest frame to FAT /usb
I        infer latest stored grayscale frame
```

7. Confirm model inference.

Expected:

```text
DEVICE_FRAME_DUMP,...
DEVICE_MODEL_DUMP,...
RESULT,<prediction>,<model_us>,<preprocess_us>,<device_us>,<scores...>
```

8. Confirm final output.

If `ENABLE_ROBOT_ARM_OUTPUT=true`, expected:

```text
OUTPUT_ARM,action=<prediction>,base=...,arm=...,pitch=...,claw=...
```

9. Optional benchmark validation.

Switch back to `RuntimeMode::kTestUartFrame`, rebuild/flash firmware, then run:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model artifacts\models\Separable_CNN_int8.tflite --dataset ..\..\model_finetune\dataset\validation --port COM6
```

Expected benchmark summary:

```text
Images Processed : <N>
Label Accuracy   : ...
Average Model Latency      : ... ms
Average Preprocess Latency : ... ms
Average Device Compute     : ... ms
Output Similarity Summary  : enabled
```

## Troubleshooting Checklist

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `idf.py` not found | Not in ESP-IDF PowerShell | Open ESP-IDF terminal or export IDF tools path |
| `TFLite Micro initialization failed` | Missing/wrong model partition or arena too small | Flash model; check PSRAM; increase `TENSOR_ARENA_SIZE` |
| PC reference skipped | TensorFlow/TFLite package missing | Re-run `python firmware\scripts\setup_pc_env.py` |
| Camera init failed | OV2640 wiring/pin/power issue | Check wiring, XCLK/SIOD/SIOC, power, PSRAM config |
| MSC drive not visible | Host still sees CDC only or app holds mount | Send `usb`; reconnect USB cable if needed |
| Input counts do not change | GPIO conflict/wiring/pull-up issue | Test without camera; verify pins in `model_config.hpp` |
| Servo resets ESP | Servo power brownout | Use external 5 V servo power and common ground |
| Output moves wrong direction | Mechanical orientation differs | Swap action mapping or invert servo direction in `output_controls.cpp` |

