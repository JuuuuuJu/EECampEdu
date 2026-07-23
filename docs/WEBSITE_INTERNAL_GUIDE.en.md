# EECampEdu AI PC Portal Internal Guide

This document is for developers and teaching assistants. It describes the web portal features, operating details, hardware routing, and current robot-control behavior. The student-facing guide is `docs/STUDENT_README.md`.

## 1. System Role

The AI PC Portal is a Flask web server running on the AI PC. Students open the site with Chrome or Edge. Through HTTPS + Web Serial, the browser talks to the ESP32-S3 main board and the ESP32 control board connected to the student's PC.

High-level flow:

```text
Dataset / OV2640 capture -> train .keras / .pth -> quantize .tflite
-> flash main board firmware -> flash model partition
-> main board OV2640 inference -> browser forwards ACTION to control board
-> control board drives servos
```

## 2. Background Services

The AI PC normally runs two user services:

```bash
systemctl --user status eecamp-portal
systemctl --user restart eecamp-portal
journalctl --user -u eecamp-portal -f

systemctl --user status eecamp-camera-app
systemctl --user restart eecamp-camera-app
journalctl --user -u eecamp-camera-app -f
```

After changing `apps/training_portal/server.py`, `apps/training_portal/templates/index.html`, portal assets, or portal docs, restart `eecamp-portal` before testing in the browser.

## 3. Portal Pages

### Home

A minimal welcome page with the camp logo and repository footer link. The brand at the top-left returns here.

### Model finetune

This page lets students build a gesture dataset and train their own model.

Features:

- Upload a dataset zip.
- Capture dataset images from OV2640.
- The dataset may contain more than 6 gesture classes, but each run uses at most 6 active classes.
- Configure class-to-robot-action mapping.
- Start TensorFlow or PyTorch training.
- Preview a `.keras` model on the current camera frame.

Training and prediction should use firmware-compatible preprocessing: convert the OV2640 frame into the model input format, currently 96x96 grayscale.

### Deploy & benchmark

This page validates the deploy path on ESP32-S3.

Features:

- Quantize a selected `.keras` model.
- Generate deployable `.tflite` artifacts and quantization reports.
- Flash deploy benchmark firmware.
- Flash the `.tflite` file into the model partition.
- Run an on-device benchmark through browser Web Serial.
- Report accuracy, latency, throughput, and PC-reference output similarity.

Benchmark is for deployment validation.

### Output demo

This page supports the output team's teaching demo. Students edit only a bounded teaching block, not the whole firmware.

Features:

- Edit a restricted C teaching block in the browser.
- Run an allowlisted ESP-IDF build on the AI PC.
- Flash only after a successful build.
- Test LED / PWM / pattern commands over serial.

Compiler errors are shown in full so students can debug their own code.

### Main board firmware

This is the full integration page: camera, USB CDC/MSC, preprocessing, TFLite Micro inference, and prediction forwarding.

Expected sequence:

1. Flash main board firmware to the ESP32-S3.
2. Flash the `.tflite` model to the model partition.
3. Flash control board firmware to the ESP32 control board.
4. Connect camera stream and choose the ESP32-S3 main board serial port.
5. Connect control board and choose the ESP32 control board serial port.
6. When the main board prints `RESULT,...`, the browser forwards the mapped action to the control board.

### Camera + USB demo

This page tests OV2640 and USB CDC/MSC behavior without focusing on training.

Features:

- Live JPEG preview.
- Pixel format, resolution, brightness, contrast, and exposure controls.
- Capture photos to ESP flash.
- Download over CDC or view files after MSC mount.

### AI PC Drive

A per-team local storage area on the AI PC for code, models, zips, and images. It is not ESP flash.

Rules:

- `0_shared` is for team-wide shared files.
- `1` through `12` are personal folders.
- Zip large file groups before upload.
- If the photo is meant to verify ESP-side storage, use ESP flash / MSC instead of AI PC Drive.

## 4. Main Board -> Control Board Logic

The main board firmware performs camera capture, preprocessing, TFLite Micro inference, and prints CDC lines like:

```text
RESULT,<pred>,<model_us>,<preprocess_us>,<total_us>,<score0>,...
```

The portal parses `RESULT` and applies two mappings:

1. `pred index -> class name`
2. `class name -> robot action`

If confidence is below 70%, the portal treats the result as `NULL / idle` and sends:

```text
ACTION,none
```

For confident predictions, it sends one of:

```text
ACTION,up
ACTION,down
ACTION,left
ACTION,right
ACTION,clamp
ACTION,release
```

## 5. Actual Control Board Servo Behavior

The control board is a normal ESP32 connected to four servos. Pins:

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

The control board serial baudrate is `115200`. The portal uses the preferred protocol:

```text
ACTION,<name>
```

Actual servo behavior:

```text
ACTION,up       pitch +5 degrees
ACTION,down     pitch -5 degrees
ACTION,left     base -5 degrees
ACTION,right    base +5 degrees
ACTION,clamp    claw -> 0 degrees
ACTION,release  claw -> 80 degrees
ACTION,none     no movement
```

Servo PWM is 50 Hz with an approximate pulse range of 500 us to 2500 us. Legacy gesture commands and manual angle commands still exist, but the main-board page should use `ACTION,<name>`.

## 6. Troubleshooting

- Flash or benchmark hangs: close Arduino Serial Monitor, idf.py monitor, PuTTY, and any other serial user.
- Web Serial is unavailable: use Chrome or Edge, and serve the portal over HTTPS or localhost secure context.
- Prediction appears but the arm does not move: press `Connect control board` on the main-board page and select the control board serial port.
- Prediction becomes NULL: confidence is below 70%, or the model/class mapping does not match the active class setup.
- UI changes do not appear: restart `eecamp-portal` and refresh the browser.