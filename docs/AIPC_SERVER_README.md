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

## Background Service 1: Training Portal (systemd)

The portal runs as a **systemd user service** (`eecamp-portal`) — no `sudo` for daily
start/stop/status/logs, and it restarts on failure and at boot. It serves HTTPS on
port 8080 (browser Web Serial flashing needs a secure context).

**Install once** (creates + enables the user service, and `deploy/eecamp-portal.env`
for secrets):

```bash
cd ~/EECampEdu
./deploy/install_services.sh              # portal only
# ./deploy/install_services.sh --with-camera   # also install the camera app service
# edit secrets, then restart:
nano deploy/eecamp-portal.env             # set EECAMP_PORTAL_SECRET / team passwords
```

If a portal is still running from the old `nohup` method, stop it first so both do
not bind port 8080: `pkill -f "apps/training_portal/server.py"`.

**Manage** (no sudo):

```bash
systemctl --user start   eecamp-portal      # start
systemctl --user stop    eecamp-portal      # stop
systemctl --user restart eecamp-portal      # restart (after code/env/cert changes)
systemctl --user status  eecamp-portal      # status
journalctl  --user -u eecamp-portal -f      # live logs
curl -sk https://127.0.0.1:8080/api/health  # health check
```

Or use the wrapper: `./deploy/eecampctl.sh {start|stop|restart|status|logs} [portal|camera]`.

**Run without an active login** (survive logout / reboot) — the only step needing sudo,
run once:

```bash
sudo loginctl enable-linger $USER
```

The unit is generated from `deploy/systemd/eecamp-portal.service.in` (interpreter and
repo path substituted at install time). Restart the service after changing Python, HTML,
CSS, JS, env vars, or certificates. Job logs remain under `apps/training_portal/runs/jobs/`.

## Portal Pages

- `/model_finetune`: dataset upload, class-to-action mapping, TensorFlow/PyTorch training, camera-only firmware flash, and browser-side OV2640 preview/capture. OV2640 is like flashing: the browser borrows the serial port from the student PC; the AI PC server does not directly own that camera USB port.
- `/deploy`: deploy benchmark firmware flash, quantization, model flashing, and **browser Web Serial benchmark** (runs on the student PC's board; the AI PC only serves dataset images).
- `/output`: output teaching firmware flash + LED/PWM controls.
- `/firmware`: full main board firmware flash + live OV2640 preview/inference.
- `/camera_usb`: full OV2640 camera + USB-storage control demo (pixel format, resolution, exposure/gain/AWB/brightness, capture to flash, expose as USB drive), modeled on `firmware/pc/tools/camera_controller.py`. Runs on the main board firmware over browser Web Serial.
- `/drive`: **AI PC Drive** — this team's shared file storage on the AI PC (see below).

All Web Serial flows (flash, model flash, benchmark, camera preview, camera+USB demo, output control) share one lifecycle: one port open at a time, release-before-acquire, and a full close after each workflow.

The UI is a responsive **top navigation bar** with the team name, Logout, a display-theme
selector (**Dark / Light / High contrast**, remembered per browser), and the flash-baud
selector at the top-right. Low-level flash offsets, developer mode, and the flash-sync
(BOOT) selector are not exposed in the student UI.

## AI PC Drive

One AI PC serves one team, so the Drive is that team's shared storage on this machine
(login still required). Files live under `apps/training_portal/runs/drive/` (git-ignored),
in folders `0_shared/` (team-wide) and `1/` … `12/` (per member/station), auto-created on
startup. Use it for code, models, reports, build logs, and general uploads.

- **Upload / list / download / delete** and inline **image preview** from the `/drive` page.
- **Zip many files before uploading** — one `.zip` is far faster than many small files
  (the server accepts uploads up to 2 GB).
- **Captured photos are not stored here** — they live on the ESP32-S3 and are viewed /
  downloaded from the camera pages.

API (all login-gated): `GET /api/drive/folders`, `GET /api/drive/list?folder=`,
`POST /api/drive/upload` (`folder`,`file`), `GET /api/drive/download?folder=&name=`,
`GET /api/drive/image?folder=&name=` (inline), `POST /api/drive/delete` (`folder`,`name`).
File names are sanitized to a single path segment inside the chosen folder (no traversal);
unknown folders are rejected.

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

## Background Service 2: Local Camera / Control App (systemd)

Optional, and runs on the **student PC** that physically owns the USB hardware (not
the AI PC). The portal now has a browser-side OV2640 preview path; use this helper only
when testing live gesture/result forwarding outside the portal. It is packaged as the
`eecamp-camera-app` user service.

```bash
cd ~/EECampEdu
./deploy/install_services.sh --with-camera   # installs both portal + camera app units
systemctl --user start  eecamp-camera-app     # start / stop / restart / status
systemctl --user status eecamp-camera-app
journalctl  --user -u eecamp-camera-app -f    # live logs
curl http://127.0.0.1:8770/health
# or: ./deploy/eecampctl.sh start camera
```

## Flash Helper

`apps/local_flash_helper` is fallback-only. Normal portal flashing uses browser Web Serial directly and does not require a helper or `127.0.0.1`.
