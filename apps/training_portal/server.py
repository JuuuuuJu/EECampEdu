"""Student-facing AI PC training portal web server.

Runs on the AI PC (one per team). Students open this page from the browser on
their own lab PC and drive the *existing* repository training / quantization
scripts through a GUI instead of typing CLI commands.

Design constraints (see apps/training_portal/README.md):
  * No arbitrary shell execution. Every job is built from a strict allowlist of
    project scripts + validated arguments. `shell=False` everywhere.
  * The ML pipeline is NOT reimplemented here. This server only *launches* the
    scripts that already live under model_finetune/ and firmware/pc/tools/.
  * Training and quantization run as background jobs (subprocess + thread) so a
    single HTTP request never blocks on a multi-minute job.
  * Uploaded zips, job logs, and web-run metadata live under the git-ignored
    apps/training_portal/runs/ folder.

Start (on the AI PC):
    conda activate eecampedu
    python apps/training_portal/server.py --host 0.0.0.0 --port 8080
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from flask import (
    Flask,
    abort,
    jsonify,
    request,
    send_file,
    send_from_directory,
)
from werkzeug.exceptions import HTTPException

# --------------------------------------------------------------------------- #
# Paths
# --------------------------------------------------------------------------- #
APP_DIR = Path(__file__).resolve().parent
REPO_ROOT = APP_DIR.parents[1]              # apps/training_portal -> repo root
TEMPLATES_DIR = APP_DIR / "templates"
RUNS_DIR = APP_DIR / "runs"                 # git-ignored runtime folder
UPLOAD_DIR = RUNS_DIR / "uploads"
JOBS_DIR = RUNS_DIR / "jobs"

MODEL_FINETUNE_DIR = REPO_ROOT / "model_finetune"
DATASET_DIR = MODEL_FINETUNE_DIR / "dataset"
TF_MODELS_DIR = MODEL_FINETUNE_DIR / "models" / "tf"
PYTORCH_MODELS_DIR = MODEL_FINETUNE_DIR / "models" / "pytorch"
ARTIFACT_MODELS_DIR = REPO_ROOT / "firmware" / "pc" / "artifacts" / "models"
ARTIFACT_REPORTS_DIR = REPO_ROOT / "firmware" / "pc" / "artifacts" / "reports"

# Fallback gesture class order when no dataset has been imported yet. Students may
# upload their own six class folders with ARBITRARY names; the imported order is
# recorded in model_finetune/dataset/class_mapping.json and used across the pipeline.
CLASS_NAMES = ["up", "ok", "thumb", "palm", "rock", "stone"]

# Shared class-order / action-mapping helper (model_finetune/class_map.py).
sys.path.insert(0, str(MODEL_FINETUNE_DIR))
try:
    import class_map as class_map_lib
except Exception:  # pragma: no cover - portal still runs without it
    class_map_lib = None

CLASS_MAP_PATH = DATASET_DIR / "class_mapping.json"
NUM_CLASSES = 6
VALIDATION_RATIO = 0.2  # auto-split fraction when the zip has no validation split
# ESP32-S3 model-partition offset (see firmware/esp/partitions.csv). Served via
# /api/flash-meta to the browser Web Serial flasher; never shown in the UI.
FLASH_OFFSET = 0x310000
# Robot-arm output actions a class may be mapped to (matches ESP2 output firmware).
OUTPUT_ACTIONS = ["up", "down", "left", "right", "clamp", "release"]


def _current_class_order():
    """Saved class order if a dataset was imported, else the fallback default."""
    if class_map_lib is not None:
        order = class_map_lib.load_class_order(default=None, path=CLASS_MAP_PATH)
        if order:
            return order
    return list(CLASS_NAMES)

# --------------------------------------------------------------------------- #
# Allowlist: the ONLY scripts this portal is permitted to run.
# --------------------------------------------------------------------------- #
# key -> recipe definition. `script` is resolved relative to REPO_ROOT and must
# exist. `supports` lists the optional tuning knobs the GUI may pass; anything
# not listed is ignored. This is what keeps the portal from becoming a generic
# "run any command" endpoint.
TRAINING_RECIPES = {
    "tf_mobilenet": {
        "framework": "tensorflow",
        "label": "MobileNetV2 (TensorFlow, recommended)",
        "script": "model_finetune/train_mobilenet.py",
        "keras_model_name": "MobileNetV2_finetuned",
        "supports": ["epochs", "batch_size", "alpha", "export_onnx"],
        "description": "Transfer-learned MobileNetV2, best accuracy/speed on the ESP32-S3.",
    },
    "tf_mini_resnet": {
        "framework": "tensorflow",
        "label": "Mini ResNet (TensorFlow)",
        "script": "model_finetune/train_mini_resnet.py",
        "keras_model_name": "Mini_ResNet_finetuned",
        "supports": [],  # module-level script, no CLI arguments
        "description": "Lightweight Keras Mini ResNet. Runs with default settings.",
    },
    "pytorch_mini_resnet": {
        "framework": "pytorch",
        "label": "Mini ResNet (PyTorch)",
        "script": "model_finetune/pytorch/train_mini_resnet.py",
        "keras_model_name": "Mini_ResNet_finetuned",
        "supports": [],  # has its own internal warmup/fine-tune flow, no CLI args
        "description": "PyTorch Mini ResNet; also exports .pth/.onnx and a .keras handoff.",
    },
}

QUANTIZE_SCRIPT = "firmware/pc/tools/quantize_keras_model.py"
# Only formats/granularities that the stock TensorFlow Lite converter can actually
# produce AND that TFLite Micro can run are offered — this is what keeps an
# unsupported, unflashable model from ever being generated (see Q6 below).
# Deliberately NOT offered (verified against TF 2.10.1's Keras->TFLite PTQ path):
#   * per-group granularity — stock TFLite exposes only per-tensor and per-channel
#     (the sole knob is converter._experimental_disable_per_channel; no group/block
#     op set or attribute exists). Blockwise/sub-channel quant is a newer LiteRT
#     feature absent from this flow.
#   * int32 format — not a deployable TFLite type. inference_input/output_type is
#     restricted to {float32, int8, uint8} (int16 only via its experimental ops
#     set); int32 exists only for internal bias accumulators, never as a model
#     format. It would be PC-comparison-only, so it is excluded rather than faked.
QUANT_FORMATS = ["int8", "int16", "float32"]
QUANT_GRANULARITIES = ["per-channel", "per-tensor"]

# Directories whose files may be listed as artifacts and downloaded. Any download
# request is resolved and confirmed to live inside one of these roots (no traversal).
ARTIFACT_ROOTS = [TF_MODELS_DIR, PYTORCH_MODELS_DIR, ARTIFACT_MODELS_DIR, ARTIFACT_REPORTS_DIR]
ARTIFACT_EXTENSIONS = {".keras", ".pth", ".onnx", ".tflite", ".json", ".h5"}


# --------------------------------------------------------------------------- #
# Background job manager
# --------------------------------------------------------------------------- #
class Job:
    """One background subprocess (a training or quantization run)."""

    def __init__(self, job_id, kind, label, cmd, log_path, meta_path):
        self.id = job_id
        self.kind = kind          # "train" | "quantize"
        self.label = label
        self.cmd = cmd
        self.log_path = log_path
        self.meta_path = meta_path
        self.status = "starting"  # starting | running | succeeded | failed
        self.returncode = None
        self.created_at = _now_iso()
        self.finished_at = None
        self.proc = None

    def to_dict(self):
        return {
            "id": self.id,
            "kind": self.kind,
            "label": self.label,
            # cmd is exposed for transparency but as a joined display string only.
            "command": " ".join(_display_arg(a) for a in self.cmd),
            "status": self.status,
            "returncode": self.returncode,
            "created_at": self.created_at,
            "finished_at": self.finished_at,
            "log": self.id,  # log fetched via /api/jobs/<id>/log
        }

    def persist(self):
        try:
            self.meta_path.write_text(json.dumps(self.to_dict(), indent=2))
        except OSError:
            pass


class JobManager:
    """Serializes jobs: at most one training/quantization job runs at a time.

    A shared classroom AI PC has limited CPU/RAM; running two TensorFlow jobs at
    once would thrash. Callers get HTTP 409 while a job is active.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._active_id = None
        self._jobs = {}  # id -> Job (this process lifetime)
        self._counter = 0

    def _new_id(self, kind):
        self._counter += 1
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        return f"{stamp}-{kind}-{self._counter:03d}"

    def active_job(self):
        with self._lock:
            if self._active_id is None:
                return None
            return self._jobs.get(self._active_id)

    def start(self, kind, label, cmd, env=None):
        with self._lock:
            if self._active_id is not None:
                active = self._jobs.get(self._active_id)
                if active and active.status in ("starting", "running"):
                    raise JobBusyError(active)
                self._active_id = None

            job_id = self._new_id(kind)
            job_dir = JOBS_DIR / job_id
            job_dir.mkdir(parents=True, exist_ok=True)
            log_path = job_dir / "job.log"
            meta_path = job_dir / "meta.json"
            job = Job(job_id, kind, label, cmd, log_path, meta_path)
            self._jobs[job_id] = job
            self._active_id = job_id

        job.persist()
        thread = threading.Thread(target=self._run, args=(job, env), daemon=True)
        thread.start()
        return job

    def _run(self, job, env):
        run_env = dict(os.environ)
        run_env["PYTHONUNBUFFERED"] = "1"  # promptly flush child output for live logs
        if env:
            run_env.update(env)
        try:
            with open(job.log_path, "w", encoding="utf-8", buffering=1) as log_fh:
                header = (
                    f"# job {job.id}\n"
                    f"# kind: {job.kind}\n"
                    f"# command: {' '.join(_display_arg(a) for a in job.cmd)}\n"
                    f"# started: {job.created_at}\n"
                    f"{'-' * 60}\n"
                )
                log_fh.write(header)
                log_fh.flush()
                job.status = "running"
                job.persist()
                job.proc = subprocess.Popen(
                    job.cmd,
                    cwd=str(REPO_ROOT),
                    stdout=log_fh,
                    stderr=subprocess.STDOUT,
                    env=run_env,
                    shell=False,
                )
                returncode = job.proc.wait()
            job.returncode = returncode
            job.status = "succeeded" if returncode == 0 else "failed"
        except Exception as exc:  # noqa: BLE001 - record any launch failure in the log
            job.status = "failed"
            job.returncode = -1
            try:
                with open(job.log_path, "a", encoding="utf-8") as log_fh:
                    log_fh.write(f"\n[portal] failed to run job: {exc}\n")
            except OSError:
                pass
        finally:
            job.finished_at = _now_iso()
            job.persist()
            with self._lock:
                if self._active_id == job.id:
                    self._active_id = None

    def get(self, job_id):
        return self._jobs.get(job_id)

    def list(self):
        return sorted(self._jobs.values(), key=lambda j: j.created_at, reverse=True)


class JobBusyError(Exception):
    def __init__(self, active_job):
        super().__init__("A job is already running.")
        self.active_job = active_job


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
def _now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _display_arg(arg):
    arg = str(arg)
    return f'"{arg}"' if " " in arg else arg


def _safe_int(value, name, minimum, maximum):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        raise ValueError(f"{name} must be an integer")
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return parsed


def _safe_float(value, name, minimum, maximum):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"{name} must be a number")
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return parsed


def _available_keras_models():
    """Allowed source models shown in the quantization dropdown.

    IDs are framework-prefixed relative paths (tf/<name>, pytorch/<name>) so
    TensorFlow and PyTorch exports can coexist even when their basenames are
    identical. Recursing lets students organize model runs in subfolders.
    """
    models = []
    for prefix, root in (("tf", TF_MODELS_DIR), ("pytorch", PYTORCH_MODELS_DIR)):
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*.keras")) + sorted(root.rglob("*.h5")):
            try:
                rel = path.relative_to(root)
            except ValueError:
                continue
            if any(part.startswith(".") for part in rel.parts):
                continue
            model_id = f"{prefix}/{rel.as_posix()}"
            stat = path.stat()
            models.append({
                "id": model_id,
                "label": f"{model_id} ({stat.st_size / 1024:.1f} KB)",
                "framework": prefix,
                "name": path.name,
                "relative_path": rel.as_posix(),
                "size_bytes": stat.st_size,
                "modified": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat(),
            })
    return models


def _resolve_keras_model_id(model_id):
    """Resolve a strict model ID to a .keras path under the allowed model roots."""
    if not model_id:
        return None
    roots = {
        "tf": TF_MODELS_DIR,
        "pytorch": PYTORCH_MODELS_DIR,
    }
    if "/" in model_id:
        prefix, rel = model_id.split("/", 1)
        root = roots.get(prefix)
        if root is None or not rel:
            return None
        rel_path = Path(rel)
        if rel_path.is_absolute() or ".." in rel_path.parts:
            return None
        if rel_path.suffix.lower() not in {".keras", ".h5"}:
            rel_path = Path(f"{rel}.keras")
        if not all(re.fullmatch(r"[A-Za-z0-9_. -]+", part) for part in rel_path.parts):
            return None
        candidate = (root / rel_path).resolve()
        try:
            candidate.relative_to(root.resolve())
        except ValueError:
            return None
        return candidate if candidate.is_file() else None

    # Backward compatibility for older UI values that only sent the basename.
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", model_id):
        return None
    matches = []
    for root in (TF_MODELS_DIR, PYTORCH_MODELS_DIR):
        for suffix in (".keras", ".h5"):
            candidate = (root / f"{model_id}{suffix}").resolve()
            if candidate.is_file():
                matches.append(candidate)
    return matches[0] if len(matches) == 1 else None


def _save_uploaded_source_model(upload, framework):
    """Store a user-uploaded .keras/.h5 model under the selected source root."""
    roots = {
        "tf": TF_MODELS_DIR,
        "pytorch": PYTORCH_MODELS_DIR,
    }
    root = roots.get(framework)
    if root is None:
        raise ValueError("framework must be 'tf' or 'pytorch'")
    filename = Path(upload.filename or "").name
    if not filename:
        raise ValueError("No model file selected.")
    suffix = Path(filename).suffix.lower()
    if suffix not in {".keras", ".h5"}:
        raise ValueError("Source model must be a .keras or .h5 file.")
    safe_name = re.sub(r"[^A-Za-z0-9._ -]", "_", filename).strip(" .")
    if not safe_name:
        raise ValueError("Model filename is invalid.")
    root.mkdir(parents=True, exist_ok=True)
    target = (root / safe_name).resolve()
    target.relative_to(root.resolve())
    upload.save(str(target))
    return target


def _resolve_within_roots(candidate):
    """Return the resolved path if it lives inside an allowed artifact root, else None."""
    try:
        resolved = candidate.resolve()
    except OSError:
        return None
    for root in ARTIFACT_ROOTS:
        try:
            resolved.relative_to(root.resolve())
            return resolved
        except ValueError:
            continue
    return None


# --------------------------------------------------------------------------- #
# Dataset upload / extraction (zip-slip safe)
# --------------------------------------------------------------------------- #
def _extract_zip_safely(zip_path, dest_dir):
    """Extract a zip into dest_dir, rejecting absolute paths and .. traversal."""
    dest_dir = dest_dir.resolve()
    extracted = 0
    with zipfile.ZipFile(zip_path) as zf:
        for member in zf.infolist():
            name = member.filename
            if name.endswith("/"):
                continue
            # Normalize and reject anything that escapes dest_dir.
            target = (dest_dir / name).resolve()
            if not str(target).startswith(str(dest_dir) + os.sep):
                raise ValueError(f"Unsafe path in zip: {name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            extracted += 1
    return extracted


IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"}
TRAIN_SPLIT_NAMES = {"train", "training"}
VALIDATION_SPLIT_NAMES = {"validation", "valid", "val"}


def _class_image_count(class_root, class_name):
    class_dir = class_root / class_name
    if not class_dir.is_dir():
        return 0
    return sum(
        1
        for path in class_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
    )


def _detect_split_name(path, base_dir):
    """Infer train/validation from the candidate path or one of its parents."""
    try:
        parts = path.relative_to(base_dir).parts
    except ValueError:
        parts = path.parts
    lowered = [part.lower() for part in parts]
    for part in lowered:
        if part in TRAIN_SPLIT_NAMES:
            return "train"
        if part in VALIDATION_SPLIT_NAMES:
            return "validation"
    return None


def _immediate_class_dirs(directory):
    """Names of immediate subfolders of `directory` that directly hold images.

    Class-folder names are ARBITRARY (e.g. n1..n6); a folder qualifies as a class
    folder if it contains at least one supported image anywhere beneath it.
    """
    names = []
    for child in sorted(directory.iterdir()):
        if not child.is_dir():
            continue
        if _class_image_count(directory, child.name) > 0:
            names.append(child.name)
    return names


def _find_class_roots(base_dir):
    """Find directories whose immediate image-holding subfolders form a class set.

    Class names are not assumed — any six sibling folders that each contain images
    are treated as the class set. Split wrappers (train/validation) are skipped as
    class roots. Accepted layouts include:
        n1/ n2/ ... n6/
        dataset/n1/ ... n6/
        train/n1/ ... and validation/n1/ ...
        dataset/train/n1/ ... and dataset/validation/n1/ ...

    Returns candidates, each: {root, split, class_names, num_classes, total_images}.
    """
    base_dir = Path(base_dir)
    split_words = TRAIN_SPLIT_NAMES | VALIDATION_SPLIT_NAMES
    candidates = []
    for directory in [base_dir, *[p for p in base_dir.rglob("*") if p.is_dir()]]:
        class_names = _immediate_class_dirs(directory)
        if len(class_names) < 2:
            continue
        # A wrapper whose children are the split folders is not itself a class root.
        if {name.lower() for name in class_names} & split_words:
            continue
        total = sum(_class_image_count(directory, name) for name in class_names)
        candidates.append({
            "root": directory,
            "split": _detect_split_name(directory, base_dir),
            "class_names": class_names,
            "num_classes": len(class_names),
            "total_images": total,
        })
    # Prefer candidates with exactly NUM_CLASSES, then split-labeled, then more images.
    candidates.sort(
        key=lambda item: (
            item["num_classes"] == NUM_CLASSES,
            item["split"] is not None,
            item["total_images"],
        ),
        reverse=True,
    )
    return candidates


def _copy_class_images(class_root, target, class_names):
    dest = DATASET_DIR / target
    dest.mkdir(parents=True, exist_ok=True)
    counts = {}
    for class_name in class_names:
        src_class = class_root / class_name
        if not src_class.is_dir():
            continue
        dest_class = dest / class_name
        dest_class.mkdir(parents=True, exist_ok=True)
        n = 0
        for img in src_class.rglob("*"):
            if not img.is_file() or img.suffix.lower() not in IMAGE_EXTENSIONS:
                continue
            rel = img.relative_to(src_class)
            safe_parts = [re.sub(r"[^A-Za-z0-9._-]", "_", part) for part in rel.parts]
            dest_name = "__".join(safe_parts)
            shutil.copy2(img, dest_class / dest_name)
            n += 1
        counts[class_name] = n
    total = sum(counts.values())
    if total == 0:
        allowed = "/".join(sorted(IMAGE_EXTENSIONS))
        raise ValueError(f"No supported images ({allowed}) found inside the class folders.")
    return {"target": target, "total_images": total, "per_class": counts}


def _list_class_images(src_class):
    return sorted(
        (p for p in src_class.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS),
        key=lambda p: str(p).lower(),
    )


def _safe_dest_name(img, src_class):
    rel = img.relative_to(src_class)
    return "__".join(re.sub(r"[^A-Za-z0-9._-]", "_", part) for part in rel.parts)


def _copy_class_images_autosplit(class_root, class_names, val_ratio):
    """Copy six class folders into dataset/train + dataset/validation.

    Deterministic per-class split (~val_ratio to validation, evenly spaced, always
    leaving at least one training image). Used when the zip has no explicit split.
    """
    train_dest = DATASET_DIR / "train"
    val_dest = DATASET_DIR / "validation"
    train_counts = {}
    val_counts = {}
    for class_name in class_names:
        src_class = class_root / class_name
        train_counts[class_name] = 0
        val_counts[class_name] = 0
        if not src_class.is_dir():
            continue
        images = _list_class_images(src_class)
        n = len(images)
        val_target = 0 if n < 2 else max(1, min(n - 1, int(round(n * val_ratio))))
        step = (n / val_target) if val_target else 0
        val_idx = {min(n - 1, int(k * step)) for k in range(val_target)} if val_target else set()
        (train_dest / class_name).mkdir(parents=True, exist_ok=True)
        (val_dest / class_name).mkdir(parents=True, exist_ok=True)
        for i, img in enumerate(images):
            dest_root = val_dest if i in val_idx else train_dest
            shutil.copy2(img, dest_root / class_name / _safe_dest_name(img, src_class))
            if i in val_idx:
                val_counts[class_name] += 1
            else:
                train_counts[class_name] += 1
    if sum(train_counts.values()) + sum(val_counts.values()) == 0:
        allowed = "/".join(sorted(IMAGE_EXTENSIONS))
        raise ValueError(f"No supported images ({allowed}) found inside the class folders.")
    return [
        {"target": "train", "total_images": sum(train_counts.values()), "per_class": train_counts},
        {"target": "validation", "total_images": sum(val_counts.values()), "per_class": val_counts},
    ]


def _write_class_map(class_names):
    """Persist the imported class order, preserving prior action mappings if unchanged."""
    class_order = sorted(class_names)  # deterministic index assignment
    actions = {}
    if class_map_lib is not None:
        existing = class_map_lib.load_class_map(default_order=None, path=CLASS_MAP_PATH)
        if existing and set(existing.get("class_order", [])) == set(class_order):
            actions = {c["name"]: c["action"] for c in existing.get("classes", []) if c.get("action")}
        return class_map_lib.save_class_map(class_order, actions=actions, path=CLASS_MAP_PATH)
    # Fallback writer if the shared module is unavailable.
    payload = {
        "version": 1,
        "class_order": class_order,
        "num_classes": len(class_order),
        "classes": [{"index": i, "name": n, "action": None} for i, n in enumerate(class_order)],
    }
    CLASS_MAP_PATH.parent.mkdir(parents=True, exist_ok=True)
    CLASS_MAP_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload


def _import_dataset(zip_path):
    """Extract one uploaded dataset zip (no train/validation choice needed).

    Auto-detects the layout and requires exactly six class folders (arbitrary
    names):
      * If the zip already has train/ AND validation/ splits, both are imported
        as-is (they must share the same six class names).
      * Otherwise the six class folders are imported and AUTO-SPLIT into
        train/validation (VALIDATION_RATIO).

    Writes model_finetune/dataset/class_mapping.json with the discovered class
    order. Returns a summary dict.
    """
    staging = UPLOAD_DIR / f"stage-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
    staging.mkdir(parents=True, exist_ok=True)
    try:
        _extract_zip_safely(zip_path, staging)
        candidates = _find_class_roots(staging)
        six_class = [c for c in candidates if c["num_classes"] == NUM_CLASSES]
        if not six_class:
            found = candidates[0]["num_classes"] if candidates else 0
            example = candidates[0]["class_names"] if candidates else []
            raise ValueError(
                f"Expected exactly {NUM_CLASSES} class folders, found {found}"
                + (f" ({', '.join(example)})" if example else "")
                + ". Zip layout: six folders with any names, each holding images "
                "(optionally under train/ and validation/)."
            )

        best_by_split = {}
        for candidate in six_class:
            split = candidate["split"]
            if split and split not in best_by_split:
                best_by_split[split] = candidate

        if "train" in best_by_split and "validation" in best_by_split:
            name_sets = {
                tuple(sorted(best_by_split["train"]["class_names"])),
                tuple(sorted(best_by_split["validation"]["class_names"])),
            }
            if len(name_sets) > 1:
                raise ValueError("train and validation folders have different class names.")
            class_names = sorted(best_by_split["train"]["class_names"])
            imports = [
                _copy_class_images(best_by_split["train"]["root"], "train", class_names),
                _copy_class_images(best_by_split["validation"]["root"], "validation", class_names),
            ]
            mode = "provided-split"
        else:
            source = best_by_split.get("train") or six_class[0]
            class_names = sorted(source["class_names"])
            imports = _copy_class_images_autosplit(source["root"], class_names, VALIDATION_RATIO)
            mode = "auto-split"

        class_map_payload = _write_class_map(class_names)

        total = sum(item["total_images"] for item in imports)
        merged_counts = {class_name: 0 for class_name in class_names}
        for item in imports:
            for class_name, count in item["per_class"].items():
                merged_counts[class_name] = merged_counts.get(class_name, 0) + count
        return {
            "mode": mode,
            "target": "+".join(item["target"] for item in imports),
            "total_images": total,
            "class_order": class_names,
            "per_class": merged_counts,
            "class_map": class_map_payload,
            "imports": imports,
        }
    finally:
        shutil.rmtree(staging, ignore_errors=True)


# --------------------------------------------------------------------------- #
# Command builders (allowlisted)
# --------------------------------------------------------------------------- #
def build_training_command(recipe_key, params):
    recipe = TRAINING_RECIPES.get(recipe_key)
    if recipe is None:
        raise ValueError(f"Unknown training recipe: {recipe_key}")
    script = (REPO_ROOT / recipe["script"]).resolve()
    if not script.is_file():
        raise ValueError(f"Training script missing: {recipe['script']}")

    cmd = [sys.executable, str(script)]
    supports = recipe["supports"]
    if "epochs" in supports and params.get("epochs") not in (None, ""):
        cmd += ["--epochs", str(_safe_int(params["epochs"], "epochs", 1, 500))]
    if "batch_size" in supports and params.get("batch_size") not in (None, ""):
        cmd += ["--batch-size", str(_safe_int(params["batch_size"], "batch_size", 1, 512))]
    if "alpha" in supports and params.get("alpha") not in (None, ""):
        cmd += ["--alpha", str(_safe_float(params["alpha"], "alpha", 0.1, 2.0))]
    if "export_onnx" in supports and params.get("export_onnx"):
        cmd += ["--export-onnx"]
    return recipe, cmd


def build_quantize_command(params):
    script = (REPO_ROOT / QUANTIZE_SCRIPT).resolve()
    if not script.is_file():
        raise ValueError(f"Quantization script missing: {QUANTIZE_SCRIPT}")

    model_name = params.get("model_name")
    available = _available_keras_models()
    if not model_name:
        raise ValueError("model_name is required for quantization")
    model_path = _resolve_keras_model_id(model_name)
    if model_path is None:
        available_labels = [item["id"] for item in available]
        raise ValueError(
            f"model_name '{model_name}' not found under model_finetune/models/tf "
            "or model_finetune/models/pytorch. "
            f"Available: {', '.join(available_labels) if available_labels else '(none - train a model first)'}"
        )

    quant_format = params.get("quant_format", "int8")
    if quant_format not in QUANT_FORMATS:
        raise ValueError(f"quant_format must be one of {QUANT_FORMATS}")

    granularity = params.get("quant_granularity", "per-channel")
    if granularity not in QUANT_GRANULARITIES:
        raise ValueError(f"quant_granularity must be one of {QUANT_GRANULARITIES}")

    cmd = [
        sys.executable,
        str(script),
        "--keras", str(model_path),
        "--quant-format", quant_format,
        "--quant-granularity", granularity,
    ]
    if params.get("samples") not in (None, ""):
        cmd += ["--samples", str(_safe_int(params["samples"], "samples", 1, 5000))]
    if params.get("skip_source_validation"):
        cmd += ["--skip-source-validation"]
    return cmd


# --------------------------------------------------------------------------- #
# Flask app
# --------------------------------------------------------------------------- #
def create_app():
    app = Flask(__name__, template_folder=str(TEMPLATES_DIR))
    # 2 GiB cap on uploaded dataset zips.
    app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024 * 1024
    jobs = JobManager()

    @app.errorhandler(HTTPException)
    def _json_error(exc):
        # Return a clean JSON body ({"error": "..."}) instead of Flask's HTML page,
        # so the browser shows a readable message rather than raw HTML.
        return jsonify({"error": exc.description, "status": exc.code}), exc.code

    @app.errorhandler(Exception)
    def _json_unexpected(exc):  # pragma: no cover - safety net for 500s
        return jsonify({"error": "Internal server error.", "status": 500}), 500

    @app.get("/")
    def index():
        return send_from_directory(str(TEMPLATES_DIR), "index.html")

    @app.get("/api/health")
    def health():
        active = jobs.active_job()
        return jsonify({
            "status": "ok",
            "repo_root": str(REPO_ROOT),
            "class_names": _current_class_order(),
            "active_job": active.id if active else None,
            "time": _now_iso(),
        })

    @app.get("/api/flash-meta")
    def flash_meta():
        """Flash metadata for the browser Web Serial flasher.

        The browser flashes the selected .tflite into the ESP32-S3 model
        partition at this offset; the offset is deliberately not shown in the UI.
        """
        return jsonify({
            "chip": "esp32s3",
            "offset": hex(FLASH_OFFSET),
            "offset_int": FLASH_OFFSET,
        })

    @app.get("/api/class-map")
    def get_class_map():
        """Return the current class order + per-class output-action mapping."""
        payload = None
        if class_map_lib is not None:
            payload = class_map_lib.load_class_map(default_order=None, path=CLASS_MAP_PATH)
        if payload is None:
            payload = {
                "version": 1,
                "class_order": list(CLASS_NAMES),
                "num_classes": len(CLASS_NAMES),
                "classes": [{"index": i, "name": n, "action": None} for i, n in enumerate(CLASS_NAMES)],
                "default": True,  # no dataset imported yet
            }
        payload["output_actions"] = OUTPUT_ACTIONS
        return jsonify(payload)

    @app.post("/api/class-map")
    def set_class_map():
        """Save the student's class -> output-action mapping.

        Body: {"actions": {"<class name>": "<action>", ...}}. The class ORDER is
        fixed by the imported dataset and cannot be changed here; only actions are
        editable, and each must be one of OUTPUT_ACTIONS (or empty to clear).
        """
        if class_map_lib is None:
            abort(500, "class_map module unavailable on the server.")
        order = class_map_lib.load_class_order(default=None, path=CLASS_MAP_PATH)
        if not order:
            abort(400, "No dataset imported yet — upload six class folders first.")
        data = request.get_json(silent=True) or {}
        actions_in = data.get("actions", {})
        if not isinstance(actions_in, dict):
            abort(400, "'actions' must be an object of {class_name: action}.")
        actions = {}
        for name, action in actions_in.items():
            if name not in order:
                abort(400, f"Unknown class '{name}'.")
            if action in (None, ""):
                continue
            if action not in OUTPUT_ACTIONS:
                abort(400, f"action '{action}' must be one of {OUTPUT_ACTIONS}.")
            actions[name] = action
        payload = class_map_lib.save_class_map(order, actions=actions, path=CLASS_MAP_PATH)
        payload["output_actions"] = OUTPUT_ACTIONS
        return jsonify({"ok": True, **payload})

    @app.get("/api/recipes")
    def recipes():
        out = []
        for key, r in TRAINING_RECIPES.items():
            out.append({
                "key": key,
                "framework": r["framework"],
                "label": r["label"],
                "description": r["description"],
                "supports": r["supports"],
            })
        return jsonify({
            "recipes": out,
            "quant_formats": QUANT_FORMATS,
            "quant_granularities": QUANT_GRANULARITIES,
            "available_keras_models": _available_keras_models(),
        })

    @app.post("/api/dataset/upload")
    def dataset_upload():
        if "file" not in request.files:
            abort(400, "No file part named 'file'.")
        upload = request.files["file"]
        if not upload.filename:
            abort(400, "No file selected.")
        if not upload.filename.lower().endswith(".zip"):
            abort(400, "Dataset must be a .zip file.")
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
        safe_name = re.sub(r"[^A-Za-z0-9._-]", "_", upload.filename)
        saved_zip = UPLOAD_DIR / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}-{safe_name}"
        upload.save(str(saved_zip))
        try:
            summary = _import_dataset(saved_zip)
        except (ValueError, zipfile.BadZipFile) as exc:
            abort(400, str(exc))
        return jsonify({"ok": True, "zip": saved_zip.name, **summary})

    @app.post("/api/models/upload")
    def model_upload():
        if "file" not in request.files:
            abort(400, "No file part named 'file'.")
        upload = request.files["file"]
        framework = request.form.get("framework", "tf")
        try:
            saved_model = _save_uploaded_source_model(upload, framework)
        except ValueError as exc:
            abort(400, str(exc))
        return jsonify({
            "ok": True,
            "model": saved_model.name,
            "framework": framework,
            "available_keras_models": _available_keras_models(),
        })

    @app.post("/api/train")
    def train():
        data = request.get_json(silent=True) or request.form.to_dict()
        recipe_key = data.get("recipe")
        try:
            recipe, cmd = build_training_command(recipe_key, data)
        except ValueError as exc:
            abort(400, str(exc))
        try:
            job = jobs.start("train", f"train:{recipe_key}", cmd)
        except JobBusyError as exc:
            return jsonify({"error": "busy", "active_job": exc.active_job.to_dict()}), 409
        return jsonify(job.to_dict()), 202

    @app.post("/api/quantize")
    def quantize():
        data = request.get_json(silent=True) or request.form.to_dict()
        try:
            cmd = build_quantize_command(data)
        except ValueError as exc:
            abort(400, str(exc))
        try:
            job = jobs.start("quantize", f"quantize:{data.get('model_name')}", cmd)
        except JobBusyError as exc:
            return jsonify({"error": "busy", "active_job": exc.active_job.to_dict()}), 409
        return jsonify(job.to_dict()), 202

    @app.get("/api/jobs")
    def list_jobs():
        return jsonify({"jobs": [j.to_dict() for j in jobs.list()]})

    @app.get("/api/jobs/<job_id>")
    def job_detail(job_id):
        job = jobs.get(job_id)
        if job is None:
            abort(404, "Unknown job.")
        return jsonify(job.to_dict())

    @app.get("/api/jobs/<job_id>/log")
    def job_log(job_id):
        job = jobs.get(job_id)
        if job is None:
            abort(404, "Unknown job.")
        try:
            offset = max(0, int(request.args.get("offset", 0)))
        except ValueError:
            offset = 0
        data = ""
        size = 0
        if job.log_path.is_file():
            size = job.log_path.stat().st_size
            if offset < size:
                with open(job.log_path, "r", encoding="utf-8", errors="replace") as fh:
                    fh.seek(offset)
                    data = fh.read()
                    offset = fh.tell()
            else:
                offset = size
        return jsonify({
            "data": data,
            "offset": offset,
            "size": size,
            "status": job.status,
            "returncode": job.returncode,
        })

    @app.get("/api/artifacts")
    def artifacts():
        items = []
        categories = [
            ("keras_source", TF_MODELS_DIR, {".keras", ".h5"}),
            ("pytorch", PYTORCH_MODELS_DIR, {".pth", ".onnx", ".keras", ".h5"}),
            ("tflite_deploy", ARTIFACT_MODELS_DIR, {".tflite"}),
            ("quant_report", ARTIFACT_REPORTS_DIR, {".json"}),
        ]
        for category, root, exts in categories:
            if not root.is_dir():
                continue
            for path in sorted(root.rglob("*")):
                if path.is_file() and path.suffix.lower() in exts:
                    rel = path.resolve().relative_to(REPO_ROOT.resolve())
                    stat = path.stat()
                    items.append({
                        "category": category,
                        "name": path.name,
                        "path": str(rel),
                        "size": stat.st_size,
                        "modified": datetime.fromtimestamp(
                            stat.st_mtime, timezone.utc
                        ).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    })
        items.sort(key=lambda x: x["modified"], reverse=True)
        return jsonify({"artifacts": items})

    @app.get("/api/artifacts/download")
    def download_artifact():
        rel = request.args.get("path", "")
        if not rel:
            abort(400, "Missing 'path'.")
        candidate = (REPO_ROOT / rel).resolve() if not os.path.isabs(rel) else Path(rel)
        resolved = _resolve_within_roots(candidate)
        if resolved is None or not resolved.is_file():
            abort(404, "Artifact not found or outside allowed directories.")
        if resolved.suffix.lower() not in ARTIFACT_EXTENSIONS:
            abort(403, "File type not downloadable.")
        return send_file(str(resolved), as_attachment=True, download_name=resolved.name)

    return app


CERT_DIR = RUNS_DIR / "certs"


def ensure_self_signed_cert(cert_path, key_path, hosts):
    """Create a self-signed cert/key pair if missing. Returns (cert_path, key_path).

    For development / classroom use only — it enables HTTPS (a secure context) so
    the browser exposes the Web Serial API. Students will still see a "not private"
    warning because the certificate is self-signed; they continue past it once.
    """
    cert_path, key_path = Path(cert_path), Path(key_path)
    if cert_path.exists() and key_path.exists():
        return cert_path, key_path

    import datetime
    import ipaddress
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "EECampEdu Training Portal")])

    san = []
    seen = set()
    for host in hosts:
        if not host or host in seen:
            continue
        seen.add(host)
        try:
            san.append(x509.IPAddress(ipaddress.ip_address(host)))
        except ValueError:
            san.append(x509.DNSName(host))

    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=825))
        .add_extension(x509.SubjectAlternativeName(san), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    cert_path.parent.mkdir(parents=True, exist_ok=True)
    key_path.write_bytes(key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.TraditionalOpenSSL,
        encryption_algorithm=serialization.NoEncryption(),
    ))
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    return cert_path, key_path


def main():
    parser = argparse.ArgumentParser(description="EECampEdu AI PC training portal web server.")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host. Default: 0.0.0.0")
    parser.add_argument("--port", type=int, default=8080, help="Bind port. Default: 8080")
    parser.add_argument("--debug", action="store_true", help="Enable Flask debug mode.")
    parser.add_argument("--https", action="store_true",
                        help="Serve over HTTPS with a self-signed cert (needed for browser Web Serial flashing).")
    parser.add_argument("--cert", help="TLS certificate PEM path (default: runs/certs/portal-cert.pem, auto-generated).")
    parser.add_argument("--key", help="TLS private key PEM path (default: runs/certs/portal-key.pem, auto-generated).")
    parser.add_argument("--cert-host", action="append", default=[],
                        help="Extra hostname/IP to embed in the generated cert's SAN (repeatable, e.g. the gateway IP).")
    args = parser.parse_args()

    for directory in (RUNS_DIR, UPLOAD_DIR, JOBS_DIR):
        directory.mkdir(parents=True, exist_ok=True)

    ssl_context = None
    scheme = "http"
    if args.https:
        cert_path = Path(args.cert) if args.cert else CERT_DIR / "portal-cert.pem"
        key_path = Path(args.key) if args.key else CERT_DIR / "portal-key.pem"
        san_hosts = ["127.0.0.1", "localhost"]
        if args.host and args.host != "0.0.0.0":
            san_hosts.append(args.host)
        san_hosts.extend(args.cert_host)
        try:
            ensure_self_signed_cert(cert_path, key_path, san_hosts)
        except ImportError:
            print("[training_portal] ERROR: --https needs the 'cryptography' package "
                  "(pip install cryptography) or supply --cert/--key.")
            return 1
        except Exception as exc:  # noqa: BLE001
            print(f"[training_portal] ERROR: could not prepare TLS cert: {exc}")
            return 1
        ssl_context = (str(cert_path), str(key_path))
        scheme = "https"

    app = create_app()
    print(f"[training_portal] repo root : {REPO_ROOT}")
    print(f"[training_portal] runtime   : {RUNS_DIR}")
    if ssl_context:
        print(f"[training_portal] TLS cert  : {ssl_context[0]} (self-signed; browsers show a one-time warning)")
    print(f"[training_portal] listening : {scheme}://{args.host}:{args.port}")
    # threaded=True so log polling / uploads are served while a job thread runs.
    app.run(host=args.host, port=args.port, debug=args.debug, threaded=True, ssl_context=ssl_context)
    return 0


if __name__ == "__main__":
    sys.exit(main())
