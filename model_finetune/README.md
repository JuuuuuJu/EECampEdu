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
    *.keras, *.onnx Source models (Keras & ONNX)
  train.py, train_96.py, train_128.py
  webcam_demo.py    Optional PC camera demo
```

## Class Order

```text
up, down, right, left, null  (Note: train_96.py and train_128.py use 4 classes: up, down, right, left)
```

Keep this order aligned with firmware and benchmark tools.

## Train Models

Run from this folder (choose the training script for your desired model/size):

```powershell
cd model_finetune
python train.py            # Main CNN/MobileNet (5-class)
python train_96.py         # Transfer Learning 96x96 (4-class)
python train_128.py        # Transfer Learning 128x128 (4-class)
```

The training script reads images from `dataset/train/`, converts them to
grayscale, resizes them to `96 x 96`, normalizes pixels to `[0.0, 1.0]`, and
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

```powershell
python webcam_demo.py
```

This is kept only for PC-side visual testing. It is not required by firmware
deployment. The demo loads firmware-generated TFLite models from
`../firmware/pc/artifacts/models/`, so run the firmware quantization tool first
if those generated files are not present.
