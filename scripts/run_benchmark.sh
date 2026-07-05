#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../pc"
python benchmark/run_benchmark_png.py \
  --model artifacts/models/Separable_CNN_int8.tflite \
  --dataset artifacts/datasets/test
