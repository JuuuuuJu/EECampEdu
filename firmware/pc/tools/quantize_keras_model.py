import argparse
import json
import os
from pathlib import Path

os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
from PIL import Image


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
PROJECT_ROOT = FIRMWARE_ROOT.parent
DEFAULT_MODEL_SOURCE_DIR = PROJECT_ROOT / "model_finetune" / "models"
DEFAULT_CALIBRATION_DIR = PROJECT_ROOT / "model_finetune" / "dataset" / "train"
DEFAULT_OUTPUT_DIR = FIRMWARE_ROOT / "pc" / "artifacts" / "models"
DEFAULT_REPORT_DIR = FIRMWARE_ROOT / "pc" / "artifacts" / "reports"
CLASS_NAMES = ["up", "down", "right", "left", "null"]
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp"}


def display_path(path):
    try:
        return str(Path(path).resolve().relative_to(PROJECT_ROOT))
    except ValueError:
        return str(path)


def parse_image_size(text):
    value = text.lower().replace(" ", "")
    if "x" in value:
        width_text, height_text = value.split("x", 1)
        return int(width_text), int(height_text)
    side = int(value)
    return side, side


def iter_images(dataset_dir):
    dataset_dir = Path(dataset_dir)
    for path in sorted(dataset_dir.rglob("*")):
        if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS:
            yield path


def preprocess_image(path, image_size, mode):
    image = Image.open(path).convert("L")
    width, height = image_size

    if mode == "resize":
        image = image.resize((width, height))
    elif mode == "center_crop":
        side = min(image.size)
        left = (image.width - side) // 2
        top = (image.height - side) // 2
        image = image.crop((left, top, left + side, top + side)).resize((width, height))
    elif mode == "letterbox":
        image.thumbnail((width, height))
        canvas = Image.new("L", (width, height), 0)
        left = (width - image.width) // 2
        top = (height - image.height) // 2
        canvas.paste(image, (left, top))
        image = canvas
    else:
        raise ValueError(f"Unsupported preprocess mode: {mode}")

    array = np.asarray(image, dtype=np.float32) / 255.0
    return array.reshape((height, width, 1))


def representative_dataset(image_paths, image_size, preprocess_mode):
    def generator():
        for path in image_paths:
            yield [preprocess_image(path, image_size, preprocess_mode)[None, ...]]

    return generator


def resolve_model_path(args):
    if args.keras:
        model_path = Path(args.keras)
    else:
        model_source_dir = Path(args.model_source_dir) if args.model_source_dir else DEFAULT_MODEL_SOURCE_DIR
        model_path = model_source_dir / f"{args.model_name}.keras"

    model_path = model_path.expanduser().resolve()
    if model_path.suffix.lower() != ".keras":
        raise ValueError(f"Deploy quantization now only accepts .keras source models: {model_path}")
    if not model_path.exists():
        raise FileNotFoundError(model_path)
    return model_path


def inspect_tflite(tf, model_path):
    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    return {
        "input_shape": input_detail["shape"].tolist(),
        "input_dtype": str(input_detail["dtype"]),
        "input_quantization": {
            "scale": float(input_detail["quantization"][0]),
            "zero_point": int(input_detail["quantization"][1]),
        },
        "output_shape": output_detail["shape"].tolist(),
        "output_dtype": str(output_detail["dtype"]),
        "output_quantization": {
            "scale": float(output_detail["quantization"][0]),
            "zero_point": int(output_detail["quantization"][1]),
        },
    }


def main():
    parser = argparse.ArgumentParser(
        description="Quantize a .keras gesture model into an ESP deploy int8 TFLite model."
    )
    parser.add_argument("--model-name", default="Separable_CNN", help="Model basename under --model-source-dir.")
    parser.add_argument("--keras", help="Explicit .keras source model path. Overrides --model-name.")
    parser.add_argument(
        "--model-source-dir",
        help=f"Directory containing .keras source models. Default: {display_path(DEFAULT_MODEL_SOURCE_DIR)}",
    )
    parser.add_argument(
        "--calibration-dir",
        help=f"Calibration image directory. Default: {display_path(DEFAULT_CALIBRATION_DIR)}",
    )
    parser.add_argument(
        "--output-dir",
        help=f"Output directory for deploy .tflite. Default: {display_path(DEFAULT_OUTPUT_DIR)}",
    )
    parser.add_argument(
        "--report-dir",
        help=f"Output directory for quantization report JSON. Default: {display_path(DEFAULT_REPORT_DIR)}",
    )
    parser.add_argument("--image-size", default="96x96", help="Model input image size. Default: 96x96.")
    parser.add_argument(
        "--preprocess-mode",
        choices=["resize", "center_crop", "letterbox"],
        default="resize",
        help="Calibration preprocessing mode. Default: resize.",
    )
    parser.add_argument("--samples", type=int, default=200, help="Maximum calibration samples. Default: 200.")
    parser.add_argument("--output", help="Explicit output .tflite path.")
    parser.add_argument("--report", help="Explicit report JSON path.")
    args = parser.parse_args()

    model_path = resolve_model_path(args)
    calibration_dir = Path(args.calibration_dir).expanduser().resolve() if args.calibration_dir else DEFAULT_CALIBRATION_DIR
    image_size = parse_image_size(args.image_size)
    image_paths = list(iter_images(calibration_dir))[: args.samples]
    if not image_paths:
        raise RuntimeError(f"No calibration images found under {calibration_dir}")

    import tensorflow as tf

    print(f"Source .keras model : {model_path}")
    print(f"Calibration images  : {calibration_dir} ({len(image_paths)} samples)")
    print(f"Preprocess          : {args.preprocess_mode}, {image_size[0]}x{image_size[1]}, grayscale / 255.0")

    model = tf.keras.models.load_model(model_path, compile=False)
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset(
        image_paths,
        image_size,
        args.preprocess_mode,
    )
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8

    output_dir = Path(args.output_dir) if args.output_dir else DEFAULT_OUTPUT_DIR
    output_path = Path(args.output) if args.output else output_dir / f"{args.model_name}_int8.tflite"
    output_path = output_path.expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(converter.convert())

    info = inspect_tflite(tf, output_path)
    if "int8" not in info["input_dtype"] or "int8" not in info["output_dtype"]:
        raise RuntimeError(
            "Converted model is not full int8. "
            f"input={info['input_dtype']} output={info['output_dtype']}"
        )

    report_dir = Path(args.report_dir) if args.report_dir else DEFAULT_REPORT_DIR
    report_path = Path(args.report) if args.report else report_dir / f"{args.model_name}_quantization_report.json"
    report_path = report_path.expanduser().resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "version": 1,
        "model_name": args.model_name,
        "source_model": display_path(model_path),
        "exported_tflite": display_path(output_path),
        "calibration_dir": display_path(calibration_dir),
        "calibration_samples": len(image_paths),
        "preprocess_mode": args.preprocess_mode,
        "image_size": [image_size[0], image_size[1]],
        "class_order": CLASS_NAMES,
        "input": {
            "shape": info["input_shape"],
            "dtype": "int8",
            **info["input_quantization"],
        },
        "output": {
            "shape": info["output_shape"],
            "dtype": "int8",
            **info["output_quantization"],
        },
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"Deploy TFLite       : {output_path}")
    print(f"Quantization report : {report_path}")
    print(f"Input               : {info['input_shape']} {info['input_dtype']} {info['input_quantization']}")
    print(f"Output              : {info['output_shape']} {info['output_dtype']} {info['output_quantization']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
