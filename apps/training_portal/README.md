# AI PC Training Portal

A browser GUI that lets students run the existing gesture-model **training** and
**quantization** pipeline on the AI PC without typing CLI commands.

One AI PC serves one team. In the classroom there are 10 AI PCs, exposed by the
gateway as:

| Team | URL |
|------|-----|
| 1 | http://140.112.194.42:8081 |
| 2 | http://140.112.194.42:8082 |
| … | … |
| 10 | http://140.112.194.42:8090 |

Current classroom setup: Team 1 is already reachable at
`http://140.112.194.42:8081`, forwarded to that AI PC's `:8080` portal.

## What it does

The portal is a thin, safe launcher around scripts that already live in the repo
— it does **not** reimplement any ML:

| GUI action | Underlying project script |
|------------|---------------------------|
| Train · MobileNetV2 (TensorFlow) | `model_finetune/train_mobilenet.py` |
| Train · Mini ResNet (TensorFlow) | `model_finetune/train_mini_resnet.py` |
| Train · Mini ResNet (PyTorch) | `model_finetune/pytorch/train_mini_resnet.py` |
| Quantize | `firmware/pc/tools/quantize_keras_model.py` |

Features: upload a dataset `.zip` (one folder per gesture class, **any names**, **two or more** classes — the dataset may hold more than six),
map each class to a robot action (`up/down/left/right/clamp/release`, saved to
`class_mapping.json`), pick framework + recipe, start training, start quantization,
watch **live job logs**, and list/download generated artifacts (`.keras`,
`.pth`, `.onnx`, `.tflite`, quantization report `.json`). See the repo README
section *Arbitrary Class Names & Robot-Action Mapping* for the full flow.

## Run (on the AI PC)

**Recommended: run it as a systemd user service** (`./deploy/install_services.sh`,
then `systemctl --user start eecamp-portal`) — see
[`docs/AIPC_SERVER_README.md`](../../docs/AIPC_SERVER_README.md) for the full
start/stop/status/log commands. The direct invocation below is equivalent and useful
for one-off runs.

Recommended — **HTTPS on port 8080** (a secure context, so browser **Web Serial
flashing** works). Port 8080 serves HTTPS when `--https` is set; there is **one
port** for both the site and flashing (no separate 8443):

```bash
conda activate eecampedu
python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https \
  --cert-host 140.112.194.42        # optional: embed the gateway IP in the cert
```

`--https` auto-generates a self-signed cert/key under `runs/certs/` on first run
(reused afterward; needs the `cryptography` package, already in requirements — or
pass your own `--cert`/`--key`). Because the cert is self-signed, **students see a
one-time "Your connection is not private" warning** and must click
**Advanced → Proceed to <site> (unsafe)** once; after that the portal loads and
Web Serial flashing works.

Plain HTTP (drop `--https`) is still available but does **not** support browser
flashing:

```bash
python apps/training_portal/server.py --host 0.0.0.0 --port 8080   # HTTP, no flashing
```

The AI PC listens on `0.0.0.0:8080`; the classroom gateway forwards each team's
public port (8081–8090) to that `:8080`. Students then open their team URL over
**HTTPS**: **`https://140.112.194.42:8081`** (not `http://…`).

For the full classroom network setup (stable IP, run-as-service, firewall, and
the gateway port-forwarding table), see [`DEPLOYMENT.md`](DEPLOYMENT.md).

## Dataset zip layout

Upload **one zip** with **one folder per gesture class** — **any names**, **two or
more** classes. The dataset may hold **more than six** classes; each training /
inference run uses an **active set of up to six** (chosen on the Model finetune
page). The layout is auto-detected:

```
dataset.zip            (auto-split into train/validation)
├── n1/  *.jpg
├── n2/  *.jpg
├── …          (any number of folders, any names)
└── nN/  *.jpg

dataset.zip            (kept as-is if it already has splits)
├── train/{n1..nN}/*.jpg
└── validation/{n1..nN}/*.jpg
```

All classes are imported to disk. The **active** class set (≤6) is saved to
`model_finetune/dataset/class_mapping.json` as `class_order`, and training,
quantization, benchmark, preview inference, and the student PC control app all use
that same active set. On import the active set defaults to the first six classes
alphabetically; change it anytime with the active-class checkboxes. Nothing is
hardcoded.

## Safety model

- **No arbitrary shell.** Jobs are built from a fixed allowlist of project
  scripts with validated numeric/enum arguments; every subprocess uses
  `shell=False`.
- **Background jobs.** Training/quantization run in a worker thread; HTTP
  requests never block. Only **one** job runs at a time (HTTP 409 while busy) so
  a shared AI PC is not overloaded.
- **Download sandbox.** `/api/artifacts/download` only serves files that resolve
  inside the known model/artifact directories — no path traversal.
- **Zip-slip safe.** Uploaded zips are validated against `..`/absolute paths.

## Runtime folder (git-ignored)

```
apps/training_portal/runs/
├── uploads/                  uploaded dataset zips
└── jobs/<job-id>/
    ├── job.log               live/full job output
    └── meta.json             web-run metadata
```

`runs/` is listed in the repository `.gitignore`.

## HTTP API

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/` | Portal page |
| GET | `/api/health` | Status + active job |
| GET | `/api/recipes` | Recipes, quant options, available `.keras` models |
| POST | `/api/dataset/upload` | Upload+import one dataset zip (`file`); auto-detect/auto-split |
| GET/POST | `/api/class-map` | Read / save class→action mapping (`class_mapping.json`) |
| POST | `/api/train` | Start training (`recipe`, optional `epochs`/`batch_size`/`alpha`/`export_onnx`) |
| POST | `/api/quantize` | Start quantization (`model_name`, `quant_format`, `quant_granularity`) |
| GET | `/api/jobs` | Job history |
| GET | `/api/jobs/<id>/log?offset=N` | Incremental log |
| GET | `/api/artifacts` | List generated artifacts |
| GET | `/api/artifacts/download?path=…` | Download an artifact |
| GET | `/api/flash-meta` | Model-partition flash metadata (hidden offset) |
| GET | `/api/firmware/meta` | Main board firmware flash plan from `flasher_args.json` (or "not built" message) |
| GET | `/api/firmware/download?name=…` | Download one firmware `.bin` (allowlisted to the build dir) |

## Pages

The GUI is a responsive **top navigation bar** (team name + Logout + Dark/Light/High-contrast theme selector at the top-right) over topic routes: `/model_finetune`, `/deploy`, `/output`, `/firmware`, `/camera_usb` (full OV2640 camera + USB-storage control, modeled on `firmware/pc/tools/camera_controller.py`), and `/drive` (**AI PC Drive** — this team's shared file storage under `runs/drive/` with folders `0_shared/` and `1/`…`12/`; upload/list/download/delete + image preview; zip many files before upload; captured photos are stored on the ESP32-S3, not here). Every Web Serial workflow — flash, model flash, benchmark, OV2640 preview, camera+USB demo, and output control — shares one lifecycle: exactly one port is open at a time, each flow releases the previous owner before acquiring, and the port is fully closed after every workflow. Firmware targets are separated by class: `firmware/model_finetune` is camera-only OV2640 preview/capture, `firmware/main_board` is full camera/USB/continuous inference, `firmware/deploy_benchmark` is benchmark-only `RuntimeMode::kTestUartFrame`, and `firmware/teaching_output_demo` is GPIO/PWM output practice.
Train/Quantize run as background jobs (shared job log / artifacts / history
panels); the flash, OV2640 preview, and **`/deploy` on-device benchmark** all run
in the browser via Web Serial against the board on the **student PC** (the AI PC
only serves dataset images through `/api/benchmark/images`). No AI-PC serial port
and no student-PC Python are involved.

## Flashing (browser Web Serial)

The ESP32-S3 is plugged into the **student PC** and flashed **from the browser**
via the Web Serial API — no install, no Python, no `127.0.0.1`. Students use
**Chrome or Edge**. Separate page sections:

- **Flash model** — updates only the `.tflite` in the model partition. Select a
  model → **Connect ESP32-S3** → **Flash model**. The page downloads the artifact
  through `/api/artifacts/download` and flashes it; the model-partition offset
  comes from `/api/flash-meta` and is not shown.
- **Flash firmware** — updates one ESP-IDF firmware target at a time. The portal reads the selected target's `build/flasher_args.json` via `/api/firmware/meta`, then flashes bootloader + partition table + app at ESP-IDF-generated offsets. Current targets:
  - `/model_finetune`: `firmware/model_finetune/build` (camera-only OV2640 preview/capture, no TFLite model required).
  - `/firmware`: `firmware/main_board/build` (`RuntimeMode::kCameraUsbMsc`, full camera/USB/continuous inference).
  - `/deploy`: `firmware/deploy_benchmark/build` (`RuntimeMode::kTestUartFrame`, benchmark-only frame input).
  - `/output`: `firmware/teaching_output_demo/build` (GPIO/LED/PWM class firmware).
  If a target is not built, that page tells the instructor which firmware folder to build first. Offsets are hidden unless **developer mode** (header checkbox) is on.

**Flash baud rate.** A **Flash baud** selector in the page header sets the write baud
used by **every** flashing flow (model partition *and* all firmware targets). The
default is **460800** (safe and fast); options are 115200 / 230400 / 460800 / 921600.
The initial ROM sync always stays at **115200** regardless of this setting, so a bad
choice never blocks the bootloader handshake — it only changes the write speed. If
flashing is unreliable (long/cheap USB cable, some USB-UART bridges), lower it to
230400 or 115200; the chosen value is echoed at the top of each flash log.

Both use the vendored [esptool-js](static/vendor/esptool-js/) (offline, pinned);
each page section has its own status line + flash log (connect → bootloader → erase →
write → verify → done).

Requirements & notes:

- **Secure context (and the plain-HTTP workaround):** browsers expose Web Serial
  only over **HTTPS** or `localhost`. Over plain `http://<ip>:<port>` it is hidden.
  Either serve the portal over HTTPS, **or** allowlist the exact origin per PC via
  `chrome://flags/#unsafely-treat-insecure-origin-as-secure` (Edge: `edge://flags/...`)
  → Enabled → Relaunch. The Flash panel auto-detects the HTTP case and shows these
  exact steps (with a Copy-origin button). The flag may be blocked by managed
  browser policy. Non-Chromium browsers show *"…please use Chrome or Edge."*
- The backend does **no** command execution — it only lists/serves artifacts and
  returns flash metadata.
- `apps/local_flash_helper/` remains a **manual fallback only** (not wired into
  the portal) for environments without Web Serial.
