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

- `/model_finetune`: dataset upload, class-to-action mapping, TensorFlow/PyTorch training, and browser-side OV2640 preview. OV2640 is like flashing: the browser borrows the serial port from the student PC; the AI PC server does not directly own that camera USB port.
- `/deploy`: quantization, model flashing, AI-PC-side benchmark.
- `/output`: output teaching firmware flash + LED/PWM controls.
- `/firmware`: full main board firmware flash.

The UI is a top menu with routes, not a terminal tutorial.

## Build Firmware Artifacts For Browser Flashing

The portal reads ESP-IDF `build/flasher_args.json`. If missing, the page tells you to build first.

```bash
cd firmware/main_board
idf.py set-target esp32s3
idf.py build

cd ../teaching_output_demo
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

Benchmark is started from `/deploy` and runs on the AI PC. The ESP32-S3 must be physically connected to the AI PC USB port. If the board is plugged into a student's PC, browser flashing can work, but AI-PC-side benchmark cannot access that serial port.

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