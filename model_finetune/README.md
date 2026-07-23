# Model Finetune

Training and model preparation live here. Students normally use the portal instead of running these scripts manually.

## Dataset

Recommended dataset layout:

```text
dataset/
  train/
    gesture_a/
      image001.jpg
    gesture_b/
      image001.jpg
  validation/
    gesture_a/
      image101.jpg
    gesture_b/
      image101.jpg
```

The dataset can contain more than 6 class folders. One run uses at most 6 active classes selected in the portal. This lets students collect many gestures and choose which 6 to train or deploy.

Camera captures are normalized to the model input path: resize/crop to `96x96`, grayscale, and scale consistently before training, quantization, and inference.

## Source Frameworks

Source framework can be PyTorch or TensorFlow/Keras. The deploy target is TFLite for the ESP32-S3 firmware.

Typical outputs:

| Source | Intermediate | Deploy |
|---|---|---|
| TensorFlow/Keras | `.keras` | `.tflite` |
| PyTorch | `.pth`, optional `.onnx` / converted model | `.tflite` |

Model outputs are grouped under:

- `model_finetune/models/tf/`
- `model_finetune/models/pytorch/`

Deployable `.tflite` artifacts are copied to `firmware/pc/artifacts/models/` for the portal flashing and benchmark flow.

## Quantization

Supported deploy formats depend on TFLite Micro kernels:

- `int8` per-tensor or per-channel
- `int16` where the converted model and kernels support it
- `float32` for comparison or when size/performance is acceptable

Unsupported or comparison-only formats should not be shown as flashable artifacts.

Quantization reports should include:

- Source model accuracy
- TFLite accuracy
- Prediction distribution
- Output variation
- Quantization format and granularity in the file name

## Webcam Demo

`webcam_demo.py` is a local fallback for PC webcam preview. In the classroom flow, students should prefer the portal and OV2640 capture path.
