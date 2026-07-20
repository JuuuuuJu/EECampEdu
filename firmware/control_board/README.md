# Control Board Output Firmware

`firmware/control_board/` is a standalone ESP-IDF project for the normal ESP32 board that controls the robotic arm servos.

main board handles camera and model inference. The PC receives main board `RESULT,...` lines, maps the predicted gesture to one output action, then forwards `ACTION,<name>` commands to control board over a second USB serial connection.

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
cd firmware\control_board
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

Expected boot output:

```text
READY,CONTROL_BOARD_SERVO_OUTPUT
STATE,gesture=4,name=null,base=90,arm=90,pitch=90,claw=30
```

## Command Protocol

Preferred action commands from PC / APP:

```text
ACTION,up       pitch servo moves upward by one step
ACTION,down     pitch servo moves downward by one step
ACTION,left     base servo moves left by one step
ACTION,right    base servo moves right by one step
ACTION,clamp    claw servo closes
ACTION,release  claw servo opens
ACTION,none     no movement
```

Legacy gesture commands are still accepted for compatibility:

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
python firmware\pc\tools\send_control_board_gesture.py --port COM7 up
python firmware\pc\tools\send_control_board_gesture.py --port COM7 clamp
python firmware\pc\tools\send_control_board_gesture.py --port COM7 release
python firmware\pc\tools\send_control_board_gesture.py --port COM7 right --repeat 3
python firmware\pc\tools\send_control_board_gesture.py --port COM7 TEST --timeout 5
```

## Full System Paths

```text
Benchmark path          firmware/pc/benchmark/run_benchmark_png.py --control-board-port COM7
Python controller path  CONTROL_BOARD_PORT=COM7 python firmware/pc/tools/camera_controller.py
ImGui app path          Connect Control Board Output panel and enable Auto-forward RESULT
```
