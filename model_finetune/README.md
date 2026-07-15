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
    tf/             TensorFlow/Keras source models (.keras) and weights (.h5)
    pytorch/        PyTorch weights (.pth) and exported ONNX models (.onnx)
  pytorch/          PyTorch versions of training and demonstration scripts
    train_mini_resnet.py PyTorch training (96x96, grayscale, 6-class)
    webcam_demo.py  PC webcam demo using OpenCV DNN to load and test ONNX models
  train_mini_resnet.py Keras/TensorFlow Mini ResNet training (96x96, grayscale, 6-class)
  train_mobilenet.py Keras/TensorFlow MobileNetV2 training (96x96, grayscale, 6-class)
  webcam_demo.py    Optional PC camera demo (Keras/TensorFlow version)
```

## Class Order

```text
up, ok, thumb, palm, rock, stone (6 classes)
```

Keep this order aligned across the training scripts, local datasets, and the webcam demo tool.

## Train Models

### MobileNetV2 version (Recommended for best accuracy/speed)

Run from this folder:

```powershell
cd model_finetune
python train_mobilenet.py   # Keras Transfer Learning (6-class MobileNetV2)
```

This trains a MobileNetV2 model using Keras/TensorFlow. It is highly recommended due to its superior accuracy and execution speed on target devices.

### Mini ResNet version (Alternative)

You can train a Mini ResNet base model using either the PyTorch or Keras version:

#### PyTorch version

```powershell
cd model_finetune
python pytorch/train_mini_resnet.py # PyTorch Transfer Learning (6-class ResNet)
```

During execution, the PyTorch training script will automatically convert existing Keras weights, complete classification head warmup, full fine-tuning, and save models under `models/pytorch/`.

#### Keras version (Legacy)

```powershell
cd model_finetune
python train_mini_resnet.py # Keras Transfer Learning (6-class ResNet)
```

The training script reads images from `dataset/train/`, converts them to
grayscale, resizes them, normalizes pixels to `[0.0, 1.0]`, and
splits the training set internally for validation.

The important handoff artifacts are:

```text
models/tf/<model_name>.keras
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
deployment. The demo loads Keras source `.keras` models directly from `models/tf/`.
