#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r pc/requirements.txt

echo "PC environment ready. Activate with: source .venv/bin/activate"
echo "Legacy ONNX debug tools require Python 3.7-3.10 and pc/requirements-legacy-onnx.txt."
