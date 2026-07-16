import argparse
import json
import os
import tempfile
import zipfile
from pathlib import Path

os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
from PIL import Image


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
PROJECT_ROOT = FIRMWARE_ROOT.parent
DEFAULT_MODEL_SOURCE_DIR = PROJECT_ROOT / "model_finetune" / "models" / "tf"
DEFAULT_CALIBRATION_DIR = PROJECT_ROOT / "model_finetune" / "dataset" / "train"
DEFAULT_OUTPUT_DIR = FIRMWARE_ROOT / "pc" / "artifacts" / "models"
DEFAULT_REPORT_DIR = FIRMWARE_ROOT / "pc" / "artifacts" / "reports"
CLASS_NAMES = ["up", "ok", "thumb", "palm", "rock", "stone"]
FOUR_CLASS_NAMES = ["up", "down", "right", "left"]
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


def build_mini_resnet_keras_model(tf, image_size, num_classes):
    width, height = image_size
    base_input = tf.keras.Input(shape=(height, width, 1), name="input_3")
    x = tf.keras.layers.GaussianNoise(0.05, name="gaussian_noise_1")(base_input)

    x1 = tf.keras.layers.Conv2D(16, (3, 3), padding="same", activation="relu", name="conv1_1")(x)
    shortcut1 = x1
    x1 = tf.keras.layers.Conv2D(16, (3, 3), padding="same", activation="relu", name="conv1_2")(x1)
    x1 = tf.keras.layers.Conv2D(16, (3, 3), padding="same", name="conv1_3")(x1)
    x1 = tf.keras.layers.add([x1, shortcut1], name="add_2")
    x1 = tf.keras.layers.Activation("relu", name="activation_2")(x1)
    x1 = tf.keras.layers.MaxPooling2D(2, 2, name="max_pooling2d_2")(x1)

    x2 = tf.keras.layers.Conv2D(32, (3, 3), padding="same", activation="relu", name="conv2_1")(x1)
    x2 = tf.keras.layers.Conv2D(32, (3, 3), padding="same", name="conv2_2")(x2)
    shortcut2 = tf.keras.layers.Conv2D(32, (1, 1), padding="same", name="shortcut_conv2")(x1)
    x2 = tf.keras.layers.add([x2, shortcut2], name="add_3")
    x2 = tf.keras.layers.Activation("relu", name="activation_3")(x2)
    x2 = tf.keras.layers.MaxPooling2D(2, 2, name="max_pooling2d_3")(x2)
    x2 = tf.keras.layers.Flatten(name="flatten_1")(x2)

    base_model = tf.keras.Model(base_input, x2, name="resnet_base")
    inputs = tf.keras.Input(shape=(height, width, 1), name="input_6")
    x = base_model(inputs)
    x = tf.keras.layers.Dropout(0.5, name="dropout_2")(x)
    outputs = tf.keras.layers.Dense(num_classes, activation="softmax", name="dense_2")(x)
    return tf.keras.Model(inputs, outputs)


def build_mobilenetv2_keras_model(tf, image_size, num_classes):
    width, height = image_size
    inputs = tf.keras.Input(shape=(height, width, 1), name="input_9")
    x = tf.keras.layers.GaussianNoise(0.05, name="gaussian_noise_2")(inputs)
    x = tf.keras.layers.Concatenate(axis=-1, name="concatenate_1")([x, x, x])
    x = tf.keras.layers.Rescaling(scale=2.0, offset=-1.0, name="rescaling_1")(x)
    base_model = tf.keras.applications.MobileNetV2(
        input_shape=(height, width, 3),
        alpha=0.35,
        include_top=False,
        weights=None,
    )
    x = base_model(x)
    x = tf.keras.layers.GlobalAveragePooling2D(name="global_average_pooling2d_1")(x)
    base = tf.keras.Model(inputs, x, name="mobilenet_base")

    ft_inputs = tf.keras.Input(shape=(height, width, 1), name="input_10")
    ft_x = base(ft_inputs, training=False)
    ft_x = tf.keras.layers.Dropout(0.5, name="dropout_2")(ft_x)
    outputs = tf.keras.layers.Dense(
        num_classes,
        activation="softmax",
        kernel_regularizer=tf.keras.regularizers.l2(0.01),
        name="dense_2",
    )(ft_x)
    return tf.keras.Model(ft_inputs, outputs)


def is_keras_archive(model_path):
    if not zipfile.is_zipfile(model_path):
        return False
    with zipfile.ZipFile(model_path) as archive:
        names = set(archive.namelist())
    return {"metadata.json", "config.json", "model.weights.h5"}.issubset(names)


def keras_archive_contains(model_path, needle):
    if not zipfile.is_zipfile(model_path):
        return False
    with zipfile.ZipFile(model_path) as archive:
        try:
            config = archive.read("config.json").decode("utf-8", errors="ignore")
        except KeyError:
            return False
    return needle in config


def h5_dataset(weights, path):
    candidates = [path, path.replace("/", "\\", 1)]
    for candidate in candidates:
        if candidate in weights:
            return weights[candidate]
    raise KeyError(path)


def sanitize_keras3_config_for_tf210(value):
    if isinstance(value, dict):
        value.pop("optional", None)
        config = value.get("config")
        if isinstance(config, dict):
            config.pop("optional", None)
            if "batch_shape" in config and "batch_input_shape" not in config:
                config["batch_input_shape"] = config.pop("batch_shape")
        for child in list(value.values()):
            sanitize_keras3_config_for_tf210(child)
    elif isinstance(value, list):
        for child in value:
            sanitize_keras3_config_for_tf210(child)


def load_keras3_weights_by_shape(model, weights_path):
    import h5py

    arrays = []
    with h5py.File(weights_path, "r") as weights:
        def visit(name, obj):
            if not hasattr(obj, "shape"):
                return
            if "/vars/" not in name:
                return
            if name.startswith("optimizer") or name.startswith("metrics"):
                return
            arrays.append([name, np.asarray(obj), False])

        weights.visititems(visit)

    if len(arrays) != len(model.weights):
        raise RuntimeError(f"Keras archive weight count mismatch: model={len(model.weights)} archive={len(arrays)}")

    for weight in model.weights:
        expected_shape = tuple(weight.shape.as_list())
        match_index = None
        for index, (_, array, used) in enumerate(arrays):
            if not used and tuple(array.shape) == expected_shape:
                match_index = index
                break
        if match_index is None:
            raise RuntimeError(f"No Keras archive weight matches {weight.name} shape={expected_shape}")
        arrays[match_index][2] = True
        weight.assign(arrays[match_index][1])


def load_mini_resnet_archive(tf, model_path, image_size):
    import h5py

    with tempfile.TemporaryDirectory() as temp_dir:
        with zipfile.ZipFile(model_path) as archive:
            archive.extract("model.weights.h5", temp_dir)
        weights_path = Path(temp_dir) / "model.weights.h5"

        with h5py.File(weights_path, "r") as weights:
            dense_kernel = h5_dataset(weights, "layers/dense/vars/0")
            num_classes = int(dense_kernel.shape[-1])
            model = build_mini_resnet_keras_model(tf, image_size, num_classes)
            base = model.get_layer("resnet_base")
            conv_mapping = [
                ("conv1_1", "layers/functional/layers/conv2d/vars"),
                ("conv1_2", "layers/functional/layers/conv2d_1/vars"),
                ("conv1_3", "layers/functional/layers/conv2d_2/vars"),
                ("conv2_1", "layers/functional/layers/conv2d_3/vars"),
                ("conv2_2", "layers/functional/layers/conv2d_4/vars"),
                ("shortcut_conv2", "layers/functional/layers/conv2d_5/vars"),
            ]
            for layer_name, group_path in conv_mapping:
                base.get_layer(layer_name).set_weights([
                    np.asarray(h5_dataset(weights, f"{group_path}/0")),
                    np.asarray(h5_dataset(weights, f"{group_path}/1")),
                ])
            model.get_layer("dense_2").set_weights([
                np.asarray(h5_dataset(weights, "layers/dense/vars/0")),
                np.asarray(h5_dataset(weights, "layers/dense/vars/1")),
            ])
    return model


def load_mobilenetv2_archive(tf, model_path, image_size):
    with tempfile.TemporaryDirectory() as temp_dir:
        with zipfile.ZipFile(model_path) as archive:
            config = json.loads(archive.read("config.json"))
            sanitize_keras3_config_for_tf210(config)
            archive.extract("model.weights.h5", temp_dir)
        weights_path = Path(temp_dir) / "model.weights.h5"

        model = tf.keras.models.model_from_json(json.dumps(config))
        load_keras3_weights_by_shape(model, weights_path)
    return model


def load_source_model(tf, model_path, image_size):
    try:
        return tf.keras.models.load_model(model_path, compile=False)
    except (OSError, ValueError) as exc:
        if not is_keras_archive(model_path):
            raise
        is_mobilenet = "MobileNetV2" in model_path.stem or keras_archive_contains(model_path, "MobileNetV2")
        model_family = "MobileNetV2" if is_mobilenet else "Mini ResNet"
        print(f"[INFO] Detected newer .keras archive. Rebuilding {model_family} for TensorFlow 2.10 compatibility.")
        try:
            if is_mobilenet:
                return load_mobilenetv2_archive(tf, model_path, image_size)
            return load_mini_resnet_archive(tf, model_path, image_size)
        except Exception as fallback_exc:
            raise RuntimeError(
                f"Failed to load newer .keras archive with the TensorFlow 2.10 {model_family} compatibility loader."
            ) from fallback_exc


def class_names_for_output(output_shape):
    classes = int(output_shape[-1]) if output_shape else len(CLASS_NAMES)
    if classes == len(FOUR_CLASS_NAMES):
        return FOUR_CLASS_NAMES
    if classes == len(CLASS_NAMES):
        return CLASS_NAMES
    return [f"class_{index}" for index in range(classes)]


def main():
    parser = argparse.ArgumentParser(
        description="Quantize a .keras gesture model into an ESP deploy int8 TFLite model."
    )
    parser.add_argument(
        "--model-name",
        default="MobileNetV2_finetuned",
        help="Model basename under --model-source-dir.",
    )
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

    model = load_source_model(tf, model_path, image_size)
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
    output_stem = model_path.stem
    output_path = Path(args.output) if args.output else output_dir / f"{output_stem}_int8.tflite"
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
    report_path = Path(args.report) if args.report else report_dir / f"{output_stem}_quantization_report.json"
    report_path = report_path.expanduser().resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "version": 1,
        "model_name": output_stem,
        "source_model": display_path(model_path),
        "exported_tflite": display_path(output_path),
        "calibration_dir": display_path(calibration_dir),
        "calibration_samples": len(image_paths),
        "preprocess_mode": args.preprocess_mode,
        "image_size": [image_size[0], image_size[1]],
        "class_order": class_names_for_output(info["output_shape"]),
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
