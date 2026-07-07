# ESP Firmware

This folder contains the ESP-IDF firmware for camera capture, flash storage,
preprocessing, TFLite Micro inference, USB/serial communication, and final
gesture output.

## Layout

```text
main/
  include/              public headers and configuration
  src/                  firmware implementation
  idf_component.yml     ESP-IDF managed dependency declaration
partitions.csv          app/model/photos/storage flash layout
flash_tflite_model.py   flashes a `.tflite` file into the model partition
flash_photo.py          converts an image and flashes it into the photos partition
```

Dependencies are managed by ESP-IDF through `main/idf_component.yml`; this
firmware uses `esp32-camera` for OV2640 capture and `esp-tflite-micro` for
inference.

## Current State

The default firmware mode is `TEST_MODE_UART_FRAME`, configured in
`main/include/model_config.hpp`. This mode runs the deploy benchmark path:

- receives a grayscale frame over UART
- performs ESP-side hand crop
- resizes to `96 x 96`
- quantizes with model input scale / zero-point
- runs TFLite Micro
- prints `RESULT`

This keeps the deploy path testable without OV2640 hardware.

Performance-sensitive defaults:

- ESP32-S3 CPU frequency is configured as `240 MHz`.
- PSRAM is enabled and the default tensor arena path uses PSRAM through
  `PREFER_INTERNAL_TENSOR_ARENA=false`.
- UART benchmark end-to-end throughput is limited mostly by raw frame transfer;
  use model latency / device compute throughput when reporting deploy speed.

Flash layout:

- `model`: 1 MB at `0x310000`.
- `photos`: all remaining flash after `model`, starting at `0x410000`.
- There is no separate `storage` partition in the current integrated layout.

`CAMERA_FLASH_MODE` uses `camera_capture_ov2640.cpp` and `photo_storage`.
The OV2640 module is ported from the provided Arduino `.ino` reference into an
ESP-IDF style `camera_capture` implementation. `camera_capture_stub.cpp` remains
only as a fallback/reference file and is not compiled by the current CMake file.

## Photo Flash Test Mode

Use this when camera hardware is not available but the integration flow should
simulate:

```text
camera capture -> photos flash partition -> deploy inference
```

1. Flash a test image into the `photos` partition:

   ```powershell
   cd D:\0706\EECampEdu
   python esp\flash_photo.py "path\to\test_image.jpg" -p COM6
   ```

   The flashing helpers invoke `python -m esptool` inside the active conda
   environment, so they do not depend on an `esptool.py` executable being on
   `PATH`.

2. Set `RUNTIME_MODE` in `main/include/model_config.hpp`:

   ```cpp
   constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kPhotoFlashTest;
   ```

3. Rebuild and flash firmware. The device reads the stored photo and runs one
   inference pass on boot.
