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
DEFAULT_VALIDATION_DIR = PROJECT_ROOT / "model_finetune" / "dataset" / "validation"
DEFAULT_OUTPUT_DIR = FIRMWARE_ROOT / "pc" / "artifacts" / "models"
DEFAULT_REPORT_DIR = FIRMWARE_ROOT / "pc" / "artifacts" / "reports"
CLASS_NAMES = ["up", "ok", "thumb", "palm", "rock", "stone"]
FOUR_CLASS_NAMES = ["up", "down", "right", "left"]
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp"}
TFLITE_EXPORT_FORMATS = ["float32", "float16", "int8", "int16"]
INT_QUANT_GRANULARITY_CHOICES = ["per-channel", "per-tensor"]


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


def select_calibration_images(calibration_dir, class_names, sample_limit):
    calibration_dir = Path(calibration_dir)
    per_class = []
    for class_name in class_names:
        class_dir = calibration_dir / class_name
        paths = list(iter_images(class_dir)) if class_dir.exists() else []
        per_class.append(paths)

    if any(per_class):
        selected = []
        max_len = max(len(paths) for paths in per_class)
        for index in range(max_len):
            for paths in per_class:
                if index < len(paths):
                    selected.append(paths[index])
                    if len(selected) >= sample_limit:
                        return selected
        return selected

    return list(iter_images(calibration_dir))[:sample_limit]


def evaluate_source_model(model, validation_dir, image_size, preprocess_mode, min_accuracy):
    validation_dir = Path(validation_dir)
    output_classes = int(model.output_shape[-1]) if model.output_shape else 0
    class_names = class_names_for_output(model.output_shape)
    if not validation_dir.exists():
        print(f"[WARN] Source validation skipped: {validation_dir} does not exist.")
        return None
    if output_classes not in (len(CLASS_NAMES), len(FOUR_CLASS_NAMES)):
        print(f"[WARN] Source validation skipped: unsupported output class count {output_classes}.")
        return None

    x_data = []
    y_data = []
    counts = {}
    for label, class_name in enumerate(class_names):
        class_dir = validation_dir / class_name
        paths = list(iter_images(class_dir)) if class_dir.exists() else []
        counts[class_name] = len(paths)
        for path in paths:
            x_data.append(preprocess_image(path, image_size, preprocess_mode))
            y_data.append(label)

    if not x_data:
        print(f"[WARN] Source validation skipped: no class folders matched {class_names}.")
        return None

    x = np.stack(x_data, axis=0).astype(np.float32)
    y = np.asarray(y_data, dtype=np.int32)
    preds = model.predict(x, batch_size=32, verbose=0)
    pred_labels = np.argmax(preds, axis=1)
    accuracy = float(np.mean(pred_labels == y))
    output_variation = float(np.mean(np.std(preds, axis=0)))
    pred_counts = {class_names[index]: int(np.sum(pred_labels == index)) for index in range(len(class_names))}

    print(f"Source validation images : {validation_dir} ({len(y)} samples)")
    print(f"Source validation classes: {counts}")
    print(f"Source .keras accuracy   : {int(np.sum(pred_labels == y))}/{len(y)} ({accuracy * 100:.2f}%)")
    print(f"Source prediction dist   : {pred_counts}")
    print(f"Source output variation  : {output_variation:.8f}")

    if accuracy < min_accuracy or output_variation < 1e-4:
        raise RuntimeError(
            "Source .keras model failed validation before quantization. "
            f"accuracy={accuracy * 100:.2f}% min={min_accuracy * 100:.2f}%, "
            f"output_variation={output_variation:.8f}. "
            "Retrain or fix the source model before exporting TFLite."
        )
    return accuracy


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


def configure_deploy_converter(tf, converter, quant_format, quant_granularity, image_paths, image_size, preprocess_mode):
    """Configure TensorFlow Lite converter for supported export/deploy formats."""
    if quant_format == "float32":
        return {
            "requires_calibration": False,
            "storage_dtype": "float32",
            "deploy_note": "Float32 TFLite export. Firmware supports float32 I/O, but it is slower and larger than int8.",
        }

    if quant_format == "float16":
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.target_spec.supported_types = [tf.float16]
        return {
            "requires_calibration": False,
            "storage_dtype": "float16 weights, float32 input/output",
            "deploy_note": "Float16 weight quantization. TFLite input/output usually remain float32; firmware needs Dequantize op support.",
        }

    if quant_granularity == "per-tensor":
        if hasattr(converter, "_experimental_disable_per_channel"):
            converter._experimental_disable_per_channel = True
        else:
            print("[WARN] This TensorFlow version cannot force per-tensor quantization; converter may still emit per-channel weights.")

    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset(
        image_paths,
        image_size,
        preprocess_mode,
    )

    if quant_format == "int8":
        converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
        converter.inference_input_type = tf.int8
        converter.inference_output_type = tf.int8
        return {
            "requires_calibration": True,
            "storage_dtype": "full int8 input/output/weights",
            "deploy_note": "Recommended ESP deploy format.",
        }

    if quant_format == "int16":
        ops_name = "EXPERIMENTAL_TFLITE_BUILTINS_ACTIVATIONS_INT16_WEIGHTS_INT8"
        if not hasattr(tf.lite.OpsSet, ops_name):
            raise RuntimeError(
                "This TensorFlow version does not expose int16 activation / int8 weight quantization. "
                f"Missing tf.lite.OpsSet.{ops_name}."
            )
        converter.target_spec.supported_ops = [getattr(tf.lite.OpsSet, ops_name)]
        converter.inference_input_type = tf.int16
        converter.inference_output_type = tf.int16
        return {
            "requires_calibration": True,
            "storage_dtype": "int16 activations, int8 weights",
            "deploy_note": "Experimental TensorFlow Lite int16 activation export. Verify TFLite Micro kernel support on ESP per model.",
        }

    raise ValueError(f"Unsupported TFLite export format: {quant_format}")


def validate_deploy_tflite(info, quant_format):
    input_dtype = info["input_dtype"]
    output_dtype = info["output_dtype"]
    if quant_format == "int8" and ("int8" not in input_dtype or "int8" not in output_dtype):
        raise RuntimeError(f"Converted model is not full int8. input={input_dtype} output={output_dtype}")
    if quant_format == "int16" and ("int16" not in input_dtype or "int16" not in output_dtype):
        raise RuntimeError(f"Converted model is not int16 I/O. input={input_dtype} output={output_dtype}")
    if quant_format == "float32" and ("float32" not in input_dtype or "float32" not in output_dtype):
        raise RuntimeError(f"Converted model is not float32 I/O. input={input_dtype} output={output_dtype}")


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
        description="Export a .keras gesture model into a deployable TFLite model."
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
    parser.add_argument(
        "--validation-dir",
        help=f"Source .keras validation directory. Default: {display_path(DEFAULT_VALIDATION_DIR)}",
    )
    parser.add_argument(
        "--min-source-accuracy",
        type=float,
        default=0.50,
        help="Minimum accepted source .keras validation accuracy before quantization. Default: 0.50.",
    )
    parser.add_argument(
        "--skip-source-validation",
        action="store_true",
        help="Skip source .keras validation before quantization. Not recommended.",
    )
    parser.add_argument(
        "--quant-format",
        choices=TFLITE_EXPORT_FORMATS,
        default="int8",
        help="TensorFlow Lite export format. Choices: float32, float16, int8, int16. Default: int8.",
    )
    parser.add_argument(
        "--quant-granularity",
        choices=INT_QUANT_GRANULARITY_CHOICES,
        default="per-channel",
        help="Integer quantization granularity. per-tensor is supported for int8; int16 currently requires per-channel. Ignored for float32/float16. Default: per-channel.",
    )
    parser.add_argument("--output", help="Explicit output .tflite path.")
    parser.add_argument("--report", help="Explicit report JSON path.")
    args = parser.parse_args()

    if args.quant_format == "int16" and args.quant_granularity != "per-channel":
        parser.error(
            "--quant-format int16 currently supports only --quant-granularity per-channel. "
            "TensorFlow Lite int16 activation quantization fails with forced per-tensor weight scales. "
            "Use int16/per-channel or int8/per-tensor."
        )

    model_path = resolve_model_path(args)
    calibration_dir = Path(args.calibration_dir).expanduser().resolve() if args.calibration_dir else DEFAULT_CALIBRATION_DIR
    image_size = parse_image_size(args.image_size)

    import tensorflow as tf

    print(f"Source .keras model : {model_path}")
    print(f"Preprocess          : {args.preprocess_mode}, {image_size[0]}x{image_size[1]}, grayscale / 255.0")

    model = load_source_model(tf, model_path, image_size)
    class_order = class_names_for_output(model.output_shape)
    needs_calibration = args.quant_format in ("int8", "int16")
    image_paths = []
    calibration_counts = {name: 0 for name in class_order}
    if needs_calibration:
        image_paths = select_calibration_images(calibration_dir, class_order, args.samples)
        if not image_paths:
            raise RuntimeError(f"No calibration images found under {calibration_dir} for classes {class_order}")
        calibration_counts = {name: sum(1 for path in image_paths if path.parent.name == name) for name in class_order}
        print(f"Calibration images  : {calibration_dir} ({len(image_paths)} samples)")
        print(f"Calibration classes : {calibration_counts}")
    else:
        print(f"Calibration images  : not required for {args.quant_format}")

    if not args.skip_source_validation:
        validation_dir = Path(args.validation_dir).expanduser().resolve() if args.validation_dir else DEFAULT_VALIDATION_DIR
        evaluate_source_model(model, validation_dir, image_size, args.preprocess_mode, args.min_source_accuracy)
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    conversion_config = configure_deploy_converter(
        tf,
        converter,
        args.quant_format,
        args.quant_granularity,
        image_paths,
        image_size,
        args.preprocess_mode,
    )

    output_dir = Path(args.output_dir) if args.output_dir else DEFAULT_OUTPUT_DIR
    output_stem = model_path.stem
    deploy_suffix = (
        f"{args.quant_format}_{args.quant_granularity}"
        if args.quant_format in ("int8", "int16")
        else args.quant_format
    )
    output_path = Path(args.output) if args.output else output_dir / f"{output_stem}_{deploy_suffix}.tflite"
    output_path = output_path.expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(converter.convert())

    info = inspect_tflite(tf, output_path)
    validate_deploy_tflite(info, args.quant_format)

    report_dir = Path(args.report_dir) if args.report_dir else DEFAULT_REPORT_DIR
    report_path = Path(args.report) if args.report else report_dir / f"{output_stem}_{deploy_suffix}_quantization_report.json"
    report_path = report_path.expanduser().resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "version": 1,
        "model_name": output_stem,
        "source_model": display_path(model_path),
        "exported_tflite": display_path(output_path),
        "calibration_dir": display_path(calibration_dir),
        "requested_quant_format": args.quant_format,
        "export_variant": deploy_suffix,
        "storage_dtype": conversion_config["storage_dtype"],
        "quantization_granularity": args.quant_granularity if args.quant_format in ("int8", "int16") else "not_applicable",
        "deploy_note": conversion_config["deploy_note"],
        "calibration_samples": len(image_paths),
        "preprocess_mode": args.preprocess_mode,
        "image_size": [image_size[0], image_size[1]],
        "class_order": class_order,
        "input": {
            "shape": info["input_shape"],
            "dtype": info["input_dtype"],
            **info["input_quantization"],
        },
        "output": {
            "shape": info["output_shape"],
            "dtype": info["output_dtype"],
            **info["output_quantization"],
        },
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"Quant format        : {args.quant_format} ({conversion_config['storage_dtype']})")
    print(f"Quant granularity   : {args.quant_granularity if args.quant_format in ('int8', 'int16') else 'not_applicable'}")
    print(f"Deploy TFLite       : {output_path}")
    print(f"Quantization report : {report_path}")
    print(f"Input               : {info['input_shape']} {info['input_dtype']} {info['input_quantization']}")
    print(f"Output              : {info['output_shape']} {info['output_dtype']} {info['output_quantization']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
