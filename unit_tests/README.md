# EECampEdu Unit Tests

This folder contains independent ESP-IDF unit test projects for each hardware / firmware group except the model group. These projects are intentionally separated from the integrated firmware so each team can test its own original function without being blocked by other modules.

## Project Map

| Folder | Team | Target Board | Needs OV2640 | Needs Model | Needs Servos | Purpose |
|---|---|---:|---:|---:|---:|---|
| `deploy_inference_test` | deploy | ESP32-S3 | No | Yes | No | Load int8 TFLite from flash partition and run one synthetic inference. |
| `camera_capture_test` | camera | ESP32-S3 | Yes | No | No | Initialize OV2640 and capture JPEG / grayscale frames. |
| `usb_cdc_jpeg_test` | USB | ESP32-S3 | No | No | No | Send a synthetic JPEG frame through USB CDC using the PC preview protocol. |
| `input_controls_test` | input | ESP32-S3 | No | No | No | Read rotary encoder GPIO21/47/48 and print live states. |
| `output_servo_test` | output | ESP2 / ESP32 | No | No | Yes | Control four output servos using gesture or manual angle commands. |

## General Build Pattern

Open an ESP-IDF terminal, then enter one test project:

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\<project_name>
idf.py set-target <esp32s3-or-esp32>
idf.py build
idf.py -p COMx flash monitor
```

Use `esp32s3` for ESP1-side tests and `esp32` for `output_servo_test` unless the actual ESP2 chip is also ESP32-S3.

## Test Details

### Deploy Inference Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\deploy_inference_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Before running, flash an int8 TFLite model to the `model` partition:

```powershell
esptool --chip esp32s3 -p COMx -b 460800 write-flash 0x310000 D:\0711_integration\EECampEdu\firmware\pc\artifacts\models\Mini_ResNet_finetuned_96_int8.tflite
```

Expected output: `READY,DEPLOY_INFERENCE_TEST`, tensor metadata, `RESULT,pred=...,latency_us=...`, and `SCORES,...`.

### Camera Capture Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\camera_capture_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Commands in monitor:

- `j`: VGA JPEG.
- `g`: 96x96 grayscale.
- `q`: QVGA grayscale.

Expected output: one `CAMERA_FRAME` line per second with width, height, format, byte count, first bytes, and checksum.

### USB CDC JPEG Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\usb_cdc_jpeg_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Commands over CDC:

- `f`: send one synthetic JPEG frame.
- `s`: start periodic frame streaming.
- `x`: stop streaming.

Expected output on the PC side: `---START_IMAGE:4:1:1:<bytes>---`, base64 JPEG payload, and `---END_IMAGE---`.

### Input Controls Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\input_controls_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Expected output: `INPUT_STATUS` every 250 ms. Rotate the encoder and press SW:

- `encoder` changes with rotation.
- `encoder_button` increments on switch press.
- `clk`, `dt`, and `sw_level` show raw GPIO levels.

### Output Servo Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\output_servo_test
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Commands:

- `TEST` or `SWEEP`: visible servo sweep diagnostic.
- `up`, `down`, `right`, `left`, `null`: gesture commands.
- `B90`, `A90`, `P90`, `C30`: manual servo angle commands.

Expected output starts with `READY,ESP2_SERVO_OUTPUT`. During `TEST`, all four servos should move through center / low / high / center positions.
