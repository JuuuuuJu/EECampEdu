# USB CDC JPEG Unit Test

Independent ESP-IDF project for the USB group. It tests the CDC image-frame protocol only; it does not use camera, model inference, MSC storage, input, or output servo.

## Hardware

- ESP32-S3 USB device port connected to PC.
- No OV2640 required.
- No model required.

## Build / Flash

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\usb_cdc_jpeg_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Test with PC App / Serial Tool

Open the ESP USB CDC COM port and send:

- `f`: send one test JPEG frame.
- `s`: start periodic JPEG frame streaming.
- `x`: stop streaming.

The transmitted envelope is:

```text
---START_IMAGE:4:1:1:<bytes>---
<base64 jpeg payload>
---END_IMAGE---
```

This verifies CDC framing and PC-side image parser without camera timing or inference noise.
