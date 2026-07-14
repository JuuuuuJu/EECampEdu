# Output Servo Unit Test

Independent ESP-IDF project for the output group. It controls the four robotic-arm servos only; it does not run camera, USB preview, or model inference.

## Hardware

- ESP2 board connected over USB serial.
- Four servos wired to the same pins as the output group's original robotic arm code:
  - base: GPIO18
  - arm: GPIO19
  - pitch: GPIO22
  - claw: GPIO21
- External servo power must share GND with ESP2.

## Build / Flash

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\output_servo_test
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

## Commands

- `TEST` or `SWEEP`: sweep all servos through a visible diagnostic sequence.
- `up`, `down`, `right`, `left`, `null`: gesture-style motion commands.
- `B90`, `A90`, `P90`, `C30`: manual base/arm/pitch/claw angle commands.

Expected monitor output starts with `READY,ESP2_SERVO_OUTPUT`.
