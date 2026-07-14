# EECampEdu Unit Tests

This folder contains independent ESP-IDF unit test projects for each hardware / firmware group except the model group. These projects are intentionally separated from the integrated firmware so each team can test its own original function without being blocked by other modules.

## Project Map

| Folder | Team | Target Board | Needs OV2640 | Needs Model | Needs Servos | Purpose |
|---|---|---:|---:|---:|---:|---|
| `deploy_inference_test` | deploy | ESP32-S3 | No | Yes | No | Load an int8 TFLite model from flash and benchmark model inference latency / FPS. |
| `camera_capture_test` | camera | ESP32-S3 | Yes | No | No | Initialize OV2640 and stream JPEG/grayscale frames to `camera_controller.py` for live preview. |
| `usb_cdc_jpeg_test` | USB | ESP32-S3 | No | No | No | Send synthetic JPEG frames through USB CDC and benchmark CDC throughput. |
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

Expected output:

- `READY,DEPLOY_INFERENCE_TEST`
- `INPUT_TENSOR,...`
- `OUTPUT_TENSOR,...`
- warm-up `RESULT,pred=...,latency_us=...,warmup=1`
- `SCORES,...`
- `--- Deploy Inference Benchmark ---`
- `Average Latency`, `Min Latency`, `Max Latency`, `Model Throughput`
- machine-readable `BENCHMARK_RESULT,iterations=50,avg_us=...,min_us=...,max_us=...,fps=...,pred=...`

This verifies deploy runtime health and gives model-only inference speed. It is not an accuracy benchmark because the input is synthetic.

### Camera Capture Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\camera_capture_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash
```

Then close any serial monitor and run the camera controller:

```powershell
cd D:\0711_integration\EECampEdu
$env:ESP1_PORT="COMx"
$env:ESP1_BAUD_RATE="2000000"
python firmware\pc\tools\camera_controller.py
```

Supported controller commands include `d1` / `d0` for preview start/stop, `c` for one frame, `f3` for JPEG, and `s3` for VGA. Expected result: `camera_controller.py` shows a live preview using the serial `---START_IMAGE...---` frame envelope.

### USB CDC JPEG Test

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\usb_cdc_jpeg_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Open the USB CDC COM port with a serial tool:

```powershell
python -m serial.tools.miniterm COMx 115200
```

APP-compatible commands:

- `D1`: start periodic JPEG frame streaming.
- `D0`: stop streaming.
- `C`: send one test JPEG frame.

Manual serial commands:

- `f`: send one test JPEG frame.
- `s`: start periodic JPEG frame streaming.
- `x`: stop streaming.

CDC speed benchmark commands:

- `B` or `BENCH`: send a 64 KiB raw CDC payload and print throughput.
- `B256`: send a 256 KiB raw CDC payload. The number after `B` is KiB.
- `T` or `SOAK`: continuously send raw CDC payload for 600 seconds, about 10 minutes.
- `T60` / `T600`: continuously send raw CDC payload for the requested number of seconds.
- `FBENCH`: send 50 framed synthetic JPEG images and print frame-protocol throughput.

Expected benchmark output:

```text
USB_CDC_BENCH_BEGIN,mode=raw,bytes=65536
USB_CDC_BENCH_RESULT,mode=raw,bytes=65536,elapsed_us=...,kBps=...
USB_CDC_BENCH_BEGIN,mode=timed_raw,duration_s=600
USB_CDC_BENCH_PROGRESS,mode=timed_raw,elapsed_s=30.0,bytes=...,kBps=...
USB_CDC_BENCH_RESULT,mode=timed_raw,duration_s=600,bytes=...,elapsed_us=...,kBps=...
USB_CDC_BENCH_BEGIN,mode=frame,frames=50
USB_CDC_BENCH_RESULT,mode=frame,frames=50,bytes=...,elapsed_us=...,kBps=...,fps=...
```

The raw benchmark measures CDC payload throughput. The frame benchmark measures the actual image envelope path used by the PC preview parser.

For the 10-minute benchmark, use the sink script so the terminal does not slow down the transfer:

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\usb_cdc_jpeg_test
python tools\cdc_timed_benchmark.py --port COMx --seconds 600
```

The sink script discards payload bytes and refreshes one host-side progress line every second. Use `--progress-interval 5` to update less often or `--progress-interval 0` to disable host-side progress.

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
