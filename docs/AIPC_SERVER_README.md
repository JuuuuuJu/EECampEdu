# AI PC Server Guide — teaching staff

The AI PC runs the **training portal** (train / quantize / serve artifacts +
firmware for browser flashing). One AI PC per team; the classroom gateway
forwards `140.112.194.42:808X` → that AI PC's `:8080`.

## 1. Environment (once)

```bash
python scripts/setup_env.py --skip-native     # creates the 'eecampedu' conda env
conda activate eecampedu
```

If conda blocks on Anaconda channel Terms of Service, either accept them or use
conda-forge: `conda create -n eecampedu -c conda-forge --override-channels python=3.10`
then `python -m pip install -r firmware/pc/requirements.txt`.

## 2. Background service — training portal server

Serve **HTTPS on port 8080** (browser Web Serial flashing needs a secure context):

```bash
conda activate eecampedu
# foreground:
python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https
# background (survives the SSH session):
mkdir -p apps/training_portal/runs
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8080 --https \
  > apps/training_portal/runs/server.log 2>&1 &
```

`--https` self-signs a cert under `runs/certs/` on first run (students accept the
warning once). Drop `--https` for plain HTTP (no browser flashing).

| | |
|---|---|
| Health | `curl -sk https://127.0.0.1:8080/api/health` |
| Log | `apps/training_portal/runs/server.log` (+ per-job logs in `runs/jobs/<id>/`) |
| Stop | `pkill -f "training_portal/server.py"` |
| Restart | stop, then re-run the nohup command |

Reboot-proof (optional): a `systemctl --user` unit — see
`apps/training_portal/DEPLOYMENT.md`.

## 3. Train / quantize

Do it from the portal **Deploy** tab, or via API/CLI:

```bash
# train (background job in the portal); direct CLI equivalent:
python model_finetune/train_mobilenet.py
# quantize a trained .keras to int8 TFLite + report:
python firmware/pc/tools/quantize_keras_model.py --quant-format int8 --quant-granularity per-channel
```

Outputs: `.keras` under `model_finetune/models/`, deploy `.tflite` + report under
`firmware/pc/artifacts/`. The portal lists/serves these for browser flashing.

### On-device benchmark (server-side)

The Deploy tab's **Run benchmark** button starts an allowlisted background job
(`firmware/pc/benchmark/run_benchmark_png.py`) with UI-selected model, dataset,
and serial port; the live log streams to the portal and a summary (accuracy,
latencies, throughput, score similarity) is shown. It runs **on the AI PC**, so
the ESP32-S3 must be connected to the **AI PC's** USB — if the board is on a
student PC, the portal reports that no AI-PC serial port is available (there is no
student-PC benchmark helper; only browser flashing works from the student PC).
Endpoints: `GET /api/serial-ports`, `GET /api/benchmark/options`, `POST /api/benchmark`.

## 4. Provide firmware artifacts for flashing

The portal flashes ESP-IDF **build output** over Web Serial; build once on the AI
PC (needs ESP-IDF, e.g. `get_idf` / `source /opt/esp/idf/export.sh`):

```bash
cd firmware/main_board          && idf.py set-target esp32s3 && idf.py build   # Deploy / Main-board tab
cd firmware/teaching_output_demo && idf.py set-target esp32s3 && idf.py build   # Output demo tab
```

The portal reads each `build/flasher_args.json`; if missing it shows
“…build … first.” Offsets are never shown to students (developer-mode toggle
reveals them).

## 5. Student-PC helper services (not on the AI PC)

These Python services run on the **student PC** (they need the board's USB port),
not the AI PC. They are optional — the portal flashes from the browser directly.

**Camera / control app** (`apps/local_camera_app/preview_app.py`, port 8770):

| | |
|---|---|
| Start | `python apps/local_camera_app/preview_app.py` |
| Background | `nohup python apps/local_camera_app/preview_app.py > ~/camera_app.log 2>&1 &` |
| Health | `curl http://127.0.0.1:8770/health` |
| Log | `~/camera_app.log` |
| Stop | `pkill -f local_camera_app/preview_app.py` |

**Flash helper** (`apps/local_flash_helper/flash_helper.py`, port 8765) — **fallback
only**, for PCs where browser Web Serial is unavailable:

| | |
|---|---|
| Start | `python apps/local_flash_helper/flash_helper.py` |
| Health | `curl http://127.0.0.1:8765/health` |
| Stop | `pkill -f local_flash_helper/flash_helper.py` |

## Notes

- No arbitrary shell execution: the portal only runs an allowlist of project
  scripts, and only lists/serves artifacts + firmware images.
- Datasets, models, build output, and `runs/` are git-ignored.
- Full classroom networking (gateway port-forwarding, HTTPS): see
  `apps/training_portal/DEPLOYMENT.md`.
