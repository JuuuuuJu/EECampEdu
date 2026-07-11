# ESP32 Camera Input Demo

This desktop UI belongs to the PC/input-interface layer, not to firmware. It combines:

- Dear ImGui demo window for UI component development.
- Input controls for zoom/exposure/gain placeholders.
- USB CDC controls for the ESP32-S3 camera firmware (`C`, `D`, `W`, `L`, `usb`, `F`, `S`, `E`, `G`, `V`).
- A serial monitor panel that shows CDC responses and frame headers.
- Live texture preview for CDC grayscale and RGB565 frames. JPEG frame preview intentionally waits for a decoder dependency decision.

Dependencies are not vendored in this app:

- `SDL3` is installed by the repository setup script through conda.
- `Dear ImGui` is downloaded by CMake FetchContent during configure.
- USB CDC uses the Windows COM port API directly, so `libserialport` is not bundled.

## Build

From the repository root after running `python scripts\\setup_env.py`:

```powershell
conda activate eecampedu
cmake -S apps\esp32_cam_input_app -B apps\esp32_cam_input_app\build -G Ninja
cmake --build apps\esp32_cam_input_app\build
```

Run:

```powershell
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

Use firmware `RuntimeMode::kCameraUsbMsc` for USB CDC/MSC integration testing.
