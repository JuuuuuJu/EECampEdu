"""
PC-side ESP-DL quantization sweep.

This does not generate deployable firmware files. It only answers:
"Is this model fundamentally failing under int16 per-tensor PTQ, and would
int8 per-channel/entropy likely recover accuracy?"

Run in the same Python environment that can import ESP-DL calibrator/evaluator:
    python quant_sweep.py
"""
import glob
import io
import os
import pickle
import sys
import tempfile
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent
CNN_DIR = SCRIPT_DIR.parent
DEPLOY_DIR = CNN_DIR / "deploy"
ARTIFACTS_DIR = CNN_DIR / "artifacts"
for import_dir in (CNN_DIR, DEPLOY_DIR):
    if str(import_dir) not in sys.path:
        sys.path.insert(0, str(import_dir))

from calibrator import Calibrator
from evaluator import Evaluator
from optimizer import optimize_fp_model
from input_quant_config import (
    INPUT_AFFINE_ALPHA,
    INPUT_AFFINE_CENTER,
    INPUT_HEIGHT,
    INPUT_QUANT_MODE,
    INPUT_WIDTH,
    NORMALIZE_MEAN,
    NORMALIZE_STD,
)

DATA_DIR = os.environ.get("QUANT_SWEEP_DATA_DIR", str(CNN_DIR / "dataset" / "test" / "00"))
CALIB_MAX_SAMPLES = int(os.environ.get("CALIB_MAX_SAMPLES", "1000"))
CALIB_STRIDE = int(os.environ.get("CALIB_STRIDE", "1"))


def first_existing(*paths):
    for path in paths:
        if Path(path).exists():
            return Path(path)
    return Path(paths[0])


def make_affine_input_model(source_path, output_path):
    model = onnx.load(source_path)
    initializers = {init.name: init for init in model.graph.initializer}

    weight_name = "model1.0.weight"
    bias_name = "model1.0.bias"
    if weight_name not in initializers or bias_name not in initializers:
        for node in model.graph.node:
            if node.op_type == "Conv" and len(node.input) >= 3:
                weight_name = node.input[1]
                bias_name = node.input[2]
                break

    weight = numpy_helper.to_array(initializers[weight_name]).astype(np.float32)
    bias = numpy_helper.to_array(initializers[bias_name]).astype(np.float32)
    weight_new = weight / np.float32(INPUT_AFFINE_ALPHA)
    bias_new = bias + np.float32(INPUT_AFFINE_CENTER) * weight.sum(axis=(1, 2, 3))
    initializers[weight_name].CopyFrom(numpy_helper.from_array(weight_new, weight_name))
    initializers[bias_name].CopyFrom(numpy_helper.from_array(bias_new.astype(np.float32), bias_name))
    onnx.save(model, output_path)


def transform_input(arr):
    arr = (arr / 255.0 - NORMALIZE_MEAN) / NORMALIZE_STD
    if INPUT_QUANT_MODE == "affine":
        arr = (arr - INPUT_AFFINE_CENTER) * INPUT_AFFINE_ALPHA
    return arr


def preprocess_image(image):
    image = image.convert("L").resize((INPUT_WIDTH, INPUT_HEIGHT))
    arr = np.array(image, dtype=np.float32)
    arr = transform_input(arr)
    return np.expand_dims(arr, axis=(0, 1))


def preprocess_bytes(byte_data):
    return preprocess_image(Image.open(io.BytesIO(byte_data)))


def load_calibration_dataset():
    x_cal_path = first_existing(ARTIFACTS_DIR / "X_cal.pkl", CNN_DIR / "X_cal.pkl")
    with open(x_cal_path, "rb") as f:
        images = pickle.load(f)

    indices = list(range(0, len(images), CALIB_STRIDE))[:CALIB_MAX_SAMPLES]
    dataset = np.array([preprocess_bytes(images[i]) for i in indices], dtype=np.float32)
    if dataset.ndim == 5 and dataset.shape[1] == 1:
        dataset = dataset.squeeze(axis=1)
    return dataset


def load_test_pngs():
    files = sorted(glob.glob(os.path.join(DATA_DIR, "**", "*.png"), recursive=True))
    if not files:
        raise FileNotFoundError(f"No PNG files under {DATA_DIR}")

    xs = []
    labels = []
    for path in files:
        folder = os.path.basename(os.path.dirname(path))
        labels.append(int(folder.split("_", 1)[0]) - 1)
        xs.append(preprocess_image(Image.open(path)))
    dataset = np.array(xs, dtype=np.float32)
    if dataset.ndim == 5 and dataset.shape[1] == 1:
        dataset = dataset.squeeze(axis=1)
    return files, dataset, np.asarray(labels, dtype=np.int64)


def evaluate_config(model_proto, calib_dataset, test_dataset, test_labels, quant_bit, granularity, method):
    with tempfile.NamedTemporaryFile(delete=False, suffix=".pickle") as tmp:
        pickle_path = tmp.name

    try:
        calib = Calibrator(quant_bit, granularity, method)
        calib.set_providers(["CPUExecutionProvider"])
        calib.generate_quantization_table(model_proto, calib_dataset, pickle_path)

        eva = Evaluator(quant_bit, granularity, "esp32s3")
        eva.set_providers(["CPUExecutionProvider"])
        eva.generate_quantized_model(model_proto, pickle_path)
        if hasattr(eva, "evaluate_quantized_model"):
            eval_result = eva.evaluate_quantized_model(test_dataset, to_float=True)
        elif hasattr(eva, "evalute_quantized_model"):
            eval_result = eva.evalute_quantized_model(test_dataset, True)
        else:
            methods = [name for name in dir(eva) if "quant" in name.lower() or "eval" in name.lower()]
            raise AttributeError(f"No quantized evaluation method found. Available candidates: {methods}")

        outputs = eval_result[0] if isinstance(eval_result, (tuple, list)) else eval_result
        logits = np.asarray(outputs[0])
        if logits.ndim > 2:
            logits = logits.reshape((logits.shape[0], -1))
        pred = np.argmax(logits, axis=1)
        correct = int(np.sum(pred == test_labels))
        unique, counts = np.unique(pred, return_counts=True)
        dist = ", ".join(f"{int(k)}:{int(v)}" for k, v in zip(unique, counts))
        print(f"{quant_bit:<5} {granularity:<12} {method:<8} {correct:>2}/{len(test_labels):<2} "
              f"{correct / len(test_labels) * 100:>6.2f}%  pred=[{dist}]")
    except Exception as e:
        print(f"{quant_bit:<5} {granularity:<12} {method:<8} FAILED: {e}")
    finally:
        if os.path.exists(pickle_path):
            os.unlink(pickle_path)


def main():
    ARTIFACTS_DIR.mkdir(exist_ok=True)
    model_path = ARTIFACTS_DIR / "model.onnx"
    if INPUT_QUANT_MODE == "affine":
        model_path = ARTIFACTS_DIR / "model_affine_input.onnx"
        make_affine_input_model(str(ARTIFACTS_DIR / "model.onnx"), str(model_path))

    optimized_model_path = optimize_fp_model(str(model_path))
    model_proto = onnx.load(optimized_model_path)
    calib_dataset = load_calibration_dataset()
    _, test_dataset, test_labels = load_test_pngs()

    print(f"calibration samples: {len(calib_dataset)}")
    print(f"test samples       : {len(test_labels)}")
    print(f"{'bits':<5} {'granularity':<12} {'method':<8} accuracy  distribution")
    print("-" * 86)
    configs = [
        ("int16", "per-tensor", "minmax"),
        ("int8", "per-tensor", "minmax"),
        ("int8", "per-tensor", "entropy"),
        ("int8", "per-channel", "minmax"),
        ("int8", "per-channel", "entropy"),
    ]
    for config in configs:
        evaluate_config(model_proto, calib_dataset, test_dataset, test_labels, *config)


if __name__ == "__main__":
    main()
