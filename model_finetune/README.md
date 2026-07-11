# Model Fine-Tune

This folder keeps only the files needed to train gesture models and hand off
`.keras` source models to the firmware deploy pipeline.

## Folder Structure

```text
model_finetune/
  dataset/
    train/          Training images used by the training scripts
    validation/     Preserved validation set for benchmark / manual checks
  models/
    *.keras, *.onnx, *.pth Source models (Keras, ONNX & PyTorch)
  pytorch/          PyTorch versions of training and demonstration scripts
    train_96.py     PyTorch training 96x96 (4-class)
    train_128.py    PyTorch training 128x128 (4-class)
    webcam_demo.py  PC webcam demo using OpenCV DNN to load and test ONNX models
  train_96.py, train_128.py (Legacy Keras versions)
  webcam_demo.py    Optional PC camera demo (Legacy Keras version)
```

## Class Order

```text
up, down, right, left, null  (Note: train_96.py and train_128.py use 4 classes: up, down, right, left)
```

Keep this order aligned with firmware and benchmark tools.

## Train Models

### PyTorch version (Recommended)

Run from this folder:

```powershell
cd model_finetune
python pytorch/train_96.py         # PyTorch Transfer Learning 96x96 (4-class)
python pytorch/train_128.py        # PyTorch Transfer Learning 128x128 (4-class)
```

During execution, the PyTorch training scripts will automatically:
1. Locate `models/mini_resnet_pretrained_weights_96.h5` and convert Keras pre-trained weights to PyTorch `.pth` weights to bypass pre-training download/execution.
2. Complete classification head warmup and full fine-tuning.
3. Save output files to `models/` in three formats: `.pth` (PyTorch weights), `.onnx` (ONNX export), and `.keras` (weight-translated Keras model, fully backward-compatible with deploy pipeline).

### Keras version (Legacy)

Run from this folder:

```powershell
cd model_finetune
python train_96.py         # Keras Transfer Learning 96x96 (4-class)
python train_128.py        # Keras Transfer Learning 128x128 (4-class)
```

The training script reads images from `dataset/train/`, converts them to
grayscale, resizes them, normalizes pixels to `[0.0, 1.0]`, and
splits the training set internally for validation.

The important handoff artifacts are:

```text
models/<model_name>.keras
models/<model_name>.onnx
```

Firmware deploy quantization reads these `.keras` files and generates int8
TFLite models under:

```text
../firmware/pc/artifacts/models/
```

## Optional Demo

### ONNX Demo (Recommended for PyTorch)

Run from this folder:

```powershell
python pytorch/webcam_demo.py
```

This runs the demo by loading ONNX models (`.onnx`) directly using OpenCV DNN. It does not require TensorFlow or PyTorch.

### Keras Demo (Legacy)

```powershell
python webcam_demo.py
```

This is kept only for PC-side visual testing. It is not required by firmware
deployment. The demo loads firmware-generated TFLite models from
`../firmware/pc/artifacts/models/` or loads source `.keras` models.

