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

Features: upload a dataset `.zip` (exactly six class folders, **any names**),
map each class to a robot action (`up/down/left/right/clamp/release`, saved to
`class_mapping.json`), pick framework + recipe, start training, start quantization,
watch **live job logs**, and list/download generated artifacts (`.keras`,
`.pth`, `.onnx`, `.tflite`, quantization report `.json`). See the repo README
section *Arbitrary Class Names & Robot-Action Mapping* for the full flow.

## Run (on the AI PC)

Plain HTTP (default):

```bash
conda activate eecampedu
python apps/training_portal/server.py --host 0.0.0.0 --port 8080
```

HTTPS (self-signed) — enables browser **Web Serial flashing**, which requires a
secure context:

```bash
python apps/training_portal/server.py --host 0.0.0.0 --port 8443 --https \
  --cert-host 140.112.194.42        # optional: embed the gateway IP in the cert
```

`--https` auto-generates a self-signed cert/key under `runs/certs/` on first run
(reused afterward; needs the `cryptography` package, already in requirements — or
pass your own `--cert`/`--key`). Because the cert is self-signed, **students see a
one-time "Your connection is not private" warning** and must click
**Advanced → Proceed to <site> (unsafe)** once; after that the portal loads and
Web Serial flashing works. HTTP mode remains fully available.

The AI PC listens on `0.0.0.0:8080` (or `:8443` for HTTPS); the classroom gateway
forwards each team's public port (8081–8090) to that port. Students then open
their team URL.

For the full classroom network setup (stable IP, run-as-service, firewall, and
the gateway port-forwarding table), see [`DEPLOYMENT.md`](DEPLOYMENT.md).

## Dataset zip layout

Upload **one zip** with exactly **six** class folders — **any names**. The layout
is auto-detected:

```
dataset.zip            (auto-split into train/validation)
├── n1/  *.jpg
├── n2/  *.jpg
├── …          (six folders, any names)
└── n6/  *.jpg

dataset.zip            (kept as-is if it already has splits)
├── train/{n1..n6}/*.jpg
└── validation/{n1..n6}/*.jpg
```

The discovered class order is saved to `model_finetune/dataset/class_mapping.json`
and used by training, quantization, benchmark, and the student PC control app.
Any six names work — nothing is hardcoded.

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

## Flashing (browser Web Serial)

The ESP32-S3 is plugged into the **student PC** and flashed **from the browser**
via the Web Serial API — no install, no Python, no `127.0.0.1`. Students use
**Chrome or Edge**: select a `.tflite` model, click **Connect ESP32-S3** (the
browser shows a serial-port picker), then **Flash model**. The page downloads the
artifact through `/api/artifacts/download` and flashes the model partition using
the vendored [esptool-js](static/vendor/esptool-js/) (offline, pinned). The job
log shows live progress: connect → bootloader → erase → write → verify → done.
The flash offset comes from `/api/flash-meta` and is not shown.

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
