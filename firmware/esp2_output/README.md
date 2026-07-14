# ESP2 Output Firmware

`firmware/esp2_output/` is a standalone ESP-IDF project for the normal ESP32 board that controls the robotic arm servos.

ESP1 handles camera and model inference. The PC receives ESP1 `RESULT,...` lines and forwards `GESTURE,<index>,<name>` commands to ESP2 over a second USB serial connection.

## Servo Pins

Pins match the original output team's `robotic_arm.ino`:

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

## Build And Flash

Use an ESP-IDF terminal:

```powershell
cd firmware\esp2_output
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

Expected boot output:

```text
READY,ESP2_SERVO_OUTPUT
STATE,gesture=4,name=null,base=90,arm=90,pitch=90,claw=30
```

## Command Protocol

Gesture commands from PC:

```text
GESTURE,0,up
GESTURE,1,down
GESTURE,2,right
GESTURE,3,left
GESTURE,4,null
```

Motion diagnostic command:

```text
TEST  sweep all four servo channels through visible test positions
SWEEP same as TEST
```

Manual angle commands:

```text
B60 / B120    set base angle/speed command away from the neutral 90 value
A60 / A140    set arm angle
P45 / P120    set pitch angle
C0 / C80      set claw angle
```

Expected ACK example:

```text
OK,gesture=0,name=up,base=90,arm=90,pitch=95,claw=30
```

## Manual PC Test

From repository root:

```powershell
python firmware\pc\tools\send_esp2_gesture.py --port COM7 up
python firmware\pc\tools\send_esp2_gesture.py --port COM7 right --repeat 3
python firmware\pc\tools\send_esp2_gesture.py --port COM7 TEST --timeout 5
```

## Full System Paths

```text
Benchmark path          firmware/pc/benchmark/run_benchmark_png.py --esp2-port COM7
Python controller path  OUTPUT_ESP2_PORT=COM7 python firmware/pc/tools/camera_controller.py
ImGui app path          Connect ESP2 Output panel and enable Auto-forward RESULT
```

