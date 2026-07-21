# AI PC Server Guide

The AI PC runs the web portal for one team. The gateway forwards the public team port to this machine's `:8080`.

Example: `https://140.112.194.42:8081` -> `https://<team-ai-pc>:8080`.

## Environment

```bash
python scripts/setup_env.py --skip-native
conda activate eecampedu
```

Use ESP-IDF separately when building firmware artifacts.

## Team Login

Default development passwords are `eecamp01` ... `eecamp10` for `team01` ... `team10`.
For real class use, set passwords outside git:

```bash
export EECAMP_PORTAL_SECRET='change-this-session-secret'
export EECAMP_TEAM_PASSWORDS='{"team01":"...","team02":"..."}'
```

## Background Service 1: Training Portal

Recommended: HTTPS on port 8080, because browser Web Serial flashing needs a secure context.

```bash
cd ~/EECampEdu
conda activate eecampedu
mkdir -p apps/training_portal/runs
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https \
  > apps/training_portal/runs/server.log 2>&1 &
```

Check / log / stop:

```bash
curl -sk https://127.0.0.1:8080/api/health
tail -f apps/training_portal/runs/server.log
pkill -f "apps/training_portal/server.py"
```

Restart after changing Python, HTML, CSS, JS, README-served text, env vars, or certificates. Existing job logs remain under `apps/training_portal/runs/jobs/`.

## Portal Pages

- `/model_finetune`: dataset upload, class-to-action mapping, TensorFlow/PyTorch training, camera-only firmware flash, and browser-side OV2640 preview/capture. OV2640 is like flashing: the browser borrows the serial port from the student PC; the AI PC server does not directly own that camera USB port.
- `/deploy`: deploy benchmark firmware flash, quantization, model flashing, and **browser Web Serial benchmark** (runs on the student PC's board; the AI PC only serves dataset images).
- `/output`: output teaching firmware flash + LED/PWM controls.
- `/firmware`: full main board firmware flash + live OV2640 preview/inference.

The UI is a top menu with routes, not a terminal tutorial.

## Build Firmware Artifacts For Browser Flashing

The portal reads ESP-IDF `build/flasher_args.json`. If missing, the page tells you to build first.

```bash
cd firmware/model_finetune      # camera-only OV2640 preview/capture firmware
idf.py set-target esp32s3
idf.py build

cd ../main_board                # full camera + USB + continuous inference firmware
idf.py set-target esp32s3
idf.py build

cd ../deploy_benchmark          # benchmark-only TEST_MODE_UART_FRAME firmware
idf.py set-target esp32s3
idf.py build

cd ../teaching_output_demo      # output class GPIO/PWM firmware
idf.py set-target esp32s3
idf.py build
```

## Train And Quantize

Students start jobs from `/model_finetune` and `/deploy`. The server only runs allowlisted project scripts; it does not expose arbitrary shell execution.

Generated files:

```text
model_finetune/models/             source models: .keras, .pth, .onnx
firmware/pc/artifacts/models/      deployable .tflite
firmware/pc/artifacts/reports/     quantization reports
apps/training_portal/runs/jobs/    live job logs
```

## Benchmark

The student benchmark on `/deploy` runs **in the browser over Web Serial**, exactly like browser flashing: the
ESP32-S3 stays on the **student's PC**, and the AI PC server only serves the dataset images
(`GET /api/benchmark/options`, `/api/benchmark/images`, `/api/benchmark/image`). There is **no** AI-PC serial port
and **no** server-side benchmark job. Students flash the deploy benchmark firmware + model, pick a dataset, and the
browser streams frames to the board and reads `RESULT` lines to compute accuracy/latency/throughput.

**Developer/teacher CLI fallback** (optional, when you deliberately put a board on this machine): run the reference
benchmark manually — see `docs/LOCAL_LEGACY_README.md` (`firmware/pc/benchmark/run_benchmark_png.py`). It additionally
computes PC-reference output similarity (Top-1 / MAE / Max Error / Cosine).

## Background Service 2: Local Camera / Control App

This is optional and runs on the PC that physically owns the USB hardware. The portal now has a browser-side OV2640 preview path; use this helper only when testing live gesture/result forwarding outside the portal.

```bash
conda activate eecampedu
nohup python apps/local_camera_app/preview_app.py > camera_app.log 2>&1 &
curl http://127.0.0.1:8770/health
tail -f camera_app.log
pkill -f "local_camera_app/preview_app.py"
```

## Flash Helper

`apps/local_flash_helper` is fallback-only. Normal portal flashing uses browser Web Serial directly and does not require a helper or `127.0.0.1`.
