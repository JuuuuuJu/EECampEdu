# Control Board Firmware

`firmware/control_board/` is the standalone ESP-IDF project for the normal ESP32 board that drives the robotic arm servos.

The ESP32-S3 main board handles camera and model inference. The browser receives main board `RESULT,...` lines, maps the predicted class to an action, and forwards the action to the control board over a second serial connection.

## Servo Pins

Pins match the output hardware design:

| Servo | GPIO |
|---|---:|
| base | 18 |
| arm | 19 |
| pitch | 22 |
| claw | 21 |

## Build And Flash

Use an ESP-IDF shell:

```powershell
cd firmware\control_board
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

## Commands

Preferred action commands:

```text
ACTION,up
ACTION,down
ACTION,left
ACTION,right
ACTION,clamp
ACTION,release
ACTION,none
```

Diagnostic and manual commands:

```text
TEST
SWEEP
B90
A90
P90
C30
```

Expected reply:

```text
OK,gesture=4,name=null,base=90,arm=90,pitch=90,claw=30
```

## Portal Behavior

Main board page should connect both:

1. ESP32-S3 main board
2. ESP32 control board

If prediction confidence is below 70%, the portal forwards idle/null behavior instead of a movement command.
