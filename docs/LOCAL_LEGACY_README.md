# Local Legacy Guide

This guide is for developers who want to run everything manually on one PC.
Students should use the AI PC portal instead.

## Environment

```bash
python scripts/setup_env.py
conda activate eecampedu
```

ESP-IDF must be installed separately for firmware builds.

## Train

```bash
python model_finetune/train_mobilenet.py
# or
python model_finetune/train_mini_resnet.py
# or
python model_finetune/pytorch/train_mini_resnet.py
```

Dataset layout:

```text
model_finetune/dataset/train/<class>/*.{jpg,jpeg,png,bmp}
model_finetune/dataset/validation/<class>/*.{jpg,jpeg,png,bmp}
```

Class folder names may be custom. The class order/action mapping is stored in `model_finetune/dataset/class_mapping.json`.

## Quantize

```bash
python firmware/pc/tools/quantize_keras_model.py --quant-format int8 --quant-granularity per-channel
```

Deployable formats currently offered by the portal: `int8`, `int16`, `float32`.

## Build And Flash Main Board Firmware

```bash
cd firmware/main_board
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor
```

## Flash Only The Model Partition

```bash
python firmware/main_board/flash_tflite_model.py firmware/pc/artifacts/models/<model>.tflite -p COM6
```

## Benchmark

```bash
python firmware/pc/benchmark/run_benchmark_png.py --model firmware/pc/artifacts/models/<model>.tflite --dataset model_finetune/dataset/validation --port COM6
```

Metrics include label accuracy, latency, throughput/FPS, Top-1 match, MAE, Max Error, and Cosine Similarity.

## Output Demo

```bash
cd firmware/teaching_output_demo
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor
```

Commands: `LED,1`, `LED,0`, `PWM,<0-255>`, `BLINK`, `STOP`, `STATUS`.

## Model Webcam Demo

```bash
python model_finetune/pytorch/webcam_demo.py
```

This is a developer/local test path. It is not the student portal workflow.