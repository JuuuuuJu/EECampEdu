# Input Unit Test

目標：確認 input team 的 rotary encoder、buttons、PC UI controls 可以控制 firmware state。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | For camera UI preview: Yes. For encoder-only: No |
| ESP32-S3 | Yes |
| Deploy model | No |
| Robotic arm | No |
| PC camera | No |

## Encoder Pins

PCB allocation:

```text
CLK / A     GPIO21
DT / B      GPIO47
SW          GPIO48
```

Configure in:

```text
firmware/esp/main/include/model_config.hpp
```

Enable:

```cpp
constexpr bool ENABLE_INPUT_CONTROLS = true;
```

## Firmware Mode

For input-only firmware log test:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kInputOutputSelfTest;
```

Run:

```powershell
cd firmware\esp
idf.py build
idf.py -p COM6 flash monitor
```

Expected log:

```text
INPUT_CONTROL,encoder=...,delta=...,encoder_button=...
```

## PC UI Test

Build:

```powershell
cd EECampEdu
python scripts\build_input_app.py --clean
```

Run:

```powershell
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

Expected UI behavior:

- Camera preview panel can connect to ESP CDC.
- Capture and stream controls send commands.
- Zoom / exposure / ISO UI controls update app state and can be wired to firmware CDC commands.
- ImGui demo window is available for component testing.

## Pass Criteria

- Encoder rotation changes delta.
- Encoder button changes state.
- PC UI can open and send commands without crashing.
- UI code remains under `apps/`, not `firmware/`.

