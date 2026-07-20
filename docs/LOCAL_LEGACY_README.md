# Local / Legacy Guide — CLI on a personal PC

Run the full pipeline directly with Python + ESP-IDF, without the AI PC browser
portal. Boards: **main board** (ESP32-S3: camera + USB + TFLite Micro inference)
and **control board** (robot-arm servo output).

## Environment

```bash
python scripts/setup_env.py            # conda env 'eecampedu' + deps
conda activate eecampedu
```

ESP-IDF is separate; make `idf.py` available (`get_idf` / `source .../export.sh`).

## 1. Train a source model

```bash
cd model_finetune
python train_mobilenet.py              # recommended: MobileNetV2 (Keras)
# alternatives: python train_mini_resnet.py  |  python pytorch/train_mini_resnet.py
cd ..
```

Dataset lives in `model_finetune/dataset/<train|validation>/<class>/` (six class
folders, any names; the class order is saved to `dataset/class_mapping.json`).
Output: `model_finetune/models/tf/<name>.keras`.

## 2. Quantize to deployable TFLite

```bash
python firmware/pc/tools/quantize_keras_model.py \
    --quant-format int8 --quant-granularity per-channel
```

Formats: `int8` (recommended) · `int16` (per-channel only) · `float32`.
Output: `firmware/pc/artifacts/models/*.tflite` + `.../reports/*_quantization_report.json`.

## 3. Build & flash main board firmware

```bash
cd firmware/main_board
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # Linux; use the board's port
cd ../..
```

## 4. Flash only the model partition

```bash
python firmware/main_board/flash_tflite_model.py \
    firmware/pc/artifacts/models/<model>_int8_per-channel.tflite -p /dev/ttyACM0
```

The partition offset is read from `firmware/main_board/partitions.csv` — you do
not pass it by hand.

## 5. Benchmark on the device

```bash
python firmware/pc/benchmark/run_benchmark_png.py --help
```

Reports label accuracy, latency, throughput, and score similarity (MAE / Max
Error / Cosine) by feeding test frames and comparing the main board's `RESULT`
output against the PC reference.

## 6. Camera preview / gesture → output

- Native preview + control app: `apps/esp32_cam_input_app` (Dear ImGui / SDL3).
- PC webcam model demo (ONNX): `python model_finetune/pytorch/webcam_demo.py`.
- Control board (robot arm) firmware: `firmware/control_board` (`idf.py build flash`).
  The main board prints `RESULT,<index>,…`; a PC bridge maps the index to an
  action (`up/down/left/right/clamp/release`) and sends it to the control board.

## Output demo (GPIO / LED / PWM)

Standalone teaching firmware:

```bash
cd firmware/teaching_output_demo
idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Then send serial commands: `LED,1` / `LED,0` / `PWM,<0-255>` / `BLINK` / `STOP` /
`STATUS`.
