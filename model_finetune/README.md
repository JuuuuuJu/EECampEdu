# Model Fine-Tune

This folder keeps only the files needed to train gesture models and hand off
`.keras` source models to the firmware deploy pipeline.

## Folder Structure

```text
model_finetune/
  dataset/
    train/          Training images used by train.py
    validation/     Preserved validation set for benchmark / manual checks
  models/
    *.keras         Source Keras models for firmware-side int8 quantization
  train.py
  webcam_demo.py    Optional PC camera demo
```

## Class Order

```text
up, down, right, left, null
```

Keep this order aligned with firmware and benchmark tools.

## Train Models

Run from this folder:

```powershell
cd model_finetune
python train.py
```

The training script reads images from `dataset/train/`, converts them to
grayscale, resizes them to `96 x 96`, normalizes pixels to `[0.0, 1.0]`, and
splits the training set internally for validation.

The important handoff artifact is:

```text
models/<model_name>.keras
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
