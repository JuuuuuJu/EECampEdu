# Camera Capture Unit Test

Independent ESP-IDF project for the camera group. It tests OV2640 initialization and frame capture only.

## Hardware

- ESP32-S3 board with OV2640 connected to the current PCB camera pins.
- PSRAM must be available for VGA JPEG capture.

## Build / Flash

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\camera_capture_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Commands

Type into monitor:

- `j`: VGA JPEG mode.
- `g`: 96x96 grayscale mode.
- `q`: QVGA grayscale mode.
- Enter/no command: keep capturing with current mode.

## Expected Result

The monitor prints `READY,CAMERA_CAPTURE_TEST`, then one `CAMERA_FRAME` line per second with width, height, format, byte count, first bytes, and checksum. This verifies camera wiring, SCCB init, frame clock, and frame buffer allocation without model/USB logic.
