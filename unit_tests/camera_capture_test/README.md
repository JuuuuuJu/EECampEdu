# Camera Capture Unit Test

Independent ESP-IDF project for the camera group. It tests OV2640 capture and can stream frames to `firmware/pc/tools/camera_controller.py` using the same serial image envelope as the integrated firmware.

## Hardware

- ESP32-S3 board with OV2640 connected to the current PCB camera pins.
- PSRAM must be available for VGA JPEG capture.
- No model, input controls, USB MSC, or output servos are required.

## Build / Flash

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\camera_capture_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

After flashing, close `idf.py monitor` before running `camera_controller.py`, because only one program can own the COM port at a time.

## Test with camera_controller.py

The unit-test firmware keeps UART0 at `115200` baud by default so `idf.py monitor` remains readable. This is slower but safer for bring-up. Use QQVGA first; switch to higher baud only after the camera path is stable.

```powershell
cd D:\0711_integration\EECampEdu
$env:ESP1_PORT="COMx"
$env:ESP1_BAUD_RATE="115200"
python firmware\pc\tools\camera_controller.py
```

The controller will automatically send:

- `d0`: stop streaming while resetting state.
- `f3`: set JPEG format.
- `s1`: set QQVGA 160x120 for reliable serial preview. The firmware also defaults to QQVGA for unit-test preview. You can try `s3` VGA only after the link is stable or after switching to a higher baud rate.
- `d1`: start live preview.

## Supported Commands

- `d1`: start preview stream.
- `d0`: stop preview stream.
- `c` or `c<timestamp>`: send one frame.
- `f0`: RGB565.
- `f1`: YUV422.
- `f2`: grayscale.
- `f3`: JPEG.
- `s0`: 96x96.
- `s1`: QQVGA 160x120.
- `s2`: QVGA 320x240.
- `s3`: VGA 640x480.
- `s4`: SVGA 800x600.
- `s5`: UXGA 1600x1200.

## Expected Result

`camera_controller.py` should display a live preview. The ESP sends frames as:

```text
---START_IMAGE:<format>:<width>:<height>:<bytes>---
<base64 frame payload>
---END_IMAGE---
```

This unit test still stays camera-focused: it only adds serial transport so the PC can view frames; it does not run deploy/model/input/output logic.


