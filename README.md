# EECampEdu

EECampEdu is the integrated workspace for the gesture-controlled robotic arm system.

Current integration architecture:

```text
Model source: PyTorch or TensorFlow
Deploy target: TensorFlow Lite, default/recommended int8 TFLite
Inference board: ESP1, ESP32-S3
Output board: ESP2, normal ESP32
PC role: live preview, benchmark, command bridge, and UI
```

ESP1 no longer drives servos directly. ESP1 runs camera/model inference and prints `RESULT,...`; the PC forwards the predicted gesture to ESP2 over a second USB serial link; ESP2 controls the robotic arm servos.

## Repository Layout

```text
EECampEdu/
  model_finetune/   Model training, fine-tuning, datasets, and source model handoff.
  firmware/         ESP1 firmware, ESP2 output firmware, quantization, flashing, benchmark, camera, USB.
  apps/             Windows Dear ImGui / SDL3 integration app, AI PC training portal (browser GUI), and student-PC flash helper.
  docs/unit_tests/  Unit-test guides for model, deploy, camera, USB, input, and output teams.
```

All paths are relative to this repository root. Do not hardcode local drive letters in committed scripts.

## System Flow

```text
Model team
  -> train / fine-tune in PyTorch or TensorFlow
  -> export source artifacts under model_finetune/models/

Deploy team
  -> export source model into deployable TFLite
  -> flash ESP1 model partition
  -> validate accuracy, latency, throughput, and output similarity

ESP1 ESP32-S3
  -> OV2640 capture
  -> grayscale / crop / resize / scale
  -> TFLite Micro inference
  -> print RESULT,<class>,<model_us>,<preprocess_us>,<device_us>,<scores...>

PC
  -> receives ESP1 result
  -> benchmark and/or UI live preview
  -> maps each gesture class to a user-selected output action

ESP2 normal ESP32
  -> receives ACTION,up/down/left/right/clamp/release/none over second USB serial
  -> drives robotic arm servos
```

## Team Responsibilities

| Team | Main folder | Main responsibility |
| --- | --- | --- |
| Model | `model_finetune/` | Train/fine-tune gesture models and provide source artifacts. |
| Deploy | `firmware/` | Convert PyTorch/TensorFlow handoff into ESP-deployable TFLite, flash, benchmark, and validate ESP behavior. |
| Camera | `firmware/esp/main/src/camera_capture_ov2640.cpp` | OV2640 capture and frame format/resolution control on ESP1. |
| USB | `firmware/esp/main/src/usb_composite.cpp` | ESP1 USB CDC command stream and USB MSC storage exposure. |
| Input | `apps/`, `firmware/esp/main/src/input_controls.cpp` | PC UI, rotary encoder/button controls, camera parameters. |
| Output | `firmware/esp2_output/`, `firmware/pc/tools/send_esp2_gesture.py` | ESP2 ESP-IDF servo control and PC-to-ESP2 gesture bridge. |

## Windows Environment

Use PowerShell with conda:

```powershell
cd EECampEdu
python scripts\setup_env.py
conda activate eecampedu
```

The setup script installs Python deploy dependencies and native packages for the ImGui/SDL3 PC app. ESP-IDF is installed separately and must be available in the terminal where `idf.py` is used.

## Build ESP1 Firmware

ESP1 is the ESP32-S3 board with camera, USB, and TFLite Micro inference.

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

Runtime mode is selected in:

```text
firmware/esp/main/include/model_config.hpp
```

Common modes:

| Mode | Purpose | Hardware |
| --- | --- | --- |
| `RuntimeMode::kTestUartFrame` | PC benchmark sends test frames over UART/CDC. | ESP1 only |
| `RuntimeMode::kCameraFlash` | OV2640 capture, flash storage, preprocessing, inference. | ESP1 + OV2640 |
| `RuntimeMode::kCameraUsbMsc` | CDC live preview, MSC storage, camera/control integration. | ESP1 + OV2640 + PC app |
| `RuntimeMode::kPhotoFlashTest` | Preloaded-photo flash test without camera. | ESP1 only |
| `RuntimeMode::kInputOutputSelfTest` | ESP1 input logging and output-route log only. | ESP1 input hardware |

## Build ESP2 Output Firmware

ESP2 is a separate normal ESP32 board dedicated to robotic arm servo output.

```powershell
cd firmware\esp2_output
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

Manual ESP2 test:

```powershell
python firmware\pc\tools\send_esp2_gesture.py --port COM7 up
python firmware\pc\tools\send_esp2_gesture.py --port COM7 right --repeat 3
python firmware\pc\tools\send_esp2_gesture.py --port COM7 P100
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
| `int8` | Required | Recommended. Full int8 input/output and int8 weights. Best size/speed target for ESP1. |
| `int16` | Required | Experimental TFLite int16 activations with int8 weights. Currently supports `per-channel` only; verify TFLite Micro operator support per model. |
| `float32` | Not required | No quantization. Useful as a reference export; usually larger/slower on ESP. |

`float16` is intentionally not offered as an ESP deploy option: this ESP TFLite Micro build registers `DEQUANTIZE`, but its kernel does not support float16-weight dequantization.

Supported `--quant-granularity` choices for integer formats:

```text
per-channel  recommended for accuracy
per-tensor   simpler shared scale per tensor; supported for int8
```


Generated deploy artifacts use the selected format suffix, for example:

```text
firmware/pc/artifacts/models/MobileNetV2_finetuned_int8_per-channel.tflite
firmware/pc/artifacts/reports/MobileNetV2_finetuned_int8_per-channel_quantization_report.json
```

Flash only the ESP1 model partition:

```powershell
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" -p COM6
```

## AI PC Training Portal (Browser GUI)

Students can drive training and quantization from a browser instead of the CLI.
The portal runs on the AI PC and reuses the exact scripts documented above; it
does not reimplement the ML pipeline.

Start the server on the AI PC:

```bash
conda activate eecampedu
python apps/training_portal/server.py --host 0.0.0.0 --port 8080
```

For SSH deployment, run it in the background so the portal stays alive after the
SSH window closes:

```bash
cd ~/EECampEdu
conda activate eecampedu
mkdir -p apps/training_portal/runs
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8080 \
  > apps/training_portal/runs/server.log 2>&1 &
```

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

Because the ESP32-S3 is plugged into the **student PC** (not the AI PC),
flashing uses a small localhost helper on the student PC:

```bash
conda activate eecampedu
python apps/local_flash_helper/flash_helper.py   # listens on 127.0.0.1:8765
```

The portal's Flash panel calls that helper, which downloads the selected
`.tflite` and runs `python -m esptool` locally (default model-partition offset
`0x310000`). See [`apps/training_portal/README.md`](apps/training_portal/README.md)
and [`apps/local_flash_helper/README.md`](apps/local_flash_helper/README.md).
The web dependency (`Flask`) is part of `firmware/pc/requirements.txt`, so no
separate environment is needed.

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

```bash
python apps/training_portal/server.py --host 0.0.0.0 --port 8080
```

Background form for classroom deployment:

```bash
mkdir -p apps/training_portal/runs
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8080 \
  > apps/training_portal/runs/server.log 2>&1 &
```

Smoke-check from another shell on the AI PC:

```bash
curl -s http://127.0.0.1:8080/api/health
curl -s http://127.0.0.1:8080/api/recipes
```

Students reach it through the gateway port for their team (`8081`–`8090`). To
expose `:8080` at `140.112.194.42:808X`, follow
[`apps/training_portal/DEPLOYMENT.md`](apps/training_portal/DEPLOYMENT.md)
(stable IP, run-as-service, firewall, and the gateway port-forward table).

### 4. Import or verify dataset structure

The training scripts read `model_finetune/dataset/train/<class>/*.jpg` with class
order `up, ok, thumb, palm, rock, stone`. Verify the layout:

```bash
for c in up ok thumb palm rock stone; do \
  echo "$c: $(ls model_finetune/dataset/train/$c 2>/dev/null | wc -l) images"; done
```

To import a new dataset, upload a `.zip` (one folder per class) from the portal's
**Import dataset** panel, or `POST` it:

```bash
curl -F "file=@dataset.zip" -F "target=train" \
  http://127.0.0.1:8080/api/dataset/upload
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
python firmware/esp/flash_tflite_model.py \
  firmware/pc/artifacts/models/MobileNetV2_finetuned_int8_per-channel.tflite -p /dev/ttyUSB0
```

(Linux serial ports are typically `/dev/ttyUSB0` or `/dev/ttyACM0`; ensure your
user can access them — see the dialout note in step 12.)

### 9. Flash path B — ESP32-S3 connected to the student PC (local helper)

The usual classroom case. On the **student PC** (board plugged in):

```bash
conda activate eecampedu
python apps/local_flash_helper/flash_helper.py   # http://127.0.0.1:8765
```

In the portal's **Flash** panel: set the helper URL, click **Check helper & list
ports**, pick the port and a `.tflite`, then **Flash model**. The helper
downloads the artifact from the portal and runs
`python -m esptool ... write-flash --flash-size keep 0x310000 model.tflite`
locally. CLI fallback without the browser:

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
cd firmware/esp && idf.py set-target esp32s3 && idf.py build
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
| ESP-IDF build | `Project build complete` | `firmware/esp/build/*.bin` produced |

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

ESP1 firmware mode:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

Benchmark inference only:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

Benchmark plus ESP2 robotic arm output:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6 --esp2-port COM7
```

Port meaning:

```text
--port      ESP1 inference board
--esp2-port ESP2 servo output board
```

## Real Camera / Output Runs

Python camera controller path:

```powershell
$env:ESP1_PORT="COM6"
$env:OUTPUT_ESP2_PORT="COM7"
python firmware\pc\tools\camera_controller.py
```

ImGui app path:

```powershell
python scripts\build_input_app.py
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```

In the app:

```text
USB CDC Port      -> ESP1 COM port
ESP2 Output Port  -> ESP2 COM port
Auto-forward RESULT checked
Gesture -> Output Mapping set in the ESP2 Output panel
```

## Unit Tests

Dedicated unit-test guides:

```text
docs/unit_tests/model_unit_test.md
docs/unit_tests/deploy_unit_test.md
docs/unit_tests/camera_unit_test.md
docs/unit_tests/usb_unit_test.md
docs/unit_tests/input_unit_test.md
docs/unit_tests/output_unit_test.md
```

Use one mode at a time for unit tests. Use `RuntimeMode::kCameraUsbMsc` only for full camera + USB + UI integration.

Full integration test order:

```text
1. Train/fine-tune source model in model_finetune/.
2. Export the source model into deployable TFLite under firmware/pc/artifacts/models; use int8 unless there is a specific experiment reason.
3. Build and flash ESP1 firmware.
4. Flash the selected TFLite model into the ESP1 model partition.
5. Build and flash ESP2 output firmware.
6. Run deploy benchmark with --esp2-port to verify accuracy, output similarity, latency, and servo command forwarding.
7. Run camera + USB + ImGui App integration for live preview and real gesture-to-output behavior.
```
