# Output Unit Test

目標：確認 `gesture result` 可以由 PC 轉送到 ESP2，並正確映射成 robotic arm servo movement。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | No for ESP2 unit test; Yes only for full camera-to-motion test |
| ESP1 ESP32-S3 | No for ESP2 unit test; Yes for full deploy benchmark / real run |
| ESP2 normal ESP32 | Yes |
| Deploy model | No for ESP2 unit test; Yes for full gesture-to-motion benchmark / real run |
| Robotic arm / 4 servos | Yes |
| PC camera | No |

## Architecture

```text
ESP1 ESP32-S3
  -> camera / preprocessing / int8 TFLite inference
  -> RESULT,<class>,<latency>,...
  -> PC benchmark / camera_controller / ImGui App
  -> GESTURE,<class>,<name> over second USB serial
  -> ESP2 normal ESP32
  -> 4 servo movement
```

ESP1 no longer drives servo GPIO directly. Servo output is owned by ESP2.

## ESP2 Servo Pins

`firmware/esp2_output/` is an ESP-IDF project. Its pins match the original output team's `robotic_arm.ino`:

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

## Step 1: Flash ESP2 Output Firmware

```powershell
cd firmware\esp2_output
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

Expected serial boot message:

```text
READY,ESP2_SERVO_OUTPUT
STATE,gesture=4,name=null,base=90,arm=90,pitch=90,claw=30
```

## Step 2: ESP2 Manual Unit Test

From repo root:

```powershell
conda activate eecampedu
python firmware\pc\tools\send_esp2_gesture.py --port COM7 up
python firmware\pc\tools\send_esp2_gesture.py --port COM7 down
python firmware\pc\tools\send_esp2_gesture.py --port COM7 right
python firmware\pc\tools\send_esp2_gesture.py --port COM7 left
python firmware\pc\tools\send_esp2_gesture.py --port COM7 null
```

Servo motion diagnostic:

```powershell
python firmware\pc\tools\send_esp2_gesture.py --port COM7 TEST --timeout 5
```

`TEST` sweeps all four servo channels through center/low/high/center positions. Use this first because `B90`, `A90`, and `P90` may equal the boot angle and show no visible movement.

Manual angle commands are also supported:

```powershell
python firmware\pc\tools\send_esp2_gesture.py --port COM7 B60
python firmware\pc\tools\send_esp2_gesture.py --port COM7 B120
python firmware\pc\tools\send_esp2_gesture.py --port COM7 A60
python firmware\pc\tools\send_esp2_gesture.py --port COM7 A140
python firmware\pc\tools\send_esp2_gesture.py --port COM7 P45
python firmware\pc\tools\send_esp2_gesture.py --port COM7 P120
python firmware\pc\tools\send_esp2_gesture.py --port COM7 C0
python firmware\pc\tools\send_esp2_gesture.py --port COM7 C80
```

Expected ACK format:

```text
OK,gesture=0,name=up,base=90,arm=90,pitch=95,claw=30
```

## Step 3: Full Deploy-To-Output Benchmark Test

Set ESP1 firmware mode:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Flash ESP1 firmware and int8 TFLite model, then run benchmark with two serial ports:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Mini_ResNet_finetuned_96_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6 --esp2-port COM7
```

Port meaning:

```text
--port      ESP1 ESP32-S3 inference board
--esp2-port ESP2 servo output board
```

## Step 4: Real Camera Controller Test

For the Python camera controller path:

```powershell
$env:ESP1_PORT="COM6"
$env:OUTPUT_ESP2_PORT="COM7"
python firmware\pc\tools\camera_controller.py
```

When the controller receives `RESULT,...` from ESP1, it forwards `GESTURE,<index>,<name>` to ESP2.

## Step 5: Real ImGui App Test

Open the ImGui app and use two serial connections:

```text
USB CDC Port      -> ESP1 COM port
ESP2 Output Port  -> ESP2 COM port
Auto-forward RESULT checked
```

Press `Capture once` or run the real inference flow. When ESP1 logs `RESULT,...`, the app forwards the gesture to ESP2.

## Gesture Mapping

```text
up     -> pitch + STEP_DEG
down   -> pitch - STEP_DEG
right  -> base + STEP_DEG
left   -> base - STEP_DEG
null   -> no movement
```

## Pass Criteria

- ESP2 prints `READY,ESP2_SERVO_OUTPUT` after reset.
- Manual commands move the intended servo axis.
- Benchmark prints ESP2 ACK for each prediction when `--esp2-port` is set.
- Python camera controller forwards `RESULT` to ESP2 when `OUTPUT_ESP2_PORT` is set.
- ImGui app forwards `RESULT` when ESP2 Output is connected and `Auto-forward RESULT` is checked.
- `null` does not move the arm.
- Servo angle limits prevent unsafe pitch/claw movement.

