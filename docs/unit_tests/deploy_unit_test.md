# Deploy Unit Test

目標：確認 deploy team 可以把 PyTorch/TensorFlow model handoff 轉成統一 int8 TFLite artifact，燒進 ESP32-S3，並用 benchmark 驗證 accuracy、latency、throughput、output similarity。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | No |
| ESP32-S3 | Yes |
| Deploy model | Yes |
| Robotic arm | No |
| PC camera | No |

## Source Framework Contract

```text
PyTorch source    -> .pth / .onnx / deploy-compatible .keras handoff
TensorFlow source -> .keras
ESP deploy target -> *_int8.tflite
```

Current firmware maps a `.tflite` model from flash and runs it with TFLite Micro.

## Firmware Mode

Edit:

```text
firmware/esp/main/include/model_config.hpp
```

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kTestUartFrame;
```

## Build And Flash Firmware

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

## Flash Model Partition

```powershell
cd EECampEdu
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\Separable_CNN_int8.tflite" -p COM6
```

## Run Benchmark

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Separable_CNN_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

## Metrics

- `Label Accuracy`: label-level correctness.
- `Average Model Latency`: pure model inference time on ESP32-S3.
- `Average Preprocess Latency`: crop/resize/scale time on ESP32-S3.
- `Device Compute Throughput`: model + preprocess FPS.
- `Top-1 Match`: ESP prediction equals PC reference prediction.
- `Average Score MAE`: average int8 score difference between ESP and PC reference.
- `Max Score Error`: largest int8 score difference.
- `Average Cosine`: vector similarity between ESP scores and PC reference scores.

## Expected Result

- ESP prints `READY`.
- Benchmark processes all images.
- Output similarity is enabled.
- `Top-1 Match` should be close to 100% if PC reference and ESP preprocessing match.
- UART end-to-end FPS can be low because raw frame transfer is slow; use model-only/device-compute throughput for deploy performance.
