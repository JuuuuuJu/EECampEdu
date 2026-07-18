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

## What it does

The portal is a thin, safe launcher around scripts that already live in the repo
— it does **not** reimplement any ML:

| GUI action | Underlying project script |
|------------|---------------------------|
| Train · MobileNetV2 (TensorFlow) | `model_finetune/train_mobilenet.py` |
| Train · Mini ResNet (TensorFlow) | `model_finetune/train_mini_resnet.py` |
| Train · Mini ResNet (PyTorch) | `model_finetune/pytorch/train_mini_resnet.py` |
| Quantize | `firmware/pc/tools/quantize_keras_model.py` |

Features: upload a dataset `.zip`, pick framework + recipe, start training,
start quantization, watch **live job logs**, and list/download generated
artifacts (`.keras`, `.pth`, `.onnx`, `.tflite`, quantization report `.json`).

## Run (on the AI PC)

```bash
conda activate eecampedu
python apps/training_portal/server.py --host 0.0.0.0 --port 8000
```

The AI PC listens on `0.0.0.0:8000`; the classroom gateway forwards each team's
public port (8081–8090) to its AI PC's `:8000`. Students then open their team URL.

For the full classroom network setup (stable IP, run-as-service, firewall, and
the gateway port-forwarding table), see [`DEPLOYMENT.md`](DEPLOYMENT.md).

## Dataset zip layout

One folder per gesture class (a single wrapper folder is also accepted):

```
dataset.zip
├── up/     *.jpg
├── ok/     *.jpg
├── thumb/  *.jpg
├── palm/   *.jpg
├── rock/   *.jpg
└── stone/  *.jpg
```

Uploads are extracted into `model_finetune/dataset/train/` (or `validation/`).
Class order must stay `up, ok, thumb, palm, rock, stone`.

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
| POST | `/api/dataset/upload` | Upload+import dataset zip (`file`, `target`) |
| POST | `/api/train` | Start training (`recipe`, optional `epochs`/`batch_size`/`alpha`/`export_onnx`) |
| POST | `/api/quantize` | Start quantization (`model_name`, `quant_format`, `quant_granularity`) |
| GET | `/api/jobs` | Job history |
| GET | `/api/jobs/<id>/log?offset=N` | Incremental log |
| GET | `/api/artifacts` | List generated artifacts |
| GET | `/api/artifacts/download?path=…` | Download an artifact |

## Flashing

The ESP32-S3 is plugged into the **student PC**, not the AI PC, so flashing is
done by the companion localhost helper — see
[`apps/local_flash_helper/`](../local_flash_helper/). The portal's "Flash"
panel calls that helper on `127.0.0.1:8765`.
