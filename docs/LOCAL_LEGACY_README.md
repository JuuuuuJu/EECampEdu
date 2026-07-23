# Local / Legacy Workflow

Use this only when the AI PC portal is unavailable or a developer needs direct CLI control.

## Environment

```powershell
conda activate eecampedu
pip install -r requirements.txt
```

For ESP-IDF commands, open an ESP-IDF enabled shell first.

## Quantize A Keras Model

```powershell
python firmware\pc\tools\quantize_keras_model.py --keras model_finetune\models\tf\MobileNetV2_finetuned.keras --quant-format int8 --quant-granularity per-channel
```

The output `.tflite` goes to `firmware\pc\artifacts\models\` and can be flashed to the model partition.

## Flash Model Partition

```powershell
python firmware\esp\flash_tflite_model.py firmware\pc\artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite -p COM5
```

This writes the `.tflite` bytes directly to the model partition. It does not build or convert a model `.bin`.

## Benchmark

Flash `firmware\deploy_benchmark` first. Then run:

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model artifacts\models\MobileNetV2_finetuned_int8_per-channel.tflite --dataset ..\..\model_finetune\dataset\validation --port COM5
```

Expected report includes:

- Label accuracy
- Average model latency
- Preprocess latency
- Device compute latency
- Throughput
- Top-1 match
- Score MAE
- Max score error
- Cosine similarity

## Camera Debug

Use `firmware/camera_usb_demo` for camera debug. Do not use `firmware/deploy_benchmark` because it has no OV2640 camera path.
