# Model Fine-Tune

`model_finetune/` is owned by the model team. It keeps training scripts, datasets, and source model artifacts.

Both source frameworks are supported:

```text
PyTorch source    -> .pth and/or .onnx, optionally translated to .keras for deploy tooling
TensorFlow source -> .keras
Deploy target     -> int8 TFLite generated under firmware/pc/artifacts/models/
```

```text
model_finetune/
  dataset/
    train/          Training images
    validation/     Validation images used by model/deploy checks
    sign_mnist/     Reference dataset
  models/           Source model artifacts; subfolders are created only when artifacts are generated
    tf/             TensorFlow/Keras training outputs
    pytorch/        PyTorch training outputs and optional Keras handoff
  pytorch/          PyTorch training and ONNX webcam demo
```

## Class Contract

Current full-system class order:

```text
up, down, right, left, null
```

Some PyTorch training scripts currently train only the four gesture classes:

```text
up, down, right, left
```

Before deploy, the model and deploy teams must confirm whether `null` is included in the exported source model.

## PyTorch Training Flow

Run from this folder:

```powershell
cd model_finetune
python pytorch\train_96.py
```

The PyTorch `train_96.py` script trains/fine-tunes gesture models and exports artifacts under `models/pytorch/`, including `.pth`, `.onnx`, and deploy-compatible `.keras` handoff files when supported by the script. The folder is created only when an artifact is saved.

## TensorFlow Training Flow

Run from this folder:

```powershell
cd model_finetune
python train_96.py
```

The TensorFlow `train_96.py` script trains/fine-tunes gesture models and saves `.keras` and `.onnx` source artifacts under `models/tf/`. The folder is created only when an artifact is saved.

## Webcam Demo

Use the PyTorch ONNX demo for model-team sanity checks:

```powershell
cd model_finetune
python pytorch\webcam_demo.py
```

This is a PC-side model check. It does not flash firmware and does not replace deploy benchmark.

For TensorFlow/Keras source models:

```powershell
python webcam_demo.py
```

## Deploy Handoff

The model team should place source models under:

```text
model_finetune/models/tf/ or model_finetune/models/pytorch/
```

Deploy consumes the agreed source artifact and generates ESP32 deploy artifacts under:

```text
firmware/pc/artifacts/models/
firmware/pc/artifacts/reports/
```

Do not put deploy-only quantization ownership inside `model_finetune/`. Quantization, flashing, and ESP benchmark belong to `firmware/`.

If the source model is PyTorch-only, export ONNX and/or a Keras-compatible handoff first. ESP32-S3 does not run PyTorch directly in this project; it runs the unified int8 TFLite deploy target.

## Boundary With Output

`model_finetune/` does not control ESP2 servo output. The model team owns source model quality and class order; deploy/firmware owns int8 TFLite conversion, ESP1 flashing, benchmark, and PC-to-ESP2 gesture forwarding.
