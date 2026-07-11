"""
Dequantized-weights forward-pass simulator.

Reads the SAME model.bin / model_offsets.h the ESP32 firmware uses,
dequantizes each weight/bias array using its exponent (matching
model_define.hpp exactly), and runs a plain float32 forward pass
(Conv-ReLU-MaxPool x3, flatten, FC-ReLU, FC) on the SAME preprocessed
image quantify.py / run_benchmark_png.py use.

Purpose: isolate whether the ESP32 misclassification (heavy bias toward
predicting class 2) is caused by

  (a) wrong weight data/shape/ordering being read from model.bin -- if so,
      THIS float simulation will ALSO misclassify / collapse toward the
      same class, since it reads the exact same bytes, or

  (b) a remaining bug specific to ESP-DL's int16 fixed-point arithmetic
      on-device -- if so, this simulation (same weight VALUES, computed
      in plain float) should classify CORRECTLY, matching the float32
      ONNX reference (crosscheck_float32.py).

Run from the cnn/ directory (needs model.bin and model_offsets.h there;
copy them in if they live under esp/model/ instead).

Usage:
    python crosscheck_quantized.py
    python crosscheck_quantized.py path/to/one_image.png   # single file
"""
import os
import re
import sys
import glob
from pathlib import Path
import numpy as np
from PIL import Image
from numpy.lib.stride_tricks import sliding_window_view

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

OFFSETS_FILE = ARTIFACTS_DIR / 'model_offsets.h'
BIN_FILE = ARTIFACTS_DIR / 'model.bin'
DATA_DIR = str(CNN_DIR / "dataset" / "test" / "00")

# (offset_macro, num_elements, shape [ESP-DL Filter convention: kh,kw,Cin,Cout], exponent)
LAYERS = {
    'conv0_w':  ('CONV_0_FILTER_ELEMENT_OFFSET', 800,          (5, 5, 1, 32),   -16),
    'conv0_b':  ('CONV_0_BIAS_ELEMENT_OFFSET',   32,           (32,),           -12),
    'conv3_w':  ('CONV_3_FILTER_ELEMENT_OFFSET', 18432,        (3, 3, 32, 64),  -17),
    'conv3_b':  ('CONV_3_BIAS_ELEMENT_OFFSET',   64,           (64,),           -11),
    'conv6_w':  ('CONV_6_FILTER_ELEMENT_OFFSET', 36864,        (3, 3, 64, 64),  -17),
    'conv6_b':  ('CONV_6_BIAS_ELEMENT_OFFSET',   64,           (64,),           -8),
    'gemm10_w': ('GEMM_10_FILTER_ELEMENT_OFFSET', 6400 * 128,  (1, 1, 6400, 128), -17),
    'gemm10_b': ('GEMM_10_BIAS_ELEMENT_OFFSET',  128,          (128,),          -8),
    'gemm12_w': ('GEMM_12_FILTER_ELEMENT_OFFSET', 128 * 10,    (1, 1, 128, 10), -17),
    'gemm12_b': ('GEMM_12_BIAS_ELEMENT_OFFSET',  10,           (10,),           -8),
}

INPUT_SIZE = (INPUT_WIDTH, INPUT_HEIGHT)


def parse_offsets(path):
    offsets = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'#define\s+(\w+)\s+(\d+)', line.strip())
            if m:
                offsets[m.group(1)] = int(m.group(2))
    return offsets


def load_weights():
    if not OFFSETS_FILE.exists():
        raise SystemExit(f"'{OFFSETS_FILE}' not found.")
    if not BIN_FILE.exists():
        raise SystemExit(f"'{BIN_FILE}' not found.")

    offsets = parse_offsets(OFFSETS_FILE)
    raw = np.fromfile(str(BIN_FILE), dtype=np.int16)

    weights = {}
    for name, (macro, n, shape, exp) in LAYERS.items():
        if macro not in offsets:
            raise SystemExit(f"Offset macro {macro} not found in {OFFSETS_FILE}")
        off = offsets[macro]
        chunk = raw[off:off + n].astype(np.float64)
        # exponent is negative: real_value = raw_int16 * 2^exponent
        chunk = chunk * (2.0 ** exp)
        weights[name] = chunk.reshape(shape)
    return weights


def conv2d_valid(x, w, b):
    """x: (H,W,Cin) HWC float64. w: (kh,kw,Cin,Cout). b: (Cout,). -> (H',W',Cout)."""
    kh, kw, Cin, Cout = w.shape
    windows = sliding_window_view(x, (kh, kw, Cin))[:, :, 0, :, :, :]  # (Ho,Wo,kh,kw,Cin)
    out = np.tensordot(windows, w, axes=([2, 3, 4], [0, 1, 2])) + b     # (Ho,Wo,Cout)
    return out


def maxpool2d(x, k=2, s=2):
    H, W, C = x.shape
    Ho, Wo = H // s, W // s
    x = x[:Ho * s, :Wo * s, :]
    x = x.reshape(Ho, s, Wo, s, C)
    return x.max(axis=(1, 3))


def relu(x):
    return np.maximum(x, 0)


def flatten_hwc(x):
    """Matches the current ESP32 Flatten_HWC layer: raw HWC memory order."""
    return x.reshape(-1)


def preprocess(image_path):
    img = Image.open(image_path).convert('L').resize(INPUT_SIZE)
    arr = np.array(img, dtype=np.float64)
    arr = (arr / 255.0 - NORMALIZE_MEAN) / NORMALIZE_STD
    if INPUT_QUANT_MODE == 'affine':
        arr = (arr - INPUT_AFFINE_CENTER) * INPUT_AFFINE_ALPHA
    return arr[:, :, None]  # HWC, C=1


def forward(x_hwc, w):
    x = relu(conv2d_valid(x_hwc, w['conv0_w'], w['conv0_b']))
    x = maxpool2d(x)
    x = relu(conv2d_valid(x, w['conv3_w'], w['conv3_b']))
    x = maxpool2d(x)
    x = relu(conv2d_valid(x, w['conv6_w'], w['conv6_b']))
    x = maxpool2d(x)
    flat = flatten_hwc(x)                       # (6400,)
    flat_hwc = flat[None, None, :]               # (1,1,6400)
    g10 = relu(conv2d_valid(flat_hwc, w['gemm10_w'], w['gemm10_b']))  # (1,1,128)
    g12 = conv2d_valid(g10, w['gemm12_w'], w['gemm12_b'])             # (1,1,10)
    return g12.flatten()


def main():
    files = sys.argv[1:]
    if not files:
        files = sorted(glob.glob(os.path.join(DATA_DIR, '**', '*.png'), recursive=True))
    if not files:
        raise SystemExit(f"No PNG files found under {DATA_DIR} and none given on the command line.")

    w = load_weights()

    print(f"{'file':<28} {'folder':<18} {'pred':>4}   logits")
    print("-" * 100)
    pred_counts = {}
    for f in files:
        x = preprocess(f)
        out = forward(x, w)
        pred = int(np.argmax(out))
        folder = os.path.basename(os.path.dirname(f))
        pred_counts[pred] = pred_counts.get(pred, 0) + 1
        logits_str = ", ".join(f"{v:8.2f}" for v in out)
        print(f"{os.path.basename(f):<28} {folder:<18} {pred:>4}   [{logits_str}]")

    print("-" * 100)
    print("Prediction distribution (dequantized weights, float32 math):")
    for k in sorted(pred_counts):
        print(f"  class {k}: {pred_counts[k]} / {len(files)}")


if __name__ == '__main__':
    main()
