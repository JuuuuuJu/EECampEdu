import argparse
import glob
import io
import math
import os
import pickle
import re
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
    INPUT_HEIGHT,
    INPUT_WIDTH,
    NORMALIZE_MEAN,
    NORMALIZE_STD,
)

INT16_MAX = 32767


def preprocess_image(image):
    image = image.convert("L").resize((INPUT_WIDTH, INPUT_HEIGHT))
    image_np = np.array(image, dtype=np.float32)
    return (image_np / 255.0 - NORMALIZE_MEAN) / NORMALIZE_STD


def iter_png_values(path):
    files = sorted(glob.glob(os.path.join(path, "**", "*.png"), recursive=True))
    if not files:
        raise FileNotFoundError(f"No PNG files found under {path}")

    for file_path in files:
        yield preprocess_image(Image.open(file_path)).reshape(-1)


def iter_pkl_values(path, max_samples=None, stride=1):
    with open(path, "rb") as f:
        images = pickle.load(f)

    selected = images[::stride]
    if max_samples is not None:
        selected = selected[:max_samples]

    for byte_data in selected:
        yield preprocess_image(Image.open(io.BytesIO(byte_data))).reshape(-1)


def choose_exponent(abs_bound):
    if abs_bound <= 0:
        return -15
    return int(math.ceil(math.log2(abs_bound / INT16_MAX)))


def write_python_config(output_path, exponent, abs_bound, source, percentile,
                        float_min, float_max, mode, affine_target_int16):
    center = (float_min + float_max) / 2.0
    half_range = max((float_max - float_min) / 2.0, 1e-12)
    represented_range = affine_target_int16 * (2 ** exponent)
    affine_alpha = represented_range / half_range
    text = f'''"""Shared input quantization settings for calibration and ESP benchmark."""

INPUT_WIDTH = {INPUT_WIDTH}
INPUT_HEIGHT = {INPUT_HEIGHT}
NORMALIZE_MEAN = {NORMALIZE_MEAN}
NORMALIZE_STD = {NORMALIZE_STD}

# "affine" maps normalized float input into calibrated min/max bounds, but
# intentionally leaves int16 headroom. Mapping the input all the way to
# +/-32767 can cause early ESP-DL activation saturation in l1/l2.
#
# Set to "exponent" to use direct ESP-DL power-of-two input quantization:
# real_value = int_value * 2^INPUT_EXPONENT.
INPUT_QUANT_MODE = {mode!r}
INPUT_EXPONENT = {exponent}
INPUT_CALIBRATION_SOURCE = {source!r}
INPUT_CALIBRATION_PERCENTILE = {percentile}
INPUT_FLOAT_MIN = {float_min!r}
INPUT_FLOAT_MAX = {float_max!r}
INPUT_CALIBRATED_ABS_MAX = {abs_bound!r}
INPUT_AFFINE_CENTER = {center!r}
INPUT_AFFINE_HALF_RANGE = {half_range!r}
INPUT_AFFINE_TARGET_INT16 = {affine_target_int16}
INPUT_AFFINE_ALPHA = {affine_alpha!r}
# Affine calibration maps calibrated min/max to approximately:
# [{-affine_target_int16}, {affine_target_int16}]
'''
    Path(output_path).write_text(text, encoding="utf-8")


def update_cpp_config(path, exponent):
    if not path:
        return

    config_path = Path(path)
    if not config_path.exists():
        print(f"skip C++ config update: {config_path} not found")
        return

    text = config_path.read_text(encoding="utf-8")
    new_text, count = re.subn(
        r"constexpr int INPUT_EXPONENT = -?\d+;",
        f"constexpr int INPUT_EXPONENT = {exponent};",
        text,
        count=1,
    )
    if count == 0:
        print(f"skip C++ config update: INPUT_EXPONENT not found in {config_path}")
        return

    config_path.write_text(new_text, encoding="utf-8")
    print(f"updated C++ config : {config_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--png-dir", default=str(CNN_DIR / "dataset" / "test" / "00"))
    parser.add_argument("--pkl", default=None)
    parser.add_argument("--percentile", type=float, default=99.95)
    parser.add_argument("--max-samples", type=int, default=None)
    parser.add_argument("--stride", type=int, default=1)
    parser.add_argument("--output", default=str(DEPLOY_DIR / "input_quant_config.py"))
    parser.add_argument("--cpp-config", default=str(CNN_DIR.parent / "esp" / "main" / "model_config.hpp"))
    parser.add_argument("--mode", choices=["affine", "exponent"], default="affine")
    parser.add_argument("--exponent", type=int, default=None)
    parser.add_argument("--affine-target-int16", type=int, default=12000)
    args = parser.parse_args()

    if args.pkl:
        pkl_path = Path(args.pkl)
        chunks = list(iter_pkl_values(pkl_path, args.max_samples, args.stride))
        source = args.pkl
    else:
        chunks = list(iter_png_values(args.png_dir))
        source = args.png_dir

    values = np.concatenate(chunks)
    abs_values = np.abs(values)
    abs_bound = float(np.percentile(abs_values, args.percentile))
    abs_max = float(abs_values.max())
    float_min = float(values.min())
    float_max = float(values.max())
    exponent = args.exponent if args.exponent is not None else choose_exponent(abs_bound)
    if not (1 <= args.affine_target_int16 <= INT16_MAX):
        raise ValueError("--affine-target-int16 must be between 1 and 32767")

    represented_range = INT16_MAX * (2 ** exponent)
    affine_represented_range = args.affine_target_int16 * (2 ** exponent)
    if args.mode == "affine":
        clipped_frac = float(np.mean((values < float_min) | (values > float_max)))
    else:
        clipped_frac = float(np.mean(abs_values > represented_range))

    write_python_config(
        args.output,
        exponent,
        abs_bound,
        source,
        args.percentile,
        float_min,
        float_max,
        args.mode,
        args.affine_target_int16,
    )
    update_cpp_config(args.cpp_config, exponent)

    print(f"samples/chunks      : {len(chunks)}")
    print(f"mode                : {args.mode}")
    print(f"float min/max       : {float_min:.6f} / {float_max:.6f}")
    print(f"abs max             : {abs_max:.6f}")
    print(f"percentile abs bound: p{args.percentile} = {abs_bound:.6f}")
    print(f"chosen exponent     : {exponent}")
    print(f"int16 real range    : [{-represented_range:.6f}, {represented_range:.6f}]")
    if args.mode == "affine":
        print(f"affine center       : {(float_min + float_max) / 2.0:.6f}")
        print(f"affine half range   : {(float_max - float_min) / 2.0:.6f}")
        print(f"affine target int16 : {args.affine_target_int16}")
        print(f"affine real range   : [{-affine_represented_range:.6f}, {affine_represented_range:.6f}]")
        print(f"affine alpha        : {affine_represented_range / max((float_max - float_min) / 2.0, 1e-12):.6f}")
    print(f"estimated clipping  : {clipped_frac * 100:.4f}%")
    print(f"wrote               : {args.output}")


if __name__ == "__main__":
    main()
