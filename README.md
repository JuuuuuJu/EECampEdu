# EECampEdu

This repository is split into two integration areas:

```text
EECampEdu/
  model_finetune/   Training, fine-tuning, datasets, and source `.keras` models.
  firmware/         ESP32-S3 firmware, deploy quantization, flashing, and benchmark tools.
```

All paths are resolved relative to the repository location. After cloning, do
not edit scripts to match a local drive letter.

## Workflow

```text
model_finetune/models/*.keras
  -> firmware deploy quantization with representative calibration images
  -> firmware/pc/artifacts/models/*_int8.tflite
  -> flash ESP32-S3 model partition
  -> run camera or UART benchmark inference
```

## Windows PC Setup

Use PowerShell with conda:

```powershell
cd EECampEdu
python firmware\scripts\setup_pc_env.py
conda activate eecampedu
```

## Quantize A Keras Model

Default inputs:

```text
model_finetune/models/<model_name>.keras
model_finetune/dataset/train/
```

Command:

```powershell
python firmware\pc\tools\quantize_keras_model.py --model-name Separable_CNN
```

Generated deploy artifacts:

```text
firmware/pc/artifacts/models/Separable_CNN_int8.tflite
firmware/pc/artifacts/reports/Separable_CNN_quantization_report.json
```

## Build Firmware

```powershell
cd firmware\esp
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

## Flash Model Only

From repository root:

```powershell
python firmware\esp\flash_tflite_model.py "firmware\pc\artifacts\models\Separable_CNN_int8.tflite" -p COM6
```

## Benchmark

Firmware must run `RuntimeMode::kTestUartFrame`.

```powershell
cd firmware\pc
python -u benchmark\run_benchmark_png.py --model "artifacts\models\Separable_CNN_int8.tflite" --dataset "..\..\model_finetune\dataset\validation" --port COM6
```

See `firmware/README.md` for firmware runtime modes, flash layout, camera
integration, and cross-team details.
