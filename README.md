# EECampEdu

EECampEdu is the integrated workspace for the gesture-controlled robotic arm system.

Current integration architecture:

```text
Model source: PyTorch or TensorFlow
Deploy target: TensorFlow Lite, default/recommended int8 TFLite
Inference board: main board, ESP32-S3
Output board: control board, normal ESP32
PC role: live preview, benchmark, command bridge, and UI
```

main board no longer drives servos directly. main board runs camera/model inference and prints `RESULT,...`; the PC forwards the predicted gesture to control board over a second USB serial link; control board controls the robotic arm servos.

## Repository Layout

```text
EECampEdu/
  model_finetune/   Training scripts, class_map.py (shared class order), source-model handoff.
                    Datasets and generated models are git-ignored (see "Generated vs tracked").
  firmware/         main board firmware, control board output firmware, quantization, flashing, benchmark, camera, USB.
  apps/
    training_portal/     AI PC browser GUI: dataset upload, class mapping, train, quantize, artifacts.
    local_flash_helper/  Fallback-only CLI helper (portal flashes via browser Web Serial instead).
    local_camera_app/    Student-PC localhost app: live gesture result + class->action + control board forward.
    esp32_cam_input_app/ Native Dear ImGui / SDL3 app: USB-CDC live camera preview + controls.
  docs/             Guides and notes.
```

All paths are relative to this repository root. Do not hardcode local drive letters in committed scripts.

### Generated vs tracked

The repo keeps **source, scripts, READMEs, and config templates** only. These are
git-ignored and regenerated locally (never commit them):

```text
model_finetune/dataset/{train,validation,test,sign_mnist}/   uploaded/imported datasets
model_finetune/dataset/class_mapping.json                        generated at dataset import
model_finetune/models/**/*.{keras,onnx,pth,h5}               trained models (except the pretrained seed)
firmware/pc/artifacts/                                        generated .tflite + quantization reports
apps/training_portal/runs/                                    uploaded zips, job logs, web-run metadata
*.zip, *.log, build/, __pycache__/
```

Kept as an input seed: `model_finetune/models/pytorch/mini_resnet_pretrained_weights.pth`.
Class-map template: `model_finetune/class_mapping.example.json`.

## System Flow

```text
Model team
  -> train / fine-tune in PyTorch or TensorFlow
  -> export source artifacts under model_finetune/models/

Deploy team
  -> export source model into deployable TFLite
  -> flash main board model partition
  -> validate accuracy, latency, throughput, and output similarity

main board ESP32-S3
  -> OV2640 capture
  -> grayscale / crop / resize / scale
  -> TFLite Micro inference
  -> print RESULT,<class>,<model_us>,<preprocess_us>,<device_us>,<scores...>

PC
  -> receives main board result
  -> benchmark and/or UI live preview
  -> maps each gesture class to a user-selected output action

control board normal ESP32
  -> receives ACTION,up/down/left/right/clamp/release/none over second USB serial
  -> drives robotic arm servos
```

## Team Responsibilities

| Team | Main folder | Main responsibility |
| --- | --- | --- |
| Model | `model_finetune/` | Train/fine-tune gesture models and provide source artifacts. |
| Deploy | `firmware/` | Convert PyTorch/TensorFlow handoff into ESP-deployable TFLite, flash, benchmark, and validate ESP behavior. |
| Camera | `firmware/main_board/main/src/camera_capture_ov2640.cpp` | OV2640 capture and frame format/resolution control on main board. |
| USB | `firmware/main_board/main/src/usb_composite.cpp` | main board USB CDC command stream and USB MSC storage exposure. |
| Input | `apps/`, `firmware/main_board/main/src/input_controls.cpp` | PC UI, rotary encoder/button controls, camera parameters. |
| Output | `firmware/control_board/`, `firmware/pc/tools/send_control_board_gesture.py` | control board ESP-IDF servo control and PC-to-control board gesture bridge. |

## Windows Environment

Use PowerShell with conda:

```powershell
cd EECampEdu
python scripts\setup_env.py
conda activate eecampedu
```

The setup script installs Python deploy dependencies and native packages for the ImGui/SDL3 PC app. ESP-IDF is installed separately and must be available in the terminal where `idf.py` is used.

## Build main board Firmware

main board is the ESP32-S3 board with camera, USB, and TFLite Micro inference.

```powershell
cd firmware\main_board
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

Runtime mode is selected in:

```text
firmware/main_board/main/include/model_config.hpp
```

Common modes:

| Mode | Purpose | Hardware |
| --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | PC benchmark sends test frames over UART/CDC. | main board only |
| `RuntimeMode::kCameraFlash` | OV2640 capture, flash storage, preprocessing, inference. | main board + OV2640 |
| `RuntimeMode::kCameraUsbMsc` | CDC live preview, MSC storage, camera/control integration. | main board + OV2640 + PC app |
| `RuntimeMode::kPhotoFlashTest` | Preloaded-photo flash test without camera. | main board only |
| `RuntimeMode::kInputOutputSelfTest` | main board input logging and output-route log only. | main board input hardware |

## Build Control Board Output Firmware

control board is a separate normal ESP32 board dedicated to robotic arm servo output.

```powershell
cd firmware\control_board
idf.py set-target esp32
idf.py build
idf.py -p COM7 flash monitor
```

```text
base  GPIO18
arm   GPIO19
pitch GPIO22
claw  GPIO21
```

Manual control board test:

```powershell
python firmware\pc\tools\send_control_board_gesture.py --port COM7 up
python firmware\pc\tools\send_control_board_gesture.py --port COM7 right --repeat 3
python firmware\pc\tools\send_control_board_gesture.py --port COM7 P100
```

## Train Source Model

Train or fine-tune the source model before deploy. The deploy pipeline expects a source model artifact under `model_finetune/models/`.

TensorFlow/Keras training flow:

```powershell
cd model_finetune
python train_mobilenet.py --check-only
python train_mobilenet.py
cd ..
```

Expected TensorFlow/Keras handoff artifact:

```text
model_finetune/models/tf/MobileNetV2_finetuned.keras
```

Mini ResNet remains available for comparison:

```powershell
python train_mini_resnet.py
cd ..
python firmware\pc\tools\quantize_keras_model.py --model-name Mini_ResNet_finetuned
```

PyTorch training flow:

```powershell
cd model_finetune
python pytorch\train_mini_resnet.py
```

PyTorch models should be exported through the documented ONNX / Keras-compatible handoff path before firmware export. The recommended ESP32 deploy target is full int8 TFLite.

## Quantize And Flash Model

Default TensorFlow source model (recommended):

```text
model_finetune/models/tf/MobileNetV2_finetuned.keras
```

Export the source model into a deployable TFLite file. Default is full `int8`, which is the recommended ESP32-S3 deployment format:

```powershell
python firmware\pc\tools\quantize_keras_model.py --quant-format int8 --quant-granularity per-channel
```

Supported `--quant-format` choices:

| Format | Calibration | Notes |
| --- | --- | --- |
| `int8` | Required | Recommended. Full int8 input/output and int8 weights. Best size/speed target for main board. |
| `int16` | Required | Experimental TFLite int16 activations with int8 weights. Currently supports `per-channel` only; verify TFLite Micro operator support per model. |
| `float32` | Not required | No quantization. Useful as a reference export; usually larger/slower on ESP. |

`float16` is intentionally not offered as an ESP deploy option: this ESP TFLite Micro build registers `DEQUANTIZE`, but its kernel does not support float16-weight dequantization.

Supported `--quant-granularity` choices for integer formats:

```text
per-channel  recommended for accuracy
per-tensor   simpler shared scale per tensor; supported for int8
```

> **Not offered — verified infeasible on this path (TensorFlow 2.10.1 Keras→TFLite PTQ):**
> `per-group` granularity and a full-`int32` format were evaluated and intentionally
> excluded. Stock TFLite exposes only per-tensor / per-channel granularity (no
> blockwise/group op set), and int32 is not a deployable TFLite type
> (`inference_input/output_type` ∈ {float32, int8, uint8}; int32 is used only for
> internal bias accumulators). Because the portal and `quantize_keras_model.py`
> only accept the formats/granularities above, an unsupported model can never be
> generated or flashed.

Generated deploy artifacts use the selected format suffix, for example:

```text
firmware/pc/artifacts/models/MobileNetV2_finetuned_int8_per-channel.tflite
firmware/pc/artifacts/reports/MobileNetV2_finetuned_int8_per-channel_quantization_report.json
```

Flash only the main board model partition:

```powershell
python firmware\main_board\flash_tflite_model.py "firmware\pc\artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" -p COM6
```

## AI PC Training Portal (Browser GUI)

Students can drive training and quantization from a browser instead of the CLI.
The portal runs on the AI PC and reuses the exact scripts documented above; it
does not reimplement the ML pipeline.

Start the server on the AI PC:

Recommended (HTTPS on port **8080**, so browser Web Serial flashing works):

```bash
conda activate eecampedu
python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https
```

Port **8080 serves HTTPS** when `--https` is set — there is only one port for both
the site and flashing (no separate 8443). `--https` generates a self-signed cert
under `runs/certs/` on first run (reused after). Plain HTTP (drop `--https`) is
still available but does not support browser flashing.

For SSH deployment, run it in the background so the portal stays alive after the
SSH window closes:

```bash
cd ~/EECampEdu
conda activate eecampedu
mkdir -p apps/training_portal/runs
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https \
  > apps/training_portal/runs/server.log 2>&1 &
```

Students open the **`https://`** team URL (the gateway forwards `8081`→AI-PC `:8080`):
**`https://140.112.194.42:8081`** — *not* `http://…`. The self-signed cert makes
the browser show a "Your connection is not private" warning once; students click
**Advanced → Proceed to 140.112.194.42 (unsafe)** and the portal loads.

Check health, inspect logs, or stop the background server:

```bash
curl http://127.0.0.1:8080/api/health
tail -f apps/training_portal/runs/server.log
pkill -f "apps/training_portal/server.py"
```

The classroom gateway forwards each team's public port to its AI PC's `:8080`:

```text
Team 1  -> http://140.112.194.42:8081
Team 2  -> http://140.112.194.42:8082
...
Team 10 -> http://140.112.194.42:8090
```

Current classroom setup: Team 1 is already reachable at
`http://140.112.194.42:8081`, forwarded to that AI PC's `:8080` portal.

For the full classroom network setup — stable AI PC IP, running the portal as a
service, firewall, and the gateway port-forwarding table (`808X` → AI PC `:8080`)
— see [`apps/training_portal/DEPLOYMENT.md`](apps/training_portal/DEPLOYMENT.md).

From the page a student can upload a dataset `.zip`, choose PyTorch or
TensorFlow and a model recipe, start training, start quantization, watch live
job logs, and download artifacts (`.keras`, `.pth`, `.onnx`, `.tflite`,
quantization reports).

### Flashing from the browser (Web Serial)

The ESP32-S3 is plugged into the **student PC**, and flashing happens **entirely
in the browser** using the Web Serial API — students install nothing, run no
Python, and never see `127.0.0.1`. Student flow:

1. Open the team portal over **HTTPS** — e.g. `https://140.112.194.42:8081`
   (not `http://…`) — in **Chrome or Edge**, and click through the one-time
   self-signed certificate warning (**Advanced → Proceed**).
2. Select a `.tflite` model in the **Flash** panel.
3. Click **Connect ESP32-S3** → the browser shows a port picker; choose the board.
4. Click **Flash model**. The browser downloads the `.tflite` from the AI PC
   (via the existing `/api/artifacts/download`) and flashes the model partition
   directly. The job log shows live progress: connected → entering bootloader →
   erasing → writing bytes → verifying → succeeded/failed. The flash offset is
   provided by the backend (`/api/flash-meta`) and never shown.

Flashing is done client-side by **esptool-js** (Espressif's official browser
flasher), vendored offline at
[`apps/training_portal/static/vendor/esptool-js/`](apps/training_portal/static/vendor/esptool-js/)
(pinned 0.5.4; provenance + checksum in its README). The AI PC backend only
lists/serves artifacts and returns flash metadata — no command execution.

> **Secure-context requirement.** Web Serial is a browser security invariant:
> `navigator.serial` exists **only** in a *secure context* — HTTPS, or
> `http://localhost` / `127.0.0.1`. A plain-HTTP non-localhost origin does **not**
> expose it, so flashing is blocked there — no page code can bypass it. Pick one:
>
> 1. **Serve the portal over HTTPS on port 8080 (recommended, default).** Run it
>    with `--port 8080 --https` (see above); students open
>    **`https://140.112.194.42:8081`** and click through the one-time self-signed
>    certificate warning (**Advanced → Proceed**). Zero per-PC setup; then Web
>    Serial just works. (A real cert / TLS-terminating reverse proxy avoids the
>    warning entirely.)
> 2. **Allowlist this exact origin per PC (works over plain HTTP).** In Chrome/Edge,
>    open `chrome://flags/#unsafely-treat-insecure-origin-as-secure`
>    (Edge: `edge://flags/...`), add `http://140.112.194.42:8081`, set it
>    **Enabled**, and **Relaunch**. This grants that origin secure-context status
>    and turns on Web Serial over HTTP. The portal's Flash panel detects the
>    HTTP case and shows these exact steps (with a **Copy** button for the origin).
>    It is a one-time per-student-PC setting and **may be blocked by managed/school
>    browser policy** — if so, use option 1 or the fallback below.
> 3. **CLI/helper fallback** (below) where neither is possible.

Browsers without Web Serial (e.g. Firefox/Safari) show:
*"Web Serial is not supported in this browser. Please use Chrome or Edge."*

**Fallback only (not part of the student flow).** `apps/local_flash_helper/` still
exists as a manual, last-resort flasher for environments where browser Web Serial
is unavailable (no HTTPS and no Chromium browser). It is run by hand and is not
wired into the portal — see
[`apps/local_flash_helper/README.md`](apps/local_flash_helper/README.md).

## Arbitrary Class Names & Robot-Action Mapping

Students may upload their own **six** gesture folders with **any names** (e.g.
`n1..n6`) — the fixed `up/ok/thumb/palm/rock/stone` set is only a fallback.

How it flows through the pipeline:

1. **Import** (portal step 1): the upload must contain exactly six class folders
   (any names). The discovered class **order** is saved to
   `model_finetune/dataset/class_mapping.json` (git-ignored). Wrong counts are
   rejected.
2. **Map to actions** (portal step 1b): assign each class an output action —
   `up / down / left / right / clamp / release` — saved into the same file.
3. **Train**: `train_mobilenet.py` / `train_mini_resnet.py` / the PyTorch trainer
   read the saved class order (`model_finetune/class_map.py`, fallback to their
   defaults if no dataset was imported). The model head size follows the class
   count.
4. **Quantize**: the class order is written into the quantization report
   (`class_order`), so calibration/validation use the right labels.
5. **Benchmark**: `run_benchmark_png.py` reads `class_order` from the report (and
   falls back to `class_mapping.json`) for label accuracy.
6. **Firmware**: the ESP32-S3 uses **only the output index + scores** — no class
   names are compiled in. The index→action mapping is applied on the student PC.

The mapping file schema is documented in `model_finetune/class_mapping.example.json`.

## Student-PC Camera / Control App

Live camera preview, reading the inference `RESULT` line, and driving the robot
arm all need the board's USB port, which is on the **student PC** — so they run
locally, never on the AI PC:

```bash
conda activate eecampedu
python apps/local_camera_app/preview_app.py   # http://127.0.0.1:8770
```

It lists serial ports, connects to main board (and optionally control board), maps the predicted
index → class → action via `class_mapping.json`, and forwards the action to control board.
Live JPEG-over-USB-CDC preview is provided by the native app
[`apps/esp32_cam_input_app`](apps/esp32_cam_input_app/); see
[`apps/local_camera_app/README.md`](apps/local_camera_app/README.md) for the
three-app split (AI PC portal · student flash helper · student camera/control).

## AI PC Full-Test Workflow (Linux)

End-to-end path for validating the AI PC deployment on a Linux AI PC. The portal
and flash helper are pure-Python and run the same on Linux and Windows; only the
shell syntax differs (use forward-slash paths and `python`).

### 1. Clone and switch to the `aipc` branch

```bash
git clone https://github.com/JuuuuuJu/EECampEdu.git
cd EECampEdu
git checkout aipc
```

### 2. Set up the Linux Python environment

The portal only needs the Python deploy stack, not the native ImGui/SDL3
packages (those are for the Windows desktop app), so use `--skip-native`:

```bash
python scripts/setup_env.py --skip-native
conda activate eecampedu
```

Notes for a fresh Linux miniconda:

- If conda reports *"Terms of Service have not been accepted"* for the default
  Anaconda channels, either accept them
  (`conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main`
  and `.../pkgs/r`) **or** create the env from conda-forge:
  `conda create -n eecampedu -c conda-forge --override-channels python=3.10`,
  then `python -m pip install -r firmware/pc/requirements.txt`.
- The pinned `tensorflow==2.10.1` / `torch==2.2.2` wheels are CPU builds and
  install on Linux for Python 3.10.

Quick dependency check:

```bash
python -c "import flask, serial, esptool, numpy, PIL; print('portal deps OK')"
python -c "import tensorflow as tf; print('tf', tf.__version__)"
```

### 3. Start the AI PC training portal

HTTPS on port 8080 (recommended — enables browser flashing):

```bash
python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https
```

Background form for classroom deployment:

```bash
mkdir -p apps/training_portal/runs
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https \
  > apps/training_portal/runs/server.log 2>&1 &
```

Smoke-check from another shell on the AI PC (`-k` accepts the self-signed cert):

```bash
curl -sk https://127.0.0.1:8080/api/health
curl -sk https://127.0.0.1:8080/api/recipes
```

Students reach it through the gateway port for their team (`8081`–`8090`). To
expose `:8080` at `140.112.194.42:808X`, follow
[`apps/training_portal/DEPLOYMENT.md`](apps/training_portal/DEPLOYMENT.md)
(stable IP, run-as-service, firewall, and the gateway port-forward table).

### 4. Import or verify dataset structure

The training scripts read `model_finetune/dataset/train/<class>/*.jpg` for the six
classes recorded in `model_finetune/dataset/class_mapping.json`. Verify the layout:

```bash
for c in $(python -c "import json;print(*json.load(open('model_finetune/dataset/class_mapping.json'))['class_order'])" 2>/dev/null); do \
  echo "$c: $(ls model_finetune/dataset/train/$c 2>/dev/null | wc -l) images"; done
```

To import a new dataset, upload **one `.zip`** (exactly six class folders, any
names) from the portal's **Import dataset** panel, or `POST` it. The layout is
auto-detected: an existing `train/`+`validation/` split is kept as-is, otherwise
the six folders are auto-split into train/validation:

```bash
curl -F "file=@dataset.zip" http://127.0.0.1:8080/api/dataset/upload
```

### 5. Train a source model from the web GUI

In the portal: choose **Framework** (TensorFlow/PyTorch), pick a **recipe**
(MobileNetV2 recommended), then **Start training** and watch the live log.
Equivalent API call:

```bash
curl -s -X POST http://127.0.0.1:8080/api/train \
  -H 'Content-Type: application/json' \
  -d '{"recipe":"tf_mobilenet","epochs":15}'
```

Trained source models land in `model_finetune/models/tf/*.keras` (TensorFlow) and
`model_finetune/models/pytorch/*` (PyTorch). A CLI dry check without training:
`python model_finetune/train_mobilenet.py --check-only`.

### 6. Quantize / export a deployable TFLite from the web GUI

In the portal's **Quantize** panel choose a source `.keras`, format
(`int8` recommended) and granularity, then **Start quantization**. Equivalent:

```bash
curl -s -X POST http://127.0.0.1:8080/api/quantize \
  -H 'Content-Type: application/json' \
  -d '{"model_name":"MobileNetV2_finetuned","quant_format":"int8","quant_granularity":"per-channel"}'
```

Underlying CLI (also runnable directly):

```bash
python firmware/pc/tools/quantize_keras_model.py \
  --quant-format int8 --quant-granularity per-channel
```

### 7. Download or select the generated artifact

List and download from the portal's **Generated artifacts** table, or:

```bash
curl -s http://127.0.0.1:8080/api/artifacts
curl -s -OJ "http://127.0.0.1:8080/api/artifacts/download?path=firmware/pc/artifacts/models/MobileNetV2_finetuned_int8_per-channel.tflite"
```

### 8. Flash path A — ESP32-S3 connected to the AI PC (direct)

If the board is plugged into the AI PC itself, flash the model partition
directly with the project script (offset handled by the script):

```bash
python firmware/main_board/flash_tflite_model.py \
  firmware/pc/artifacts/models/MobileNetV2_finetuned_int8_per-channel.tflite -p /dev/ttyUSB0
```

(Linux serial ports are typically `/dev/ttyUSB0` or `/dev/ttyACM0`; ensure your
user can access them — see the dialout note in step 12.)

### 9. Flash path B — ESP32-S3 connected to the student PC (browser Web Serial)

The usual classroom case, done entirely in the browser (no install). On the
**student PC**, in **Chrome or Edge**, open the team portal (which must be served
over **HTTPS** — Web Serial needs a secure context), then in the **Flash** panel:

1. Select the `.tflite` model.
2. Click **Connect ESP32-S3** and choose the board in the browser's port picker.
3. Click **Flash model** — the browser downloads the artifact and flashes it,
   showing live progress (connect → bootloader → erase → write → verify → done).

No Python and no local helper. See *Flashing from the browser (Web Serial)* above.

CLI fallback (only where Web Serial is unavailable — e.g. no HTTPS and no
Chromium browser), run manually on the machine with the board:

```bash
python -m esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
  write-flash --flash-size keep 0x310000 model.tflite
```

### 10. Benchmark / validation commands

Portal-independent checks that run on the AI PC:

```bash
# Quantization already validates the source .keras and the int8 TFLite against
# the validation set and writes a report:
ls firmware/pc/artifacts/reports/*.json

# PC-side benchmark / cross-checks (see the Benchmark section below):
python firmware/pc/benchmark/run_benchmark_png.py --help
python firmware/pc/tools/crosscheck_quantized.py --help
```

ESP-IDF firmware build (only if ESP-IDF is already installed and `idf.py` is on
PATH):

```bash
idf.py --version
cd firmware/main_board && idf.py set-target esp32s3 && idf.py build
```

### 11. Expected outputs and success criteria

| Step | Expected output | Success criteria |
|------|-----------------|------------------|
| Env | `portal deps OK`, `tf 2.10.1` | imports succeed |
| Portal start | `listening : http://0.0.0.0:8080` | `/api/health` returns `{"status":"ok"}` |
| Dataset | per-class image counts > 0 | all 6 classes present |
| Train | `.keras` under `model_finetune/models/` | job status `succeeded`, reload check passes |
| Quantize | `*_int8_per-channel.tflite` + `*_quantization_report.json` | report accuracy above threshold; job `succeeded` |
| Artifacts | file listed in `/api/artifacts` | download returns the bytes |
| Flash | `Hash of data verified.` from esptool | esptool returncode `0` |
| ESP-IDF build | `Project build complete` | `firmware/main_board/build/*.bin` produced |

### 12. Known hardware / network / browser-dependent steps

These cannot be fully validated without the classroom hardware/network:

- **ESP32-S3 board** — actual flashing, serial-port enumeration, and on-device
  inference require the physical board. On Linux, serial access usually needs the
  user in the `dialout` group (`sudo usermod -aG dialout $USER` then re-login) —
  a one-time admin step, not run automatically here.
- **Gateway port forwarding** — the `8081`–`8090` → AI PC `:8080` mapping is
  configured on the classroom gateway, outside this repo.
- **Browser → localhost helper** — the portal page (gateway origin) calling
  `http://127.0.0.1:8765` is cross-origin + public→localhost. Works with both
  sides on plain `http`; blocked if the portal is served over `https`
  (mixed content) or if a browser/enterprise policy forbids public→localhost
  (use the step-9 CLI fallback). The helper already sends the Chrome Private
  Network Access preflight header.
- **ESP-IDF** — firmware build/flash needs a separate ESP-IDF install with
  `idf.py` on PATH; it is not part of the Python portal environment.

## Benchmark

main board firmware mode:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Benchmark inference only:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

Benchmark plus control board robotic arm output:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6 --control-board-port COM7
```

Port meaning:

```text
--port      main board inference board
--control-board-port control board servo board
```

## Real Camera / Output Runs

Python camera controller path:

```powershell
$env:MAIN_BOARD_PORT="COM6"
$env:CONTROL_BOARD_PORT="COM7"
python firmware\pc\tools\camera_controller.py
```

ImGui app path:

```powershell
python scripts\build_input_app.py
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

In the app:

```text
USB CDC Port      -> main board COM port
Control Board Output Port  -> control board COM port
Auto-forward RESULT checked
Gesture -> Output Mapping set in the Control Board Output panel
```

The browser-driven version of steps 1–4 (upload → class mapping → train →
quantize → flash) is in **AI PC Full-Test Workflow** above; the full chain is
**train → quantize → flash → benchmark → camera preview / gesture output**.
