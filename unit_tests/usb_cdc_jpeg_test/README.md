# USB CDC JPEG Unit Test

Independent ESP-IDF project for the USB group. It tests the CDC image-frame protocol and CDC transfer speed only; it does not use camera, model inference, MSC storage, input, or output servo.

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

Open the ESP USB CDC COM port:

```powershell
python -m serial.tools.miniterm COMx 115200
```

APP-compatible commands:

- `D1`: start periodic JPEG frame streaming.
- `D0`: stop streaming.
- `C`: send one test JPEG frame.

Manual serial commands are also supported:

- `f`: send one test JPEG frame.
- `s`: start periodic JPEG frame streaming.
- `x`: stop streaming.

The transmitted image envelope is:

```text
---START_IMAGE:4:1:1:<bytes>---
<base64 jpeg payload>
---END_IMAGE---
```

## CDC Speed Benchmark

Commands:

- `B` or `BENCH`: send a 64 KiB raw CDC payload and print throughput.
- `B256`: send a 256 KiB raw CDC payload. The number after `B` is KiB.
- `T` or `SOAK`: continuously send raw CDC payload for 600 seconds, about 10 minutes.
- `T60` / `T600`: continuously send raw CDC payload for the requested number of seconds.
- `FBENCH` or `FRAME_BENCH`: send 50 framed synthetic JPEG images and print frame-protocol throughput.

Expected output:

```text
USB_CDC_BENCH_BEGIN,mode=raw,bytes=65536
USB_CDC_BENCH_RESULT,mode=raw,bytes=65536,elapsed_us=...,kBps=...
USB_CDC_BENCH_BEGIN,mode=timed_raw,duration_s=600
USB_CDC_BENCH_PROGRESS,mode=timed_raw,elapsed_s=30.0,bytes=...,kBps=...
USB_CDC_BENCH_RESULT,mode=timed_raw,duration_s=600,bytes=...,elapsed_us=...,kBps=...
USB_CDC_BENCH_BEGIN,mode=frame,frames=50
USB_CDC_BENCH_RESULT,mode=frame,frames=50,bytes=...,elapsed_us=...,kBps=...,fps=...
```

Metric meaning:

- `kBps`: CDC payload throughput in KiB/s.
- `fps`: image-frame protocol throughput, using the same frame envelope as the PC preview path.
- `elapsed_us`: ESP-side elapsed time measured around CDC writes.

Use `B64` first if the terminal becomes noisy; raw benchmark intentionally sends real payload bytes before printing the summary.

For the 10-minute benchmark, prefer the sink script instead of `miniterm`; terminal rendering can become the bottleneck:

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\usb_cdc_jpeg_test
python tools\cdc_timed_benchmark.py --port COMx --seconds 600
```

The sink script discards payload bytes and refreshes one host-side progress line every second:

```text
Host progress: elapsed=   12.0s received=    8.42 MiB avg=  718.30 KiB/s
```

Adjust or disable host progress with:

```powershell
python tools\cdc_timed_benchmark.py --port COMx --seconds 600 --progress-interval 5
python tools\cdc_timed_benchmark.py --port COMx --seconds 600 --progress-interval 0
```

Use a shorter smoke test first:

```powershell
python tools\cdc_timed_benchmark.py --port COMx --seconds 60
```
