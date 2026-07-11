import os
import glob
from pathlib import Path
import numpy as np
import onnxruntime as ort
from PIL import Image

# Configuration
SCRIPT_DIR = Path(__file__).resolve().parent
CNN_DIR = SCRIPT_DIR.parent
ARTIFACTS_DIR = CNN_DIR / "artifacts"
ONNX_MODEL_PATH = ARTIFACTS_DIR / 'model.onnx'
DATA_DIR = CNN_DIR / 'dataset' / 'test' / '00'
INPUT_WIDTH = 96
INPUT_HEIGHT = 96
NORMALIZE_MEAN = 0.0979
NORMALIZE_STD = 0.1991

def preprocess_for_onnx(image_path):
    """
    Preprocess image exactly as defined in quantify.py and run_benchmark_png.py
    to establish a ground-truth baseline for FP32 inference.
    """
    img = Image.open(image_path).convert('L')
    img = img.resize((INPUT_WIDTH, INPUT_HEIGHT))
    img_array = np.array(img, dtype=np.float32)

    # Standardization matching training phase
    normalized = (img_array / 255.0 - NORMALIZE_MEAN) / NORMALIZE_STD
    
    # ONNX / PyTorch typically expects NCHW format (Batch, Channel, Height, Width)
    input_tensor = np.expand_dims(normalized, axis=(0, 1))
    return input_tensor

def main():
    if not ONNX_MODEL_PATH.exists():
        print(f"[ERROR] ONNX model not found at {ONNX_MODEL_PATH}")
        return

    print("[INFO] Loading ONNX Runtime session...")
    session = ort.InferenceSession(str(ONNX_MODEL_PATH), providers=['CPUExecutionProvider'])
    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name

    search_pattern = os.path.join(str(DATA_DIR), '**', '*.png')
    image_files = sorted(glob.glob(search_pattern, recursive=True))

    if not image_files:
        print(f"[ERROR] No PNG files found in {DATA_DIR}")
        return

    print(f"[INFO] Evaluating {len(image_files[:20])} images for baseline comparison...\n")
    print(f"{'Filename':<25} | {'ONNX FP32 Pred'} | {'Logits (Top 3)'}")
    print("-" * 75)

    for img_path in image_files[:20]:
        filename = os.path.basename(img_path)
        input_tensor = preprocess_for_onnx(img_path)

        # Execute FP32 inference
        outputs = session.run([output_name], {input_name: input_tensor})
        logits = outputs[0].flatten()
        
        pred_idx = int(np.argmax(logits))
        
        # Get top 3 logits for debugging confidence levels
        top_3_indices = np.argsort(logits)[-3:][::-1]
        top_3_str = ", ".join([f"[{i}]: {logits[i]:.2f}" for i in top_3_indices])

        print(f"{filename:<25} | {pred_idx:<14} | {top_3_str}")

if __name__ == '__main__':
    main()
