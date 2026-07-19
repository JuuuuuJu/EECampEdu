import argparse
import os
from pathlib import Path

os.environ.setdefault("TF_ENABLE_ONEDNN_OPTS", "0")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import tensorflow as tf
from PIL import Image

import class_map  # shared class-order loader (same folder)

SCRIPT_DIR = Path(__file__).resolve().parent
TRAIN_DIR = SCRIPT_DIR / "dataset" / "train"
VALIDATION_DIR = SCRIPT_DIR / "dataset" / "validation"
MODELS_DIR = SCRIPT_DIR / "models" / "tf"
MODEL_PATH = MODELS_DIR / "MobileNetV2_finetuned.keras"
ONNX_PATH = MODELS_DIR / "MobileNetV2_finetuned.onnx"
IMG_SIZE = (96, 96)
# Default gesture classes; overridden by model_finetune/dataset/class_map.json when
# a student uploads their own six class folders (arbitrary names supported).
DEFAULT_CLASS_NAMES = ["up", "ok", "thumb", "palm", "rock", "stone"]
CLASS_NAMES = class_map.load_class_order(default=DEFAULT_CLASS_NAMES)
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp"}


def parse_args():
    parser = argparse.ArgumentParser(description="Train deploy-ready MobileNetV2 gesture classifier.")
    parser.add_argument("--check-only", action="store_true", help="Only construct the model and print handoff info.")
    parser.add_argument("--epochs", type=int, default=15, help="Head training epochs. Default: 15.")
    parser.add_argument("--batch-size", type=int, default=32, help="Batch size. Default: 32.")
    parser.add_argument("--alpha", type=float, default=0.35, help="MobileNetV2 width multiplier. Default: 0.35.")
    parser.add_argument("--weights", default="imagenet", choices=["imagenet", "none"], help="MobileNetV2 source weights. Default: imagenet.")
    parser.add_argument("--export-onnx", action="store_true", help="Optionally export ONNX if tf2onnx is installed.")
    return parser.parse_args()


def iter_class_images(root, class_name):
    class_dir = root / class_name
    if not class_dir.is_dir():
        return []
    return sorted(path for path in class_dir.rglob("*") if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS)


def load_dataset(root):
    x_data = []
    y_data = []
    counts = {}
    for label, class_name in enumerate(CLASS_NAMES):
        paths = iter_class_images(root, class_name)
        counts[class_name] = len(paths)
        for path in paths:
            image = Image.open(path).convert("L").resize(IMG_SIZE)
            x_data.append(np.asarray(image, dtype=np.float32) / 255.0)
            y_data.append(label)
    if not x_data:
        raise RuntimeError(f"No images found under {root} for classes {CLASS_NAMES}")
    x = np.expand_dims(np.asarray(x_data, dtype=np.float32), axis=-1)
    y = np.asarray(y_data, dtype=np.int32)
    return x, y, counts


def split_train_validation(x, y, validation_ratio=0.2):
    rng = np.random.default_rng(123)
    indices = np.arange(len(x))
    rng.shuffle(indices)
    split = max(1, int(len(indices) * (1.0 - validation_ratio)))
    train_idx = indices[:split]
    val_idx = indices[split:]
    return x[train_idx], y[train_idx], x[val_idx], y[val_idx]


def build_model(alpha=0.35, weights="imagenet"):
    inputs = tf.keras.Input(shape=(IMG_SIZE[1], IMG_SIZE[0], 1), name="input")
    x = tf.keras.layers.Concatenate(axis=-1, name="gray_to_rgb")([inputs, inputs, inputs])
    x = tf.keras.layers.Rescaling(scale=2.0, offset=-1.0, name="mobilenet_rescale")(x)

    base_weights = None if weights == "none" else "imagenet"
    base = tf.keras.applications.MobileNetV2(
        input_shape=(IMG_SIZE[1], IMG_SIZE[0], 3),
        alpha=alpha,
        include_top=False,
        weights=base_weights,
    )
    base.trainable = False

    x = base(x, training=False)
    x = tf.keras.layers.GlobalAveragePooling2D(name="global_average_pooling")(x)
    x = tf.keras.layers.Dropout(0.2, name="dropout")(x)
    x = tf.keras.layers.Dense(64, activation="relu", name="dense_features")(x)
    outputs = tf.keras.layers.Dense(len(CLASS_NAMES), activation="softmax", name="predictions")(x)
    return tf.keras.Model(inputs, outputs, name="MobileNetV2_gesture_classifier")


def evaluate_predictions(model, x, y, title):
    preds = model.predict(x, batch_size=32, verbose=0)
    pred_labels = np.argmax(preds, axis=1)
    accuracy = float(np.mean(pred_labels == y))
    variation = float(np.mean(np.std(preds, axis=0)))
    pred_counts = {CLASS_NAMES[index]: int(np.sum(pred_labels == index)) for index in range(len(CLASS_NAMES))}
    print(f"{title} accuracy: {int(np.sum(pred_labels == y))}/{len(y)} ({accuracy * 100:.2f}%)")
    print(f"{title} prediction dist: {pred_counts}")
    print(f"{title} output variation: {variation:.8f}")
    return accuracy, variation


def main():
    args = parse_args()
    MODELS_DIR.mkdir(parents=True, exist_ok=True)

    if args.check_only:
        model = build_model(alpha=args.alpha, weights=args.weights)
        print("=== MobileNetV2 training handoff check ===")
        print(f"Input shape : {model.input_shape}")
        print(f"Output shape: {model.output_shape}")
        print(f"Class order : {', '.join(f'{i}={name}' for i, name in enumerate(CLASS_NAMES))}")
        print(f"Source model: {MODEL_PATH}")
        print("Status      : OK")
        return 0

    print("=== Step 1: Loading local gesture dataset ===")
    x_train_all, y_train_all, train_counts = load_dataset(TRAIN_DIR)
    print(f"Train directory: {TRAIN_DIR}")
    print(f"Train classes  : {train_counts}")

    if VALIDATION_DIR.exists() and any(iter_class_images(VALIDATION_DIR, class_name) for class_name in CLASS_NAMES):
        x_train, y_train = x_train_all, y_train_all
        x_val, y_val, val_counts = load_dataset(VALIDATION_DIR)
        print(f"Validation directory: {VALIDATION_DIR}")
        print(f"Validation classes  : {val_counts}")
    else:
        print("[WARN] dataset/validation not found. Splitting dataset/train into train/validation.")
        x_train, y_train, x_val, y_val = split_train_validation(x_train_all, y_train_all)

    print(f"Train samples     : {len(x_train)}")
    print(f"Validation samples: {len(x_val)}")
    print(f"Class order       : {', '.join(f'{i}={name}' for i, name in enumerate(CLASS_NAMES))}")

    print("\n=== Step 2: Building MobileNetV2 feature extractor ===")
    model = build_model(alpha=args.alpha, weights=args.weights)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    model.summary()

    callbacks = [
        tf.keras.callbacks.EarlyStopping(monitor="val_accuracy", patience=5, mode="max", restore_best_weights=True),
    ]

    print("\n=== Step 3: Training classification head ===")
    history = model.fit(
        x_train,
        y_train,
        validation_data=(x_val, y_val),
        epochs=args.epochs,
        batch_size=args.batch_size,
        callbacks=callbacks,
        verbose=1,
    )

    print("\n=== Step 4: Validating and saving deploy source model ===")
    evaluate_predictions(model, x_val, y_val, "In-memory .keras")
    print(f"Saving Keras model to {MODEL_PATH}...")
    model.save(MODEL_PATH)

    reloaded = tf.keras.models.load_model(MODEL_PATH, compile=False)
    reloaded_acc, reloaded_variation = evaluate_predictions(reloaded, x_val, y_val, "Reloaded .keras")
    if reloaded_acc < 0.50 or reloaded_variation < 1e-4:
        raise RuntimeError(
            "Saved .keras model looks unhealthy. "
            f"accuracy={reloaded_acc * 100:.2f}%, output_variation={reloaded_variation:.8f}"
        )

    if args.export_onnx:
        print(f"Converting and saving ONNX model to {ONNX_PATH}...")
        try:
            import tf2onnx
            spec = (tf.TensorSpec(reloaded.inputs[0].shape, reloaded.inputs[0].dtype, name="input"),)
            tf2onnx.convert.from_keras(reloaded, input_signature=spec, opset=13, output_path=str(ONNX_PATH))
            print(f"ONNX model saved successfully at {ONNX_PATH}")
        except Exception as exc:
            print(f"Failed to save ONNX model: {exc}")

    best_val_acc = max(history.history.get("val_accuracy", [0.0]))
    print("\n" + "=" * 70)
    print(f"{'Model / Stage':<25} | {'Best Val Acc':<12} | {'Saved Acc':<12}")
    print("=" * 70)
    print(f"{'MobileNetV2 feature head':<25} | {best_val_acc * 100:<11.2f}% | {reloaded_acc * 100:<11.2f}%")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
