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
    python apps/training_portal/server.py --host 0.0.0.0 --port 8000
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

# The gesture class order the training scripts expect (see model_finetune/README.md).
CLASS_NAMES = ["up", "ok", "thumb", "palm", "rock", "stone"]

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
    """Allowed .keras model IDs shown in the quantization dropdown.

    IDs are framework-prefixed (tf/<name>, pytorch/<name>) so TensorFlow and
    PyTorch exports can coexist even when their basenames are identical.
    """
    models = []
    for prefix, root in (("tf", TF_MODELS_DIR), ("pytorch", PYTORCH_MODELS_DIR)):
        if root.is_dir():
            for path in sorted(root.glob("*.keras")):
                models.append(f"{prefix}/{path.stem}")
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
        prefix, stem = model_id.split("/", 1)
        root = roots.get(prefix)
        if root is None or not re.fullmatch(r"[A-Za-z0-9_.-]+", stem or ""):
            return None
        candidate = (root / f"{stem}.keras").resolve()
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
        candidate = (root / f"{model_id}.keras").resolve()
        if candidate.is_file():
            matches.append(candidate)
    return matches[0] if len(matches) == 1 else None


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


def _find_class_roots(base_dir):
    """Find directories that directly contain gesture class subfolders.

    Accepted zip layouts include:
        up/ ok/ thumb/ ...
        dataset/up/ ok/ thumb/ ...
        train/up/ ... and validation/up/ ...
        dataset/train/up/ ... and dataset/validation/up/ ...

    Returns candidates sorted by confidence. Each candidate contains:
        root: directory with class folders
        split: "train", "validation", or None
        total_images: image count under known class folders
    """
    base_dir = Path(base_dir)
    candidates = []
    for directory in [base_dir, *[p for p in base_dir.rglob("*") if p.is_dir()]]:
        counts = {class_name: _class_image_count(directory, class_name) for class_name in CLASS_NAMES}
        present = [class_name for class_name, count in counts.items() if count > 0]
        if len(present) < 2:
            continue
        total = sum(counts.values())
        split = _detect_split_name(directory, base_dir)
        candidates.append({
            "root": directory,
            "split": split,
            "total_images": total,
            "present_classes": len(present),
        })
    candidates.sort(
        key=lambda item: (
            item["split"] is not None,
            item["present_classes"],
            item["total_images"],
            -len(item["root"].relative_to(base_dir).parts),
        ),
        reverse=True,
    )
    return candidates


def _copy_class_images(class_root, target):
    dest = DATASET_DIR / target
    dest.mkdir(parents=True, exist_ok=True)
    counts = {}
    for class_name in CLASS_NAMES:
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


def _import_dataset(zip_path, target):
    """Extract an uploaded dataset zip into model_finetune/dataset/<target>.

    If the zip contains explicit train/validation folders, both splits are
    imported in one upload. Otherwise, the discovered class root is imported
    into the UI-selected target folder. Existing per-class images are kept
    unless a same-named file is overwritten. Returns a summary dict.
    """
    if target not in ("train", "validation"):
        raise ValueError("target must be 'train' or 'validation'")

    staging = UPLOAD_DIR / f"stage-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
    staging.mkdir(parents=True, exist_ok=True)
    try:
        _extract_zip_safely(zip_path, staging)
        candidates = _find_class_roots(staging)
        if not candidates:
            raise ValueError(
                "Zip does not contain gesture class folders "
                f"({', '.join(CLASS_NAMES)}). Accepted layouts include "
                "<class>/*.jpg, dataset/<class>/*.jpg, train/<class>/*.jpg, "
                "or dataset/train/<class>/*.jpg."
            )

        best_by_split = {}
        for candidate in candidates:
            split = candidate["split"]
            if split and split not in best_by_split:
                best_by_split[split] = candidate

        imports = []
        if best_by_split:
            for split in ("train", "validation"):
                candidate = best_by_split.get(split)
                if candidate:
                    imports.append(_copy_class_images(candidate["root"], split))
        else:
            imports.append(_copy_class_images(candidates[0]["root"], target))

        total = sum(item["total_images"] for item in imports)
        merged_counts = {class_name: 0 for class_name in CLASS_NAMES}
        for item in imports:
            for class_name, count in item["per_class"].items():
                merged_counts[class_name] += count
        return {
            "target": "+".join(item["target"] for item in imports),
            "total_images": total,
            "per_class": merged_counts,
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
        raise ValueError(
            f"model_name '{model_name}' not found under model_finetune/models/tf "
            "or model_finetune/models/pytorch. "
            f"Available: {', '.join(available) if available else '(none - train a model first)'}"
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

    @app.get("/")
    def index():
        return send_from_directory(str(TEMPLATES_DIR), "index.html")

    @app.get("/api/health")
    def health():
        active = jobs.active_job()
        return jsonify({
            "status": "ok",
            "repo_root": str(REPO_ROOT),
            "class_names": CLASS_NAMES,
            "active_job": active.id if active else None,
            "time": _now_iso(),
        })

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
        target = request.form.get("target", "train")
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
        safe_name = re.sub(r"[^A-Za-z0-9._-]", "_", upload.filename)
        saved_zip = UPLOAD_DIR / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}-{safe_name}"
        upload.save(str(saved_zip))
        try:
            summary = _import_dataset(saved_zip, target)
        except (ValueError, zipfile.BadZipFile) as exc:
            abort(400, str(exc))
        return jsonify({"ok": True, "zip": saved_zip.name, **summary})

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
            ("pytorch", PYTORCH_MODELS_DIR, {".pth", ".onnx", ".keras"}),
            ("tflite_deploy", ARTIFACT_MODELS_DIR, {".tflite"}),
            ("quant_report", ARTIFACT_REPORTS_DIR, {".json"}),
        ]
        for category, root, exts in categories:
            if not root.is_dir():
                continue
            for path in sorted(root.glob("*")):
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


def main():
    parser = argparse.ArgumentParser(description="EECampEdu AI PC training portal web server.")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host. Default: 0.0.0.0")
    parser.add_argument("--port", type=int, default=8000, help="Bind port. Default: 8000")
    parser.add_argument("--debug", action="store_true", help="Enable Flask debug mode.")
    args = parser.parse_args()

    for directory in (RUNS_DIR, UPLOAD_DIR, JOBS_DIR):
        directory.mkdir(parents=True, exist_ok=True)

    app = create_app()
    print(f"[training_portal] repo root : {REPO_ROOT}")
    print(f"[training_portal] runtime   : {RUNS_DIR}")
    print(f"[training_portal] listening : http://{args.host}:{args.port}")
    # threaded=True so log polling / uploads are served while a job thread runs.
    app.run(host=args.host, port=args.port, debug=args.debug, threaded=True)


if __name__ == "__main__":
    main()
