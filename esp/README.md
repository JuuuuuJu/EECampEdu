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

`CAMERA_FLASH_MODE` is scaffolded with `camera_capture` and `photo_storage`
modules. The camera module is currently a stub; replace
`src/camera_capture_stub.cpp` with the OV2640 ESP-IDF implementation when camera
hardware is available.

## Photo Flash Test Mode

Use this when camera hardware is not available but the integration flow should
simulate:

```text
camera capture -> photos flash partition -> deploy inference
```

1. Flash a test image into the `photos` partition:

   ```bash
   python flash_photo.py path/to/test_image.jpg -p COM6
   ```

2. Set `RUNTIME_MODE` in `main/include/model_config.hpp`:

   ```cpp
   constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kPhotoFlashTest;
   ```

3. Rebuild and flash firmware. The device reads the stored photo and runs one
   inference pass on boot.
