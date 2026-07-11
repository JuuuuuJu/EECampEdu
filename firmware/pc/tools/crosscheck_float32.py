"""
Cross-check tool: runs the ORIGINAL float32 ONNX model (no quantization) on
the same test images the ESP32 benchmark uses, applying the exact same
preprocessing as cnn/quantify.py's preprocess(). This tells us whether the
"almost everything predicts class 2" pattern seen on-device is:

  (a) a remaining firmware/quantization bug  -> float32 predictions here will
      be varied and roughly match each folder's expected gesture, while the
      ESP32 stays stuck on one class, OR

  (b) an actual model/dataset limitation      -> float32 predictions here will
      ALSO cluster heavily on one class, meaning the bug (if any) is upstream
      of the firmware entirely (label mapping, training, calibration set).

Run this from the same directory as run_benchmark_png.py (so ./dataset/test/00
resolves), with model.onnx available (the same one quantify.py loads).

Usage:
    python crosscheck_float32.py
"""
import os
import glob
import sys
from pathlib import Path
import numpy as np
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent
CNN_DIR = SCRIPT_DIR.parent
DEPLOY_DIR = CNN_DIR / "deploy"
ARTIFACTS_DIR = CNN_DIR / "artifacts"
for import_dir in (CNN_DIR, DEPLOY_DIR):
    if str(import_dir) not in sys.path:
        sys.path.insert(0, str(import_dir))

from input_quant_config import (
    INPUT_AFFINE_ALPHA,
    INPUT_AFFINE_CENTER,
    INPUT_HEIGHT,
    INPUT_QUANT_MODE,
    INPUT_WIDTH,
    NORMALIZE_MEAN,
    NORMALIZE_STD,
)

try:
    import onnxruntime as ort
except ImportError:
    raise SystemExit(
        "onnxruntime not installed. Install with:\n"
        "    pip install onnxruntime\n"
        "(use the same my_env37 venv as the rest of the cnn/ toolchain)"
    )

# ---- Must match cnn/quantify.py exactly ----
DATA_DIR = str(CNN_DIR / "dataset" / "test" / "00")
MODEL_PATH = ARTIFACTS_DIR / ('model_affine_input.onnx' if INPUT_QUANT_MODE == 'affine' else 'model.onnx')
INPUT_SIZE = (INPUT_WIDTH, INPUT_HEIGHT)


def preprocess(image_path):
    """Identical to quantify.py's preprocess(), minus the byte_data branch."""
    image = Image.open(image_path).convert('L')
    image = image.resize(INPUT_SIZE)
    image_np = np.array(image, dtype=np.float32)
    image_np = (image_np / 255.0 - NORMALIZE_MEAN) / NORMALIZE_STD
    if INPUT_QUANT_MODE == 'affine':
        image_np = (image_np - INPUT_AFFINE_CENTER) * INPUT_AFFINE_ALPHA
    image_np = np.expand_dims(image_np, axis=(0, 1))  # (1, 1, 96, 96) NCHW
    return image_np


def main():
    if not MODEL_PATH.exists():
        raise SystemExit(f"'{MODEL_PATH}' not found.")

    files = sorted(glob.glob(os.path.join(DATA_DIR, '**', '*.png'), recursive=True))
    if not files:
        raise SystemExit(f"No PNG files found under {DATA_DIR}")

    sess = ort.InferenceSession(str(MODEL_PATH), providers=['CPUExecutionProvider'])
    input_name = sess.get_inputs()[0].name

    print(f"{'file':<28} {'folder':<18} {'pred':>4}   logits")
    print("-" * 90)

    pred_counts = {}
    for f in files:
        x = preprocess(f)
        out = sess.run(None, {input_name: x})[0]
        out = np.asarray(out).flatten()
        pred = int(np.argmax(out))
        folder = os.path.basename(os.path.dirname(f))
        pred_counts[pred] = pred_counts.get(pred, 0) + 1

        logits_str = ", ".join(f"{v:8.2f}" for v in out)
        print(f"{os.path.basename(f):<28} {folder:<18} {pred:>4}   [{logits_str}]")

    print("-" * 90)
    print("Prediction distribution (float32, unquantized model):")
    for k in sorted(pred_counts):
        print(f"  class {k}: {pred_counts[k]} / {len(files)}")


if __name__ == '__main__':
    main()
