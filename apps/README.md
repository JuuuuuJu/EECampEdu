# PC Applications

`apps/` contains desktop-side integration tools. These are intentionally outside
`firmware/` because they run on the host PC, not on the ESP32-S3.

## Applications

- `esp32_cam_input_app/`: Dear ImGui + SDL3 demo that combines the input team's
  controls, the ImGui demo window, and USB CDC commands for the ESP32 camera
  firmware.

Generated build directories are ignored by git.
