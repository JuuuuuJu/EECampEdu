# Model Unit Test

目標：確認 model team 訓練出的 PyTorch 或 TensorFlow source model 可以在 PC 上正確分類，並能交給 deploy team。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | No |
| ESP32-S3 | No |
| Deploy model | No |
| Robotic arm | No |
| PC camera | Optional |

## Inputs

```text
model_finetune/dataset/train/
model_finetune/dataset/validation/
model_finetune/models/
```

Class order must be documented:

```text
up, down, right, left, null
```

If the model only supports four classes, explicitly mark:

```text
up, down, right, left
```

## Run PyTorch Training

```powershell
cd model_finetune
python pytorch\train_96.py
```

Optional 128x128 model:

```powershell
python pytorch\train_128.py
```

## Run TensorFlow Training

```powershell
cd model_finetune
python train_96.py
python train_128.py
```

## Run PC Webcam Demo

```powershell
cd model_finetune
python pytorch\webcam_demo.py
```

TensorFlow/Keras demo:

```powershell
python webcam_demo.py
```

## Expected Result

- Training finishes without exception.
- Exported model files appear under `model_finetune/models/`.
- Validation accuracy is printed by the training script.
- Webcam demo shows live prediction.
- Model team reports model framework, model input size, class order, preprocessing, and source artifact path.

## Pass Criteria

- Source model is reproducible from scripts.
- Class mapping is unambiguous.
- Model input contract is clear enough for deploy conversion into unified int8 TFLite.
