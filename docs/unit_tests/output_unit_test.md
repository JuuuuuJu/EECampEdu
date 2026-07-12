# Output Unit Test

目標：確認 gesture result 可以映射到 robotic arm servo movement。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | No |
| ESP32-S3 | Yes |
| Deploy model | No for servo self-test, Yes for full gesture-to-motion |
| Robotic arm | Yes |
| PC camera | No |

## Servo Pins

PCB allocation:

```text
base    GPIO39
arm     GPIO40
pitch   GPIO41
claw    GPIO42
```

Configure in:

```text
firmware/esp/main/include/model_config.hpp
```

Enable:

```cpp
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = true;
```

## Self-Test Mode

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kInputOutputSelfTest;
```

Build and flash:

```powershell
cd firmware\esp
idf.py build
idf.py -p COM6 flash monitor
```

Expected behavior:

- Servos initialize to configured initial angles.
- Firmware cycles or logs output actions.
- No camera or model is required for this self-test.

## Full Gesture-To-Motion Test

Set a camera/inference mode, flash a valid deploy model, and enable robot output:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraFlash;
constexpr bool ENABLE_ROBOT_ARM_OUTPUT = true;
```

Expected mapping:

```text
up     -> arm moves up
down   -> arm moves down
right  -> arm moves right
left   -> arm moves left
null   -> no movement
```

## Pass Criteria

- Servo PWM initializes.
- Each command moves the intended axis.
- `null` does not move the arm.
- Movement limits prevent unsafe angles.

