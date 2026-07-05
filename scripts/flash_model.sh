#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "Usage: scripts/flash_model.sh MODEL.tflite [PORT]"
  exit 1
fi

MODEL="$1"
PORT="${2:-/dev/ttyACM0}"

cd "$(dirname "$0")/../esp"
python flash_tflite_model.py "$MODEL" -p "$PORT"
