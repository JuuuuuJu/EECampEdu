# Apps

`apps/` contains PC applications. UI code stays here, not inside firmware.

## Apps

- `esp32_cam_input_app/` — Windows Dear ImGui / SDL3 integration demo (below).
- `training_portal/` — AI PC browser GUI to run training + quantization without
  the CLI. See [`training_portal/README.md`](training_portal/README.md).
- `local_flash_helper/` — student-PC localhost helper that flashes the ESP32-S3
  from the browser. See [`local_flash_helper/README.md`](local_flash_helper/README.md).

## Current App

```text
apps/esp32_cam_input_app/
  src/App/        Dear ImGui application state and integration logic
  src/Image/      JPEG decode helper for live preview
  src/Usb/        Windows USB CDC serial client
  src/Windows/    Camera preview and control panels
```

The app is an integration demo for:

- ESP1 USB CDC live preview.
- ESP1 camera capture / stream / format / resolution commands.
- Input UI controls for zoom placeholder, exposure, ISO/gain, and capture.
- ESP2 output connection and manual servo movement tests.
- Automatic forwarding from ESP1 `RESULT,...` lines to ESP2 `GESTURE,<index>,<name>` commands.
- ImGui demo window for rapid component testing.

## Build

From repository root:

```powershell
conda activate eecampedu
python scripts\build_input_app.py --clean
```

Run:

```powershell
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

Use a 64-bit Visual Studio Build Tools environment. The build script avoids the old MinGW compiler because conda `SDL3` is 64-bit.

## Firmware Mode

For USB live preview and UI control tests, set ESP1 firmware to:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

## Serial Connections

The app uses two independent serial connections:

```text
USB CDC Port      ESP1 ESP32-S3 camera/inference board, for example COM6
ESP2 Output Port  ESP2 servo output board, for example COM7
```

With `Auto-forward RESULT` checked, the app forwards every ESP1 `RESULT,...` line to ESP2 as a `GESTURE,<index>,<name>` command.

Manual output buttons are available in the `ESP2 Output` panel for unit testing without camera/model inference.
