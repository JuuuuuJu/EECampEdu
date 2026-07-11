"""
Print PC-side ONNX intermediate tensor stats in the same style as ESP
LAYER_DUMP. Use this to find the first layer where ESP-DL diverges.

Usage:
    python compare_onnx_layers.py ./dataset/test/00/01_palm/frame_00_01_0010.png
"""
import os
import sys
import tempfile
import pickle
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper
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

MODEL_PATH = ARTIFACTS_DIR / ("model_affine_input_optimized.onnx" if INPUT_QUANT_MODE == "affine" else "model_optimized.onnx")
CALIB_PATH = ARTIFACTS_DIR / "handrecognition_calib.pickle"

LAYER_OUTPUTS = [
    ("l1", "onnx::MaxPool_12"),
    ("l2", "input.4"),
    ("l3", "onnx::MaxPool_15"),
    ("l4", "input.12"),
    ("l5", "onnx::MaxPool_18"),
    ("l6", "onnx::Flatten_19"),
    ("flatten", "onnx::Gemm_20"),
    ("l7", "onnx::Gemm_22"),
    ("l8", "input.24"),
    ("softmax", "output"),
]


def preprocess(image_path):
    image = Image.open(image_path).convert("L").resize((INPUT_WIDTH, INPUT_HEIGHT))
    arr = np.array(image, dtype=np.float32)
    arr = (arr / 255.0 - NORMALIZE_MEAN) / NORMALIZE_STD
    if INPUT_QUANT_MODE == "affine":
        arr = (arr - INPUT_AFFINE_CENTER) * INPUT_AFFINE_ALPHA
    return np.expand_dims(arr, axis=(0, 1))


def make_debug_model():
    model = onnx.load(str(MODEL_PATH))
    existing_outputs = {output.name for output in model.graph.output}
    for _, output_name in LAYER_OUTPUTS:
        if output_name not in existing_outputs:
            model.graph.output.append(helper.make_tensor_value_info(output_name, TensorProto.FLOAT, None))
            existing_outputs.add(output_name)

    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".onnx")
    tmp.close()
    onnx.save(model, tmp.name)
    return tmp.name


def quantized_stats(name, tensor, scale):
    flat_float = np.asarray(tensor, dtype=np.float64).reshape(-1)
    if scale is None:
        print(f"ONNX_DUMP,{name},float,min={flat_float.min():.8f},max={flat_float.max():.8f},sum={flat_float.sum():.8f}")
        return

    q = np.round(flat_float / scale)
    clipped = int(np.sum((q < -32768) | (q > 32767)))
    q = np.clip(q, -32768, 32767).astype(np.int16)
    print(
        f"ONNX_DUMP,{name},scale={scale},min={int(q.min())},max={int(q.max())},"
        f"clip={clipped},sum={int(q.astype(np.int64).sum())},first={int(q.reshape(-1)[0])}"
    )


def main():
    image_path = Path(sys.argv[1]) if len(sys.argv) > 1 else CNN_DIR / "dataset" / "test" / "00" / "01_palm" / "frame_00_01_0010.png"
    if not image_path.exists():
        raise SystemExit(f"image not found: {image_path}")
    if not MODEL_PATH.exists():
        raise SystemExit(f"model not found: {MODEL_PATH}")

    with open(CALIB_PATH, "rb") as f:
        scales = pickle.load(f)

    debug_model = make_debug_model()
    try:
        sess = ort.InferenceSession(debug_model, providers=["CPUExecutionProvider"])
        input_name = sess.get_inputs()[0].name
        outputs = sess.run(None, {input_name: preprocess(str(image_path))})
    finally:
        os.unlink(debug_model)

    print(f"image={image_path}")
    for output_info, value in zip(sess.get_outputs(), outputs):
        name = output_info.name
        layer_name = next((layer for layer, output_name in LAYER_OUTPUTS if output_name == name), name)
        quantized_stats(layer_name, value, scales.get(name))


if __name__ == "__main__":
    main()
