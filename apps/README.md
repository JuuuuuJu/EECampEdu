# Apps

`apps/` contains Windows PC applications. UI code stays here, not inside firmware.

## Current App

```text
apps/esp32_cam_input_app/
  src/App/        Dear ImGui application state and windows
  src/Image/      JPEG decode helper for live preview
  src/Usb/        USB CDC serial client
  src/Windows/    Camera preview and control panels
```

The app is an integration demo for:

- USB CDC live preview from ESP32-S3.
- Camera capture / stream / format / resolution commands.
- Input UI controls for zoom, exposure, ISO, and capture.
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

For USB live preview and UI control tests, set firmware to:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

