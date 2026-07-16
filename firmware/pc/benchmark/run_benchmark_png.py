import argparse
import glob
import json
import os
import time
import warnings
from pathlib import Path

# Keep TensorFlow's optional CUDA / CPU feature banner out of benchmark logs.
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")

import numpy as np
import serial
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent
CNN_DIR = SCRIPT_DIR.parent

PORT = os.environ.get("BENCHMARK_PORT", "COM6")
BAUDRATE = int(os.environ.get("BENCHMARK_BAUDRATE", "115200"))
ESP2_PORT = os.environ.get("OUTPUT_ESP2_PORT", os.environ.get("ESP2_PORT", ""))
ESP2_BAUDRATE = int(os.environ.get("OUTPUT_ESP2_BAUDRATE", os.environ.get("ESP2_BAUDRATE", "115200")))
ESP2_TIMEOUT_SEC = float(os.environ.get("OUTPUT_ESP2_TIMEOUT_SEC", os.environ.get("ESP2_TIMEOUT_SEC", "0.3")))
DEFAULT_DATA_DIR = CNN_DIR / "dataset" / "test" / "tflite"
READY_TIMEOUT_SEC = int(os.environ.get("BENCHMARK_READY_TIMEOUT_SEC", "30"))
READY_RETRY_LIMIT = int(os.environ.get("BENCHMARK_READY_RETRY_LIMIT", "2"))
RESULT_TIMEOUT_SEC = int(os.environ.get("BENCHMARK_RESULT_TIMEOUT_SEC", "20"))
DEFAULT_TFLITE_MODEL_PATH = CNN_DIR / "artifacts" / "models" / "MobileNetV2_finetuned_int8.tflite"
ENABLE_OUTPUT_COMPARE = os.environ.get("BENCHMARK_COMPARE_OUTPUT", "1") != "0"

MODEL_INPUT_WIDTH = 96
MODEL_INPUT_HEIGHT = 96
DEFAULT_FRAME_WIDTH = int(os.environ.get("BENCHMARK_FRAME_WIDTH", "160"))
DEFAULT_FRAME_HEIGHT = int(os.environ.get("BENCHMARK_FRAME_HEIGHT", "160"))
DEFAULT_CLASS_NAMES = ["up", "ok", "thumb", "palm", "rock", "stone"]
CLASS_NAMES = list(DEFAULT_CLASS_NAMES)
ESP2_DIRECTION_CLASSES = {"up", "down", "right", "left", "null"}
PREPROCESS_MODE = os.environ.get("BENCHMARK_PREPROCESS_MODE", "resize").lower()
HAND_CROP_MARGIN_PERCENT = int(os.environ.get("BENCHMARK_HAND_CROP_MARGIN_PERCENT", "18"))
HAND_CROP_MIN_AREA_PERCENT = int(os.environ.get("BENCHMARK_HAND_CROP_MIN_AREA_PERCENT", "1"))
HAND_CROP_DARK_DELTA = int(os.environ.get("BENCHMARK_HAND_CROP_DARK_DELTA", "25"))
HAND_CROP_MIN_THRESHOLD = int(os.environ.get("BENCHMARK_HAND_CROP_MIN_THRESHOLD", "70"))
HAND_CROP_MAX_THRESHOLD = int(os.environ.get("BENCHMARK_HAND_CROP_MAX_THRESHOLD", "205"))


def resolve_data_dir(path_text):
    path = Path(path_text).expanduser()
    if path.is_absolute():
        return path
    return CNN_DIR / path


def resolve_project_path(path_text):
    path = Path(path_text).expanduser()
    if path.is_absolute():
        return path
    return CNN_DIR / path



def load_class_names_for_model(model_path):
    stem = Path(model_path).stem
    report_dir = CNN_DIR / "artifacts" / "reports"
    report_names = [f"{stem}_quantization_report.json"]
    if stem.endswith("_int8"):
        report_names.append(f"{stem[:-5]}_quantization_report.json")

    for report_name in report_names:
        report_path = report_dir / report_name
        if not report_path.exists():
            continue
        try:
            with open(report_path, "r", encoding="utf-8") as file:
                report = json.load(file)
            class_order = report.get("class_order")
            if isinstance(class_order, list) and class_order and all(isinstance(name, str) for name in class_order):
                return class_order
        except (OSError, json.JSONDecodeError) as exc:
            print(f"[WARN] Failed to read class map from {report_path}: {exc}")

    return list(DEFAULT_CLASS_NAMES)

def parse_frame_size(size_text):
    text = size_text.lower().replace(" ", "")
    if "x" not in text:
        value = int(text)
        return value, value
    width_text, height_text = text.split("x", 1)
    return int(width_text), int(height_text)


def parse_args():
    parser = argparse.ArgumentParser(description="Benchmark ESP32-S3 TFLite Micro gesture inference.")
    parser.add_argument(
        "--dataset",
        "--data-dir",
        dest="data_dir",
        default=os.environ.get("BENCHMARK_DATA_DIR", str(DEFAULT_DATA_DIR)),
        help="Dataset folder. Relative paths are resolved from firmware/pc.",
    )
    parser.add_argument(
        "-0",
        "--no-label",
        action="store_true",
        help="Disable dataset label checking and skip Label Accuracy.",
    )
    parser.add_argument(
        "--label",
        choices=["0", "1"],
        default=os.environ.get("BENCHMARK_LABEL", "1"),
        help="Set to 0 to disable label checking, 1 to enable it. Default: 1.",
    )
    parser.add_argument(
        "--model",
        default=os.environ.get("BENCHMARK_TFLITE_MODEL", str(DEFAULT_TFLITE_MODEL_PATH)),
        help="PC reference .tflite path. Relative paths are resolved from firmware/pc.",
    )
    parser.add_argument(
        "--port",
        default=os.environ.get("BENCHMARK_PORT", PORT),
        help="Serial port connected to ESP32-S3. Default: BENCHMARK_PORT or COM6.",
    )
    parser.add_argument(
        "--baudrate",
        "--baud",
        type=int,
        default=int(os.environ.get("BENCHMARK_BAUDRATE", str(BAUDRATE))),
        help="Serial baudrate. Default: BENCHMARK_BAUDRATE or 115200.",
    )
    parser.add_argument(
        "--esp2-port",
        default=os.environ.get("OUTPUT_ESP2_PORT", os.environ.get("ESP2_PORT", ESP2_PORT)),
        help="Optional serial port connected to ESP2 servo controller. If omitted, servo forwarding is disabled.",
    )
    parser.add_argument(
        "--esp2-baudrate",
        "--esp2-baud",
        type=int,
        default=int(os.environ.get("OUTPUT_ESP2_BAUDRATE", os.environ.get("ESP2_BAUDRATE", str(ESP2_BAUDRATE)))),
        help="ESP2 servo-controller serial baudrate. Default: 115200.",
    )
    parser.add_argument(
        "--esp2-timeout",
        type=float,
        default=float(os.environ.get("OUTPUT_ESP2_TIMEOUT_SEC", os.environ.get("ESP2_TIMEOUT_SEC", str(ESP2_TIMEOUT_SEC)))),
        help="ESP2 ACK read timeout in seconds. Default: 0.3.",
    )
    parser.add_argument(
        "--frame-size",
        default=f"{DEFAULT_FRAME_WIDTH}x{DEFAULT_FRAME_HEIGHT}",
        help="Raw grayscale frame size sent to ESP32 before on-device crop. Default: 160x160.",
    )
    parser.add_argument(
        "--no-crop",
        action="store_true",
        help="Disable PC-side reference crop. Use only if ESP32 firmware crop is disabled too.",
    )
    return parser.parse_args()


ARGS = parse_args()
DATA_DIR = resolve_data_dir(ARGS.data_dir)
TFLITE_MODEL_PATH = resolve_project_path(ARGS.model)
CLASS_NAMES = load_class_names_for_model(TFLITE_MODEL_PATH)
FRAME_WIDTH, FRAME_HEIGHT = parse_frame_size(ARGS.frame_size)
PORT = ARGS.port
BAUDRATE = ARGS.baudrate
ESP2_PORT = ARGS.esp2_port
ESP2_BAUDRATE = ARGS.esp2_baudrate
ESP2_TIMEOUT_SEC = ARGS.esp2_timeout
ENABLE_LABELS = (not ARGS.no_label) and ARGS.label == "1"
ENABLE_PC_CROP = not ARGS.no_crop


def parse_expected_label(image_path):
    folder = Path(image_path).parent.name.lower()
    tokens = [token for token in folder.replace("-", "_").split("_") if token]

    for token in tokens:
        if token in CLASS_NAMES:
            return CLASS_NAMES.index(token)

    if folder in CLASS_NAMES:
        return CLASS_NAMES.index(folder)

    # Numeric-only folders are allowed for indexed datasets, e.g. 0/, 1/, 2/.
    # Do not treat legacy folders such as "01_palm" as
    # class 1; that silently corrupts accuracy for the old 10-class dataset.
    if len(tokens) == 1:
        try:
            value = int(tokens[0])
        except ValueError:
            return None
        if 0 <= value < len(CLASS_NAMES):
            return value

    return None


def preprocess_image(image_path):
    img = Image.open(image_path).convert("L")

    if PREPROCESS_MODE == "resize":
        return img.resize((FRAME_WIDTH, FRAME_HEIGHT))

    if PREPROCESS_MODE == "center_crop":
        side = min(img.size)
        left = (img.width - side) // 2
        top = (img.height - side) // 2
        return img.crop((left, top, left + side, top + side)).resize((FRAME_WIDTH, FRAME_HEIGHT))

    if PREPROCESS_MODE == "letterbox":
        img.thumbnail((FRAME_WIDTH, FRAME_HEIGHT))
        canvas = Image.new("L", (FRAME_WIDTH, FRAME_HEIGHT), 0)
        left = (FRAME_WIDTH - img.width) // 2
        top = (FRAME_HEIGHT - img.height) // 2
        canvas.paste(img, (left, top))
        return canvas

    raise ValueError(
        f"Unsupported BENCHMARK_PREPROCESS_MODE={PREPROCESS_MODE}. "
        "Use resize, center_crop, or letterbox."
    )


def label_name(label):
    if label is None:
        return "unmapped"
    if 0 <= label < len(CLASS_NAMES):
        return CLASS_NAMES[label]
    return "unknown"


def print_label_mapping_warnings(image_files):
    folders = sorted({Path(path).parent.name for path in image_files})
    unmapped = [folder for folder in folders if parse_expected_label(Path(folder) / "dummy.png") is None]
    mapped = [
        (folder, parse_expected_label(Path(folder) / "dummy.png"))
        for folder in folders
        if parse_expected_label(Path(folder) / "dummy.png") is not None
    ]

    if mapped:
        print("Mapped labels:")
        for folder, label in mapped:
            print(f"  {folder} -> {label}({label_name(label)})")
    if unmapped:
        print("[WARN] Unmapped dataset folders will not be counted in Label Accuracy:")
        for folder in unmapped:
            print(f"  {folder}")



def open_esp2_serial():
    if not ESP2_PORT:
        print("ESP2 output: disabled (pass --esp2-port COMx to forward predictions to the servo controller)")
        return None
    try:
        esp2 = serial.Serial(ESP2_PORT, ESP2_BAUDRATE, timeout=ESP2_TIMEOUT_SEC, write_timeout=ESP2_TIMEOUT_SEC)
        time.sleep(2.0)
        esp2.reset_input_buffer()
        print(f"ESP2 output: {ESP2_PORT} @ {ESP2_BAUDRATE}")
        return esp2
    except Exception as exc:
        print(f"[WARN] Failed to open ESP2 output port {ESP2_PORT}: {exc}")
        print("       Benchmark will continue without servo forwarding.")
        return None


def forward_prediction_to_esp2(esp2, pred_idx, pred_name):
    if esp2 is None:
        return ""
    if pred_name not in ESP2_DIRECTION_CLASSES:
        return f" | ESP2: skipped(unmapped gesture '{pred_name}')"
    command = f"GESTURE,{pred_idx},{pred_name}\n"
    try:
        esp2.write(command.encode("ascii"))
        esp2.flush()
        ack = esp2.readline().decode("utf-8", errors="ignore").strip()
        if ack:
            return f" | ESP2: {ack}"
        return " | ESP2: sent(no ack)"
    except Exception as exc:
        return f" | ESP2: ERROR({exc})"

def cosine_similarity(a, b):
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom == 0:
        return 1.0 if np.linalg.norm(a - b) == 0 else 0.0
    return float(np.dot(a, b) / denom)


def clamp_int(value, lo, hi):
    return max(lo, min(hi, value))


def find_hand_crop_box(frame):
    flat = frame.reshape(-1)
    threshold = clamp_int(
        int(flat.astype(np.uint64).sum() // flat.size) - HAND_CROP_DARK_DELTA,
        HAND_CROP_MIN_THRESHOLD,
        HAND_CROP_MAX_THRESHOLD,
    )
    foreground = frame < threshold
    ys, xs = np.where(foreground)
    area = int(xs.size)

    min_area = FRAME_WIDTH * FRAME_HEIGHT * HAND_CROP_MIN_AREA_PERCENT // 100
    found = area >= min_area
    if found:
        x0 = int(xs.min())
        x1 = int(xs.max())
        y0 = int(ys.min())
        y1 = int(ys.max())
    else:
        x0, y0, x1, y1 = (0, 0, FRAME_WIDTH - 1, FRAME_HEIGHT - 1)

    crop_w = x1 - x0 + 1
    crop_h = y1 - y0 + 1
    margin_x = crop_w * HAND_CROP_MARGIN_PERCENT // 100
    margin_y = crop_h * HAND_CROP_MARGIN_PERCENT // 100
    box = (
        clamp_int(x0 - margin_x, 0, FRAME_WIDTH - 1),
        clamp_int(y0 - margin_y, 0, FRAME_HEIGHT - 1),
        clamp_int(x1 + margin_x, 0, FRAME_WIDTH - 1),
        clamp_int(y1 + margin_y, 0, FRAME_HEIGHT - 1),
    )
    return box, area, threshold, found


def resize_crop_to_model_input(frame, box):
    x0, y0, x1, y1 = box
    crop_w = x1 - x0 + 1
    crop_h = y1 - y0 + 1
    out = np.empty((MODEL_INPUT_HEIGHT, MODEL_INPUT_WIDTH), dtype=np.uint8)

    for oy in range(MODEL_INPUT_HEIGHT):
        sy_fp = 0 if MODEL_INPUT_HEIGHT == 1 else oy * (crop_h - 1) * 1024 // (MODEL_INPUT_HEIGHT - 1)
        sy = sy_fp >> 10
        fy = sy_fp & 1023
        sy1 = sy + 1 if sy + 1 < crop_h else sy
        for ox in range(MODEL_INPUT_WIDTH):
            sx_fp = 0 if MODEL_INPUT_WIDTH == 1 else ox * (crop_w - 1) * 1024 // (MODEL_INPUT_WIDTH - 1)
            sx = sx_fp >> 10
            fx = sx_fp & 1023
            sx1 = sx + 1 if sx + 1 < crop_w else sx

            p00 = int(frame[y0 + sy, x0 + sx])
            p01 = int(frame[y0 + sy, x0 + sx1])
            p10 = int(frame[y0 + sy1, x0 + sx])
            p11 = int(frame[y0 + sy1, x0 + sx1])
            wx0 = 1024 - fx
            wy0 = 1024 - fy
            value = p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy
            out[oy, ox] = (value + (1 << 19)) >> 20

    return out


def preprocess_frame_to_model_input(raw_frame):
    if not ENABLE_PC_CROP:
        return np.asarray(Image.fromarray(raw_frame).resize((MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT)), dtype=np.uint8)
    box, area, threshold, found = find_hand_crop_box(raw_frame)
    model_frame = resize_crop_to_model_input(raw_frame, box)
    print(f"[PC_CROP] box={box} area={area} threshold={threshold} found={int(found)}")
    return model_frame


class PCTFLiteReference:
    def __init__(self, model_path):
        self.model_path = Path(model_path)
        self.interpreter = None
        self.input_detail = None
        self.output_detail = None
        self.output_scale = None
        self.output_zero_point = None

    def setup(self):
        if not ENABLE_OUTPUT_COMPARE:
            print("[INFO] PC output comparison disabled by BENCHMARK_COMPARE_OUTPUT=0.")
            return False

        if not self.model_path.exists():
            print(f"[WARN] TFLite model not found: {self.model_path}")
            return False

        try:
            from tflite_runtime.interpreter import Interpreter
            backend_name = "tflite_runtime"
        except ImportError as tflite_error:
            try:
                warnings.filterwarnings(
                    "ignore",
                    message=".*tf.lite.Interpreter is deprecated.*",
                    category=UserWarning,
                )
                # TensorFlow 2.18+ may not export Interpreter from the
                # tensorflow.lite package directly, but tf.lite.Interpreter is
                # still available.
                old_stderr_fd = os.dup(2)
                try:
                    with open(os.devnull, "w") as devnull:
                        os.dup2(devnull.fileno(), 2)
                        import tensorflow as tf
                finally:
                    os.dup2(old_stderr_fd, 2)
                    os.close(old_stderr_fd)
                Interpreter = tf.lite.Interpreter
                backend_name = f"tensorflow.lite ({tf.__version__})"
            except (ImportError, AttributeError) as tf_error:
                print("[WARN] PC TFLite reference disabled: TFLite interpreter backend unavailable.")
                print("       Run: python -m pip install -r pc/requirements.txt")
                print(f"       tflite_runtime import error: {tflite_error}")
                print(f"       tensorflow.lite import error: {tf_error}")
                print("       ESP32 benchmark will still run, but output similarity will be skipped.")
                return False

        self.interpreter = Interpreter(model_path=str(self.model_path))
        self.interpreter.allocate_tensors()
        self.input_detail = self.interpreter.get_input_details()[0]
        self.output_detail = self.interpreter.get_output_details()[0]
        self.output_scale, self.output_zero_point = self.output_detail.get("quantization", (0.0, 0))

        print(f"[INFO] PC TFLite reference enabled ({backend_name}): {self.model_path}")
        print(
            "[INFO] PC input: "
            f"shape={self.input_detail['shape'].tolist()}, "
            f"dtype={self.input_detail['dtype']}, "
            f"quant={self.input_detail.get('quantization')}"
        )
        print(
            "[INFO] PC output: "
            f"shape={self.output_detail['shape'].tolist()}, "
            f"dtype={self.output_detail['dtype']}, "
            f"quant={self.output_detail.get('quantization')}"
        )
        return True

    def dequantize_output_scores(self, scores):
        values = np.asarray(scores, dtype=np.float64)
        if self.output_scale:
            return (values - self.output_zero_point) * self.output_scale
        return values / 1000000.0

    def infer_scores(self, raw_u8):
        if self.interpreter is None:
            return None

        input_shape = self.input_detail["shape"]
        input_dtype = self.input_detail["dtype"]
        raw_flat = raw_u8.astype(np.float32).reshape(-1)

        if input_dtype == np.int8 or input_dtype == np.uint8:
            scale, zero_point = self.input_detail.get("quantization", (0.0, 0))
            if not scale:
                raise ValueError("Quantized TFLite input has missing scale.")
            quantized = np.round((raw_flat / 255.0) / scale + zero_point)
            if input_dtype == np.int8:
                model_input = np.clip(quantized, -128, 127).astype(np.int8)
            else:
                model_input = np.clip(quantized, 0, 255).astype(np.uint8)
        elif input_dtype == np.float32:
            model_input = (raw_flat / 255.0).astype(np.float32)
        else:
            raise ValueError(f"Unsupported PC TFLite input dtype: {input_dtype}")

        self.interpreter.set_tensor(
            self.input_detail["index"],
            model_input.reshape(input_shape),
        )
        self.interpreter.invoke()
        output = self.interpreter.get_tensor(self.output_detail["index"]).reshape(-1)

        if output.dtype == np.int8 or output.dtype == np.uint8:
            return output.astype(np.int16)[:len(CLASS_NAMES)]

        return np.round(output.astype(np.float64) * 1000000.0).astype(np.int32)[:len(CLASS_NAMES)]


def load_preprocessed_raw_grayscale(image_path):
    img = preprocess_image(image_path)
    return np.asarray(img, dtype=np.uint8)


def dump_raw_grayscale(image_path, raw, width, height, prefix):
    flat = raw.reshape(-1)
    center_idx = (height // 2) * width + (width // 2)
    checksum = int(flat.astype(np.uint64).sum())
    print(
        f"[{prefix}] {Path(image_path).name} first10={flat[:10].tolist()} "
        f"center[{center_idx}]={int(flat[center_idx])} sum={checksum}"
    )


def wait_for_ready(ser, timeout_sec=READY_TIMEOUT_SEC):
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if "READY" in line:
            return True
        if line:
            print(f"[ESP32 LOG] {line}")

    print(f"[ERROR] Timed out waiting for ESP32 READY after {timeout_sec}s.")
    print("[HINT] Rebuild/flash the TFLite firmware and close idf.py monitor if it owns the port.")
    return False


def send_frame_to_esp32(ser, raw):
    roundtrip_start_ns = time.perf_counter_ns()
    ser.write(raw.tobytes())
    ser.flush()
    return roundtrip_start_ns


def setup_pc_reference_after_serial_ready():
    pc_reference = PCTFLiteReference(TFLITE_MODEL_PATH)
    print("[INFO] Initializing PC TFLite reference. TensorFlow backend may take a while on first load...")
    start_ns = time.perf_counter_ns()
    enabled = pc_reference.setup()
    elapsed_ms = (time.perf_counter_ns() - start_ns) / 1_000_000.0
    if enabled:
        print(f"[INFO] PC TFLite reference initialization took {elapsed_ms:.1f} ms.")
    return pc_reference, enabled


def run_benchmark():
    image_files = []
    for extension in ("*.png", "*.jpg", "*.jpeg", "*.bmp"):
        search_pattern = os.path.join(DATA_DIR, "**", extension)
        image_files.extend(glob.glob(search_pattern, recursive=True))
    image_files = sorted(image_files)
    if not image_files:
        print(f"Error: No image files found in {DATA_DIR}")
        return

    print(f"Found {len(image_files)} images.")
    print(f"Class map: {', '.join(f'{i}={name}' for i, name in enumerate(CLASS_NAMES))}")
    print(f"Preprocess mode: {PREPROCESS_MODE}")
    print(f"Frame size: {FRAME_WIDTH}x{FRAME_HEIGHT}; model input: {MODEL_INPUT_WIDTH}x{MODEL_INPUT_HEIGHT}; crop: {int(ENABLE_PC_CROP)}")
    print(f"TFLite model: {TFLITE_MODEL_PATH}")
    print(f"Serial: {PORT} @ {BAUDRATE}")
    print(f"ESP2 output port: {ESP2_PORT if ESP2_PORT else 'disabled'}")
    if ENABLE_LABELS:
        print_label_mapping_warnings(image_files)
    else:
        print("[INFO] Label checking disabled. Label Accuracy will be skipped.")

    try:
        ser = serial.Serial(PORT, BAUDRATE, timeout=1)
    except Exception as e:
        print(f"Failed to open port {PORT}: {e}")
        return

    esp2_ser = open_esp2_serial()
    total_model_latency_us = 0
    total_preprocess_us = 0
    total_device_compute_us = 0
    total_roundtrip_us = 0
    total_uart_io_overhead_us = 0
    extended_timing_count = 0
    success_count = 0
    correct_count = 0
    labeled_count = 0
    output_compare_count = 0
    output_top1_match_count = 0
    output_mae_total = 0.0
    output_max_error_max = 0.0
    output_cosine_total = 0.0
    output_exact_match_count = 0
    output_prob_mae_total = 0.0
    output_prob_max_error_max = 0.0
    output_prob_cosine_total = 0.0

    print("Awaiting ESP32 sync signal...")
    if not wait_for_ready(ser):
        ser.close()
        if esp2_ser is not None:
            esp2_ser.close()
        return
    pc_reference, pc_reference_enabled = setup_pc_reference_after_serial_ready()
    ser.reset_input_buffer()
    need_ready = False

    try:
        for image_path in image_files:
            filename = Path(image_path).name
            expected_label = parse_expected_label(image_path) if ENABLE_LABELS else None
            raw = load_preprocessed_raw_grayscale(image_path)
            model_input_raw = preprocess_frame_to_model_input(raw)
            reference_scores = pc_reference.infer_scores(model_input_raw) if pc_reference_enabled else None

            if need_ready and not wait_for_ready(ser):
                break

            dump_raw_grayscale(image_path, raw, FRAME_WIDTH, FRAME_HEIGHT, "PC_FRAME_DUMP")
            dump_raw_grayscale(image_path, model_input_raw, MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT, "PC_MODEL_DUMP")
            roundtrip_start_ns = send_frame_to_esp32(ser, raw)
            ready_retry_count = 0
            abort_benchmark = False
            result_deadline = time.time() + RESULT_TIMEOUT_SEC

            while True:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
                if not line and time.time() > result_deadline:
                    print(f"[ERROR] Timed out waiting for RESULT from {filename} after {RESULT_TIMEOUT_SEC}s.")
                    print("[HINT] Check ESP32 monitor output. Firmware should be in RuntimeMode::kTestUartFrame and print RESULT after receiving one frame.")
                    abort_benchmark = True
                    break

                if "READY" in line:
                    if ready_retry_count >= READY_RETRY_LIMIT:
                        print(
                            f"[ERROR] ESP32 returned READY while waiting for RESULT from {filename} "
                            f"more than {READY_RETRY_LIMIT} times."
                        )
                        print(
                            "[HINT] This usually means the firmware rebooted during inference. "
                            "Check the ESP32 panic/backtrace and tensor arena/PSRAM settings."
                        )
                        abort_benchmark = True
                        break

                    ready_retry_count += 1
                    print(
                        f"[WARN] ESP32 returned READY while waiting for RESULT from {filename}; "
                        f"resending frame ({ready_retry_count}/{READY_RETRY_LIMIT})."
                    )
                    roundtrip_start_ns = send_frame_to_esp32(ser, raw)
                    result_deadline = time.time() + RESULT_TIMEOUT_SEC
                    continue

                if line.startswith("RESULT"):
                    roundtrip_us = (time.perf_counter_ns() - roundtrip_start_ns) / 1000.0
                    try:
                        parts = line.split(",")
                        if len(parts) < 6:
                            raise ValueError("RESULT line has too few fields")

                        pred_idx = int(parts[1])
                        model_latency_us = int(parts[2])
                        preprocess_us = int(parts[3])
                        device_compute_us = int(parts[4])
                        score_start = 5
                        extended_timing_count += 1

                        scores = [int(x) for x in parts[score_start:]]
                        if not scores:
                            raise ValueError("RESULT line has no output scores")
                        uart_io_overhead_us = max(0.0, roundtrip_us - device_compute_us)
                        total_model_latency_us += model_latency_us
                        total_roundtrip_us += roundtrip_us
                        total_device_compute_us += device_compute_us
                        total_uart_io_overhead_us += uart_io_overhead_us
                        if preprocess_us is not None:
                            total_preprocess_us += preprocess_us
                        success_count += 1

                        pred_name = CLASS_NAMES[pred_idx] if 0 <= pred_idx < len(CLASS_NAMES) else "unknown"
                        esp2_text = forward_prediction_to_esp2(esp2_ser, pred_idx, pred_name)
                        label_text = ""
                        if expected_label is not None:
                            is_correct = pred_idx == expected_label
                            labeled_count += 1
                            correct_count += int(is_correct)
                            expected_name = CLASS_NAMES[expected_label]
                            label_text = f" | Expected: {expected_label}({expected_name}) | {'OK' if is_correct else 'FAIL'}"

                        compare_text = ""
                        if reference_scores is not None and len(scores) == len(reference_scores):
                            esp_scores = np.asarray(scores, dtype=np.int32)
                            ref_scores = np.asarray(reference_scores, dtype=np.int32)
                            abs_error = np.abs(esp_scores - ref_scores)
                            mae = float(np.mean(abs_error))
                            max_error = float(np.max(abs_error))
                            cos = cosine_similarity(esp_scores, ref_scores)
                            exact_match = bool(np.array_equal(esp_scores, ref_scores))
                            esp_prob = pc_reference.dequantize_output_scores(esp_scores)
                            ref_prob = pc_reference.dequantize_output_scores(ref_scores)
                            prob_abs_error = np.abs(esp_prob - ref_prob)
                            prob_mae = float(np.mean(prob_abs_error))
                            prob_max_error = float(np.max(prob_abs_error))
                            prob_cos = cosine_similarity(esp_prob, ref_prob)
                            ref_pred = int(np.argmax(ref_scores))
                            top1_match = pred_idx == ref_pred

                            output_compare_count += 1
                            output_top1_match_count += int(top1_match)
                            output_exact_match_count += int(exact_match)
                            output_mae_total += mae
                            output_max_error_max = max(output_max_error_max, max_error)
                            output_cosine_total += cos
                            output_prob_mae_total += prob_mae
                            output_prob_max_error_max = max(output_prob_max_error_max, prob_max_error)
                            output_prob_cosine_total += prob_cos
                            compare_text = (
                                f" | PCRef: {ref_pred}({label_name(ref_pred)})"
                                f" | Top1Match: {'OK' if top1_match else 'FAIL'}"
                                f" | Exact: {'OK' if exact_match else 'FAIL'}"
                                f" | MAE: {mae:.2f}"
                                f" | MaxErr: {max_error:.0f}"
                                f" | Cos: {cos:.5f}"
                                f" | ProbMAE: {prob_mae:.6f}"
                                f" | RefScores: {ref_scores.tolist()}"
                            )

                        print(
                            f"File: {filename:<28} | Prediction: {pred_idx}({pred_name}) "
                            f"| Model: {model_latency_us} us"
                            f"{'' if preprocess_us is None else f' | Preprocess: {preprocess_us} us'}"
                            f" | Roundtrip: {roundtrip_us:.0f} us"
                            f" | UART/IO: {uart_io_overhead_us:.0f} us"
                            f"{label_text}{compare_text}{esp2_text} | Scores: {scores}"
                        )
                    except ValueError as e:
                        print(f"Error parsing response for {filename}: {e}: {line}")
                    break
                elif line:
                    print(f"[ESP32 LOG] {line}")

            if abort_benchmark:
                break
            if success_count > 0:
                need_ready = True
    finally:
        ser.close()
        if esp2_ser is not None:
            esp2_ser.close()
    if success_count > 0:
        avg_model_latency_ms = total_model_latency_us / success_count / 1000.0
        avg_device_compute_ms = total_device_compute_us / success_count / 1000.0
        avg_roundtrip_ms = total_roundtrip_us / success_count / 1000.0
        avg_uart_io_overhead_ms = total_uart_io_overhead_us / success_count / 1000.0
        model_fps = 1000000.0 / (total_model_latency_us / success_count)
        device_compute_fps = 1000000.0 / (total_device_compute_us / success_count)
        end_to_end_fps = 1000000.0 / (total_roundtrip_us / success_count)
        print("\n--- Benchmark Summary ---")
        print(f"Images Processed : {success_count}")
        if not ENABLE_LABELS:
            print("Label Accuracy   : skipped (disabled)")
        elif labeled_count:
            print(f"Label Accuracy   : {correct_count}/{labeled_count} ({correct_count / labeled_count * 100:.2f}%)")
            print(f"Labeled Images   : {labeled_count}/{success_count}")
        else:
            print("Label Accuracy   : skipped (no folders matched the active class map)")
        print(f"Average Model Latency      : {avg_model_latency_ms:.2f} ms")
        if extended_timing_count:
            avg_preprocess_ms = total_preprocess_us / extended_timing_count / 1000.0
            print(f"Average Preprocess Latency : {avg_preprocess_ms:.2f} ms")
            print(f"Average Device Compute     : {avg_device_compute_ms:.2f} ms")
        print(f"Average Host Roundtrip     : {avg_roundtrip_ms:.2f} ms")
        print(f"Estimated UART/IO Overhead : {avg_uart_io_overhead_ms:.2f} ms")
        print(f"Model-only Throughput      : {model_fps:.2f} FPS")
        if extended_timing_count:
            print(f"Device Compute Throughput  : {device_compute_fps:.2f} FPS")
        print(f"End-to-end UART Throughput : {end_to_end_fps:.2f} FPS")
        if output_compare_count:
            print("\n--- Output Similarity Summary ---")
            print("Reference         : PC TFLite inference using the same raw frame and crop pipeline")
            print(f"Compared Samples  : {output_compare_count}")
            print(f"Top-1 Match       : {output_top1_match_count}/{output_compare_count} ({output_top1_match_count / output_compare_count * 100:.2f}%)")
            print(f"Exact Score Match : {output_exact_match_count}/{output_compare_count} ({output_exact_match_count / output_compare_count * 100:.2f}%)")
            print(f"Average Score MAE : {output_mae_total / output_compare_count:.2f} int units")
            print(f"Max Score Error   : {output_max_error_max:.0f} int units")
            print(f"Average Cosine    : {output_cosine_total / output_compare_count:.6f}")
            print(f"Average Prob MAE  : {output_prob_mae_total / output_compare_count:.8f}")
            print(f"Max Prob Error    : {output_prob_max_error_max:.8f}")
            print(f"Average Prob Cos  : {output_prob_cosine_total / output_compare_count:.6f}")
        print("-------------------------")


if __name__ == "__main__":
    run_benchmark()





