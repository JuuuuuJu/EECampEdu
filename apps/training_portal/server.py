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
import base64
import binascii
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

from flask import (
    Flask,
    abort,
    jsonify,
    redirect,
    render_template,
    request,
    send_file,
    send_from_directory,
    session,
    url_for,
)
from werkzeug.exceptions import HTTPException
from werkzeug.security import check_password_hash, generate_password_hash

# --------------------------------------------------------------------------- #
# Paths
# --------------------------------------------------------------------------- #
APP_DIR = Path(__file__).resolve().parent
REPO_ROOT = APP_DIR.parents[1]              # apps/training_portal -> repo root
TEMPLATES_DIR = APP_DIR / "templates"
RUNS_DIR = APP_DIR / "runs"                 # git-ignored runtime folder
UPLOAD_DIR = RUNS_DIR / "uploads"
JOBS_DIR = RUNS_DIR / "jobs"
ML_RUNTIME_DIR = RUNS_DIR / "ml_runtime"
USER_DATASETS_DIR = RUNS_DIR / "user_datasets"
DATASET_EXPORTS_DIR = RUNS_DIR / "dataset_exports"
# AI PC Drive: one AI PC serves one team, so this is that team's shared storage
# (code / models / reports / build logs / general uploads — NOT the primary place
# for captured photos, which live on the ESP32-S3). Login is still required.
DRIVE_ROOT = RUNS_DIR / "drive"
DRIVE_FOLDERS = ["0_shared"] + [str(i) for i in range(1, 13)]  # 0_shared, 1 .. 12

MODEL_FINETUNE_DIR = REPO_ROOT / "model_finetune"
TF_MODELS_DIR = MODEL_FINETUNE_DIR / "models" / "tf"
PYTORCH_MODELS_DIR = MODEL_FINETUNE_DIR / "models" / "pytorch"
# ESP32-S3 main board firmware (ESP-IDF project). Firmware flashing reads the
# ESP-IDF-generated build metadata (flasher_args.json) rather than hardcoding offsets.
MAIN_BOARD_DIR = REPO_ROOT / "firmware" / "main_board"
FIRMWARE_BUILD_DIR = MAIN_BOARD_DIR / "build"
FIRMWARE_FLASHER_ARGS = FIRMWARE_BUILD_DIR / "flasher_args.json"
# Flashable firmware targets (class day -> ESP-IDF build dir). Each is flashed with
# its own ESP-IDF flasher_args.json so offsets are never hardcoded.
FIRMWARE_TARGETS = {
    "model_finetune": {
        "label": "Model finetune camera firmware",
        "build_dir": REPO_ROOT / "firmware" / "model_finetune" / "build",
        "build_hint": "firmware/model_finetune camera-only OV2640 + USB firmware",
    },
    "deploy_benchmark": {
        "label": "Deploy benchmark firmware",
        "build_dir": REPO_ROOT / "firmware" / "deploy_benchmark" / "build",
        "build_hint": "firmware/deploy_benchmark with RuntimeMode::kTestUartFrame",
    },
    "main_board": {
        "label": "Main board full integration firmware",
        "build_dir": REPO_ROOT / "firmware" / "main_board" / "build",
        "build_hint": "firmware/main_board with RuntimeMode::kCameraUsbMsc",
    },
    "control_board": {
        "label": "Control board output firmware",
        "build_dir": REPO_ROOT / "firmware" / "control_board" / "build",
        "build_hint": "firmware/control_board plain ESP32 servo-output firmware",
    },
    "camera_usb_demo": {
        "label": "Camera + USB demo firmware",
        "build_dir": REPO_ROOT / "firmware" / "camera_usb_demo" / "build",
        "build_hint": "firmware/camera_usb_demo",
    },
    "output_demo": {
        "label": "Output demo (GPIO/LED/PWM) firmware",
        "build_dir": REPO_ROOT / "firmware" / "teaching_output_demo" / "build",
        "build_hint": "firmware/teaching_output_demo",
    },
}
ARTIFACT_MODELS_DIR = REPO_ROOT / "firmware" / "pc" / "artifacts" / "models"
ARTIFACT_REPORTS_DIR = REPO_ROOT / "firmware" / "pc" / "artifacts" / "reports"

# Fallback gesture class order when no dataset has been imported yet. Students may
# upload their own six class folders with ARBITRARY names; the imported order is
# recorded in each browser session's dataset/class_mapping.json.
CLASS_NAMES = ["background", "up", "ok", "thumb", "palm", "rock"]

# Shared class-order / action-mapping helper (model_finetune/class_map.py).
sys.path.insert(0, str(MODEL_FINETUNE_DIR))
try:
    import class_map as class_map_lib
except Exception:  # pragma: no cover - portal still runs without it
    class_map_lib = None

NUM_CLASSES = 7
MODEL_PREDICT_CACHE = {}
MODEL_INPUT_SIZE = (96, 96)
MAX_CAPTURE_IMAGE_BYTES = 8 * 1024 * 1024
VALIDATION_RATIO = 0.2  # auto-split fraction when the zip has no validation split
# ESP32-S3 model-partition offset (see firmware/main_board/partitions.csv). Served via
# /api/flash-meta to the browser Web Serial flasher; never shown in the UI.
FLASH_OFFSET = 0x310000
# Robot-arm output actions a class may be mapped to (matches control board output firmware).
OUTPUT_ACTIONS = ["up", "down", "left", "right", "clamp", "release"]

# Output teaching demo: students edit only the code between these markers in
# firmware/teaching_output_demo/main/app_main.c, then build+flash from the portal.
OUTPUT_DEMO_DIR = REPO_ROOT / "firmware" / "teaching_output_demo"
OUTPUT_DEMO_SOURCE = OUTPUT_DEMO_DIR / "main" / "app_main.c"
OUTPUT_DEMO_DEFAULT_BLOCK = OUTPUT_DEMO_DIR / "main" / "teaching_block_default.txt"
TEACHING_BLOCK_START = "// >>> TEACHING_BLOCK_START <<<"
TEACHING_BLOCK_END = "// >>> TEACHING_BLOCK_END <<<"
TEACHING_BLOCK_MAX_CHARS = 20000
# ESP-IDF environment for allowlisted firmware builds (idf.py).
IDF_EXPORT_SH = os.environ.get("IDF_EXPORT_SH", "/opt/esp/idf/export.sh")


def _class_map_path(dataset_dir):
    return Path(dataset_dir) / "class_mapping.json"


def _current_class_order(dataset_dir):
    """Saved class order if a dataset was imported, else the fallback default."""
    if class_map_lib is not None:
        order = class_map_lib.load_class_order(
            default=None,
            path=_class_map_path(dataset_dir),
        )
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
        "supports": ["epochs", "batch_size", "alpha", "export_onnx", "augment_flip"],
        "description": "Transfer-learned MobileNetV2, best accuracy/speed on the ESP32-S3.",
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

# On-device benchmark (Deploy tab). Runs server-side as an allowlisted background
# job against an ESP32-S3 connected to THIS (AI PC) machine's serial port.
BENCHMARK_SCRIPT = "firmware/pc/benchmark/run_benchmark_png.py"
# Allowlisted benchmark dataset directories (only existing ones with images are offered).
BENCHMARK_SHARED_DATASET_DIRS = [
    ("test_tflite", REPO_ROOT / "firmware" / "pc" / "dataset" / "test" / "tflite"),
]
SERIAL_PORT_RE = re.compile(r"^(/dev/[A-Za-z0-9._\-]+|COM[0-9]+)$")

# Directories whose files may be listed as artifacts and downloaded. Any download
# request is resolved and confirmed to live inside one of these roots (no traversal).
ARTIFACT_ROOTS = [TF_MODELS_DIR, PYTORCH_MODELS_DIR, ARTIFACT_MODELS_DIR, ARTIFACT_REPORTS_DIR]
ARTIFACT_EXTENSIONS = {".keras", ".pth", ".onnx", ".tflite", ".json", ".h5"}
ML_JOB_KINDS = {"train", "quantize"}
ML_MAX_PARALLEL_JOBS = max(1, int(os.environ.get("EECAMP_MAX_PARALLEL_ML_JOBS", "6")))
TEMP_ARTIFACT_TTL_SECONDS = max(
    60,
    int(os.environ.get("EECAMP_TEMP_ARTIFACT_TTL_SECONDS", str(3 * 60 * 60))),
)
TEMP_ARTIFACT_SWEEP_SECONDS = max(
    60,
    int(os.environ.get("EECAMP_TEMP_ARTIFACT_SWEEP_SECONDS", "300")),
)
TEMP_ARTIFACT_DIRNAME = "temporary"
LOCAL_TIMEZONE = ZoneInfo(os.environ.get("EECAMP_PORTAL_TIMEZONE", "Asia/Taipei"))
# Classroom authentication. One AI PC usually serves one team, but the portal
# supports team01..team10 accounts so the same code can be reused across machines.
# Configure real passwords outside git:
#   export EECAMP_TEAM_PASSWORDS='{"team01":"...","team02":"..."}'
# Fallback passwords are intentionally classroom-simple and should be overridden.
def _load_team_passwords():
    raw = os.environ.get("EECAMP_TEAM_PASSWORDS", "").strip()
    if raw:
        try:
            data = json.loads(raw)
            return {str(k): str(v) for k, v in data.items() if re.match(r"^team\d{2}$", str(k))}
        except ValueError:
            pass
    prefix = os.environ.get("EECAMP_TEAM_PASSWORD_PREFIX", "eecamp")
    return {f"team{i:02d}": f"{prefix}{i:02d}" for i in range(1, 11)}

TEAM_PASSWORDS = _load_team_passwords()
TEAM_PASSWORD_STORE = RUNS_DIR / "team_passwords.json"

def _team_from_hostname():
    override = os.environ.get("EECAMP_PORTAL_TEAM", "").strip()
    if override in TEAM_PASSWORDS:
        return override
    try:
        hostname = os.uname().nodename.lower()
    except AttributeError:
        import socket
        hostname = socket.gethostname().lower()
    for suffix in reversed(re.findall(r"\d+", hostname)):
        team = f"team{int(suffix):02d}"
        if team in TEAM_PASSWORDS:
            return team
    if len(TEAM_PASSWORDS) == 1:
        return next(iter(TEAM_PASSWORDS))
    return None

def _load_runtime_password_hashes():
    try:
        data = json.loads(TEAM_PASSWORD_STORE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {str(k): str(v) for k, v in data.items() if str(k) in TEAM_PASSWORDS and isinstance(v, str)}


def _save_runtime_password_hash(team, password):
    if team not in TEAM_PASSWORDS:
        raise ValueError("Unknown team account.")
    TEAM_PASSWORD_STORE.parent.mkdir(parents=True, exist_ok=True)
    data = _load_runtime_password_hashes()
    data[team] = generate_password_hash(password)
    tmp = TEAM_PASSWORD_STORE.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(TEAM_PASSWORD_STORE)


def _team_password_matches(team, password):
    runtime_hash = _load_runtime_password_hashes().get(team)
    if runtime_hash:
        try:
            return check_password_hash(runtime_hash, password)
        except ValueError:
            return False
    return TEAM_PASSWORDS.get(team) == password


PUBLIC_PATH_PREFIXES = ("/login", "/api/health", "/static/", "/favicon.ico")
TOPIC_ROUTES = {
    "/": "home",
    "/home": "home",
    "/model_finetune": "model",
    "/deploy": "deploy",
    "/output": "output",
    "/firmware": "mainfw",
    "/camera_usb": "camerausb",
    "/drive": "drive",
    "/account": "account",
}


# --------------------------------------------------------------------------- #
# Background job manager
# --------------------------------------------------------------------------- #
class Job:
    """One background subprocess or queued subprocess."""

    def __init__(self, job_id, kind, label, cmd, log_path, meta_path, cwd=None, extra=None):
        self.id = job_id
        self.kind = kind
        self.label = label
        self.cmd = cmd
        self.log_path = log_path
        self.meta_path = meta_path
        self.cwd = Path(cwd) if cwd else REPO_ROOT
        self.extra = extra or {}
        self.owner_id = self.extra.get("owner_id")
        self.owner_team = self.extra.get("owner_team")
        self.status = "queued"  # queued | starting | running | succeeded | failed
        self.returncode = None
        self.created_at = _now_iso()
        self.started_at = None
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
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "owner_team": self.owner_team,
            "log": self.id,  # log fetched via /api/jobs/<id>/log
        }

    def persist(self):
        try:
            self.meta_path.write_text(json.dumps(self.to_dict(), indent=2))
        except OSError:
            pass


class JobManager:
    """Run train/quantize jobs through a bounded queue.

    Training and quantization are allowed to run concurrently up to
    ML_MAX_PARALLEL_JOBS. Other job kinds remain exclusive so firmware builds,
    camera demos, and similar local-device operations do not overlap with ML
    jobs or each other.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._active_ids = set()
        self._queued_ids = []
        self._jobs = {}  # id -> Job (this process lifetime)
        self._counter = 0

    def _new_id(self, kind):
        self._counter += 1
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        return f"{stamp}-{kind}-{self._counter:03d}"

    def active_job(self):
        with self._lock:
            for job_id in sorted(self._active_ids):
                job = self._jobs.get(job_id)
                if job is not None:
                    return job
            return None

    def _active_ml_count_locked(self):
        return sum(1 for job_id in self._active_ids
                   if self._jobs.get(job_id) and self._jobs[job_id].kind in ML_JOB_KINDS)

    def _has_active_non_ml_locked(self):
        return any(self._jobs.get(job_id) and self._jobs[job_id].kind not in ML_JOB_KINDS
                   for job_id in self._active_ids)

    def _first_live_job_locked(self):
        for job_id in list(self._active_ids) + list(self._queued_ids):
            job = self._jobs.get(job_id)
            if job is not None and job.status in ("queued", "starting", "running"):
                return job
        return None

    def _live_owner_job_locked(self, owner_id):
        if not owner_id:
            return None
        for job_id in list(self._active_ids) + list(self._queued_ids):
            job = self._jobs.get(job_id)
            if (
                job is not None
                and job.kind in ML_JOB_KINDS
                and job.owner_id == owner_id
                and job.status in ("queued", "starting", "running")
            ):
                return job
        return None

    def start(self, kind, label, cmd, env=None, cwd=None, extra=None):
        extra = extra or {}
        with self._lock:
            if kind not in ML_JOB_KINDS:
                active = self._first_live_job_locked()
                if active is not None:
                    raise JobBusyError(active)
            else:
                active = self._live_owner_job_locked(extra.get("owner_id"))
                if active is not None:
                    raise JobBusyError(active)

            job_id = self._new_id(kind)
            job_dir = JOBS_DIR / job_id
            job_dir.mkdir(parents=True, exist_ok=True)
            log_path = job_dir / "job.log"
            meta_path = job_dir / "meta.json"
            job = Job(job_id, kind, label, cmd, log_path, meta_path, cwd=cwd, extra=extra)
            self._jobs[job_id] = job
            if kind in ML_JOB_KINDS:
                self._queued_ids.append(job_id)
            else:
                self._spawn_locked(job, env)

        job.persist()
        if kind in ML_JOB_KINDS:
            with self._lock:
                self._drain_locked()
        return job

    def _spawn_locked(self, job, env=None):
        if job.id in self._active_ids:
            return
        job.status = "starting"
        job.started_at = _now_iso()
        self._active_ids.add(job.id)
        job.persist()
        thread = threading.Thread(target=self._run, args=(job, env), daemon=True)
        thread.start()

    def _drain_locked(self):
        if self._has_active_non_ml_locked():
            return
        while self._queued_ids and self._active_ml_count_locked() < ML_MAX_PARALLEL_JOBS:
            job_id = self._queued_ids.pop(0)
            job = self._jobs.get(job_id)
            if job is None or job.status != "queued":
                continue
            self._spawn_locked(job)

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
                    cwd=str(job.cwd),
                    stdout=log_fh,
                    stderr=subprocess.STDOUT,
                    env=run_env,
                    shell=False,
                )
                returncode = job.proc.wait()
            job.returncode = returncode
            job.status = "succeeded" if returncode == 0 else "failed"
            if job.status == "succeeded":
                _finalize_job_artifacts(job)
                if _job_has_artifacts(job):
                    _prune_owner_kind_artifacts(job)
                else:
                    _remove_job_artifacts(job)
            else:
                _remove_job_artifacts(job)
        except Exception as exc:  # noqa: BLE001 - record any launch failure in the log
            job.status = "failed"
            job.returncode = -1
            _remove_job_artifacts(job)
            try:
                with open(job.log_path, "a", encoding="utf-8") as log_fh:
                    log_fh.write(f"\n[portal] failed to run job: {exc}\n")
            except OSError:
                pass
        finally:
            job.finished_at = _now_iso()
            _cleanup_job_runtime(job)
            _prune_expired_temp_artifacts()
            job.persist()
            with self._lock:
                self._active_ids.discard(job.id)
                self._drain_locked()

    def get(self, job_id):
        return self._jobs.get(job_id)

    def list(self):
        return sorted(self._jobs.values(), key=lambda j: j.created_at, reverse=True)


class JobBusyError(Exception):
    def __init__(self, active_job):
        super().__init__("A job is already running.")
        self.active_job = active_job


def _artifact_owner_slug(owner_id):
    if not owner_id:
        raise ValueError("Temporary artifacts require an owner.")
    return hashlib.sha256(str(owner_id).encode("utf-8")).hexdigest()[:16]


USER_DATASET_LOCKS = {}
USER_DATASET_LOCKS_GUARD = threading.Lock()


def _user_dataset_root(owner_id):
    return USER_DATASETS_DIR / _artifact_owner_slug(owner_id)


def _user_dataset_dir(owner_id):
    return _user_dataset_root(owner_id) / "dataset"


def _user_upload_dir(owner_id):
    return _user_dataset_root(owner_id) / "uploads"


def _user_dataset_lock(owner_id):
    slug = _artifact_owner_slug(owner_id)
    with USER_DATASET_LOCKS_GUARD:
        return USER_DATASET_LOCKS.setdefault(slug, threading.RLock())


def _artifact_token(job):
    token = str(job.extra.get("artifact_token", ""))
    if not re.fullmatch(r"[a-f0-9]{32}", token):
        raise ValueError("Temporary artifacts require a valid result ID.")
    return token


def _temporary_artifact_dir(root, owner_id, kind, token, create=False):
    if kind not in ML_JOB_KINDS:
        raise ValueError(f"Unsupported temporary artifact kind: {kind}")
    if not re.fullmatch(r"[a-f0-9]{32}", str(token)):
        raise ValueError("Invalid temporary artifact result ID.")
    target = (
        root
        / "runs"
        / TEMP_ARTIFACT_DIRNAME
        / _artifact_owner_slug(owner_id)
        / kind
        / str(token)
    )
    if create:
        target.mkdir(parents=True, exist_ok=True)
    return target


def _artifact_run_dir(root, job):
    return _temporary_artifact_dir(
        root,
        job.owner_id,
        job.kind,
        _artifact_token(job),
        create=True,
    )


def _remove_empty_temp_parents(path, stop):
    current = path
    while current != stop:
        try:
            current.rmdir()
        except OSError:
            break
        current = current.parent


def _remove_temporary_result(owner_id, kind, token):
    if kind not in ML_JOB_KINDS or not owner_id:
        return
    for root in ARTIFACT_ROOTS:
        target = _temporary_artifact_dir(root, owner_id, kind, token)
        shutil.rmtree(target, ignore_errors=True)
        _remove_empty_temp_parents(
            target.parent,
            root / "runs" / TEMP_ARTIFACT_DIRNAME,
        )


def _remove_job_artifacts(job):
    if job.kind not in ML_JOB_KINDS or not job.owner_id:
        return
    try:
        token = _artifact_token(job)
    except ValueError:
        return
    _remove_temporary_result(job.owner_id, job.kind, token)


def _job_has_artifacts(job):
    try:
        token = _artifact_token(job)
    except ValueError:
        return False
    for root in ARTIFACT_ROOTS:
        result_dir = _temporary_artifact_dir(root, job.owner_id, job.kind, token)
        if result_dir.is_dir() and any(path.is_file() for path in result_dir.rglob("*")):
            return True
    return False


def _prune_owner_kind_artifacts(job):
    """Keep only this successful result for its browser session and job kind."""
    if job.kind == "train":
        return
    token = _artifact_token(job)
    for root in ARTIFACT_ROOTS:
        current = _temporary_artifact_dir(root, job.owner_id, job.kind, token)
        kind_dir = current.parent
        if not kind_dir.is_dir():
            continue
        for candidate in kind_dir.iterdir():
            if candidate.name != token and candidate.is_dir() and not candidate.is_symlink():
                shutil.rmtree(candidate, ignore_errors=True)


def _prune_expired_temp_artifacts():
    cutoff = time.time() - TEMP_ARTIFACT_TTL_SECONDS
    for root in ARTIFACT_ROOTS:
        temp_root = root / "runs" / TEMP_ARTIFACT_DIRNAME
        if not temp_root.is_dir():
            continue
        for result_dir in temp_root.glob("*/*/*"):
            if not result_dir.is_dir() or result_dir.is_symlink():
                continue
            try:
                mtimes = [result_dir.stat().st_mtime]
                mtimes.extend(
                    path.stat().st_mtime
                    for path in result_dir.rglob("*")
                    if path.is_file()
                )
            except OSError:
                continue
            if max(mtimes) < cutoff:
                shutil.rmtree(result_dir, ignore_errors=True)
                _remove_empty_temp_parents(result_dir.parent, temp_root)


def _artifact_cleanup_loop():
    while True:
        time.sleep(TEMP_ARTIFACT_SWEEP_SECONDS)
        try:
            _prune_expired_temp_artifacts()
        except Exception:
            pass


def _copy_recent_artifacts(source_root, dest_root, cutoff_mtime, extensions, rename_stem=None):
    copied = []
    if not source_root.is_dir():
        return copied
    dest_root.mkdir(parents=True, exist_ok=True)
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in extensions:
            continue
        try:
            if path.stat().st_mtime < cutoff_mtime - 1:
                continue
        except OSError:
            continue
        rel = path.relative_to(source_root)
        if rename_stem:
            target = dest_root / rel.parent / f"{rename_stem}{path.suffix}"
        else:
            target = dest_root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        copied.append(target)
    return copied


def _finalize_job_artifacts(job):
    if job.kind != "train":
        return
    runtime_root = job.extra.get("runtime_root")
    if not runtime_root:
        return
    runtime_root = Path(runtime_root)
    cutoff = float(job.extra.get("artifact_cutoff_mtime", 0))
    model_name = job.extra.get("model_name")
    copied = []
    copied += _copy_recent_artifacts(
        runtime_root / "model_finetune" / "models" / "tf",
        _artifact_run_dir(TF_MODELS_DIR, job),
        cutoff,
        {".keras", ".h5", ".onnx"},
        rename_stem=model_name,
    )
    copied += _copy_recent_artifacts(
        runtime_root / "model_finetune" / "models" / "pytorch",
        _artifact_run_dir(PYTORCH_MODELS_DIR, job),
        cutoff,
        {".keras", ".h5", ".pth", ".onnx"},
        rename_stem=model_name,
    )
    if copied:
        try:
            with open(job.log_path, "a", encoding="utf-8") as log_fh:
                log_fh.write("\n[portal] exported artifacts:\n")
                for path in copied:
                    log_fh.write(f"[portal]   {path.resolve().relative_to(REPO_ROOT.resolve())}\n")
        except OSError:
            pass


def _cleanup_job_runtime(job):
    runtime_root = job.extra.get("runtime_root") if getattr(job, "extra", None) else None
    if not runtime_root:
        return
    try:
        shutil.rmtree(runtime_root, ignore_errors=True)
    except OSError:
        pass


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
def _now_iso():
    return datetime.now(LOCAL_TIMEZONE).isoformat(timespec="seconds")


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
            
            parts = rel.parts
            if len(parts) >= 3 and parts[0] == "runs" and parts[1] == "temporary":
                mtime_dt = datetime.fromtimestamp(stat.st_mtime, LOCAL_TIMEZONE)
                time_str = mtime_dt.strftime("%m/%d %H:%M")
                label = f"{prefix}/{path.name} (Temp - {time_str}) ({stat.st_size / 1024:.1f} KB)"
            else:
                label = f"{model_id} ({stat.st_size / 1024:.1f} KB)"

            models.append({
                "id": model_id,
                "label": label,
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


# ---------------- AI PC Drive (per-team file storage on this AI PC) ----------------
def _ensure_drive():
    for folder in DRIVE_FOLDERS:
        (DRIVE_ROOT / folder).mkdir(parents=True, exist_ok=True)


def _drive_folder_dir(folder):
    if folder not in DRIVE_FOLDERS:
        raise ValueError(f"Unknown drive folder '{folder}'.")
    target = DRIVE_ROOT / folder
    target.mkdir(parents=True, exist_ok=True)
    return target


def _drive_safe_file(folder, name):
    """Resolve <folder>/<name> to a real file directly inside the folder (no traversal)."""
    folder_dir = _drive_folder_dir(folder)
    base = os.path.basename(str(name or "").strip())
    if not base or base in {".", ".."}:
        raise ValueError("Invalid file name.")
    candidate = (folder_dir / base).resolve()
    if candidate.parent != folder_dir.resolve():
        raise ValueError("Invalid file path.")
    return candidate


def _drive_list(folder):
    folder_dir = _drive_folder_dir(folder)
    files = []
    for path in sorted(folder_dir.iterdir(), key=lambda p: p.name.lower()):
        if path.is_file():
            stat = path.stat()
            files.append({
                "name": path.name,
                "size": stat.st_size,
                "modified": datetime.fromtimestamp(stat.st_mtime).strftime("%Y-%m-%d %H:%M"),
                "is_image": path.suffix.lower() in IMAGE_EXTENSIONS,
            })
    return files


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
    # Prefer candidates with MORE classes (the dataset may hold more than the
    # active-6 limit), then split-labeled, then more images.
    candidates.sort(
        key=lambda item: (
            item["num_classes"],
            item["split"] is not None,
            item["total_images"],
        ),
        reverse=True,
    )
    return candidates


def _copy_class_images(class_root, target, class_names, dataset_dir):
    dest = Path(dataset_dir) / target
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
            dest_name = f"{Path('__'.join(safe_parts)).stem}.png"
            _save_processed_model_image(img, dest_class / dest_name)
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


def _processed_dest_name(img, src_class):
    stem = Path(_safe_dest_name(img, src_class)).stem
    return f"{stem}.png"


def _crop_to_center_square(img):
    width, height = img.size
    if width == height:
        return img
    min_dim = min(width, height)
    left = (width - min_dim) // 2
    top = (height - min_dim) // 2
    right = left + min_dim
    bottom = top + min_dim
    return img.crop((left, top, right, bottom))


def _save_processed_model_image(src_path, dest_path, image_size=MODEL_INPUT_SIZE):
    """Store training images in the same grayscale 96x96 format used by inference."""
    try:
        from PIL import Image
    except ImportError as exc:
        raise ValueError(f"Pillow missing on the AI PC environment: {exc}")
    try:
        with Image.open(src_path) as img:
            img = _crop_to_center_square(img).convert("L").resize(image_size)
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            img.save(dest_path, format="PNG")
    except Exception as exc:
        raise ValueError(f"Failed to preprocess image {src_path.name}: {exc}") from exc


def _save_processed_model_image_bytes(raw, dest_path, image_size=MODEL_INPUT_SIZE):
    try:
        from PIL import Image
    except ImportError as exc:
        raise ValueError(f"Pillow missing on the AI PC environment: {exc}")
    try:
        with Image.open(io.BytesIO(raw)) as img:
            img = _crop_to_center_square(img).convert("L").resize(image_size)
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            img.save(dest_path, format="PNG")
    except Exception as exc:
        raise ValueError(f"Failed to preprocess captured frame: {exc}") from exc


def _copy_class_images_autosplit(class_root, class_names, val_ratio, dataset_dir):
    """Copy six class folders into dataset/train + dataset/validation.

    Deterministic per-class split (~val_ratio to validation, evenly spaced, always
    leaving at least one training image). Used when the zip has no explicit split.
    """
    train_dest = Path(dataset_dir) / "train"
    val_dest = Path(dataset_dir) / "validation"
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
            _save_processed_model_image(img, dest_root / class_name / _processed_dest_name(img, src_class))
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


def _write_class_map(class_names, dataset_dir):
    """Persist the ACTIVE class order (<= NUM_CLASSES), preserving any prior
    per-class action for classes that remain active."""
    class_order = sorted(class_names)  # deterministic index assignment
    class_map_path = _class_map_path(dataset_dir)
    actions = {}
    if class_map_lib is not None:
        existing = class_map_lib.load_class_map(default_order=None, path=class_map_path)
        if existing:
            # Carry over actions by NAME for any class still in the active set.
            actions = {
                c["name"]: c["action"]
                for c in existing.get("classes", [])
                if c.get("action") and c.get("name") in class_order
            }
        return class_map_lib.save_class_map(class_order, actions=actions, path=class_map_path)
    # Fallback writer if the shared module is unavailable.
    payload = {
        "version": 1,
        "class_order": class_order,
        "num_classes": len(class_order),
        "classes": [{"index": i, "name": n, "action": None} for i, n in enumerate(class_order)],
    }
    class_map_path.parent.mkdir(parents=True, exist_ok=True)
    class_map_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload


def _import_dataset(zip_path, dataset_dir, upload_dir):
    """Extract one uploaded dataset zip (no train/validation choice needed).

    Auto-detects the layout and requires exactly six class folders (arbitrary
    names):
      * If the zip already has train/ AND validation/ splits, both are imported
        as-is (they must share the same six class names).
      * Otherwise the six class folders are imported and AUTO-SPLIT into
        train/validation (VALIDATION_RATIO).

    Replaces only the owning browser session's active dataset after processing
    succeeds. Returns a summary dict.
    """
    dataset_dir = Path(dataset_dir)
    upload_dir = Path(upload_dir)
    token = uuid.uuid4().hex
    staging = upload_dir / f"stage-{token}"
    imported_dataset = dataset_dir.parent / f".dataset-import-{token}"
    previous_dataset = dataset_dir.parent / f".dataset-previous-{token}"
    staging.mkdir(parents=True, exist_ok=True)
    try:
        _extract_zip_safely(zip_path, staging)
        candidates = _find_class_roots(staging)
        if not candidates:
            raise ValueError(
                "No gesture class folders with images found. Zip layout: one folder "
                "per class (any names), each holding images — optionally grouped under "
                "train/ and validation/. At least two classes are required."
            )

        best_by_split = {}
        for candidate in candidates:
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
                _copy_class_images(
                    best_by_split["train"]["root"],
                    "train",
                    class_names,
                    imported_dataset,
                ),
                _copy_class_images(
                    best_by_split["validation"]["root"],
                    "validation",
                    class_names,
                    imported_dataset,
                ),
            ]
            mode = "provided-split"
        else:
            source = best_by_split.get("train") or candidates[0]
            class_names = sorted(source["class_names"])
            imports = _copy_class_images_autosplit(
                source["root"],
                class_names,
                VALIDATION_RATIO,
                imported_dataset,
            )
            mode = "auto-split"

        # The dataset may hold more than NUM_CLASSES classes; each training/
        # inference run uses an ACTIVE subset of at most NUM_CLASSES. Default the
        # active set to the first NUM_CLASSES classes alphabetically; the student
        # can change it later on the Model finetune page.
        active_names = class_names[:NUM_CLASSES]
        class_map_payload = _write_class_map(active_names, imported_dataset)

        total = sum(item["total_images"] for item in imports)
        merged_counts = {class_name: 0 for class_name in class_names}
        for item in imports:
            for class_name, count in item["per_class"].items():
                merged_counts[class_name] = merged_counts.get(class_name, 0) + count
        summary = {
            "mode": mode,
            "target": "+".join(item["target"] for item in imports),
            "total_images": total,
            "class_order": active_names,
            "available_classes": class_names,
            "active_classes": active_names,
            "per_class": merged_counts,
            "class_map": class_map_payload,
            "imports": imports,
        }
        if dataset_dir.exists():
            dataset_dir.replace(previous_dataset)
        try:
            imported_dataset.replace(dataset_dir)
        except Exception:
            if previous_dataset.exists() and not dataset_dir.exists():
                previous_dataset.replace(dataset_dir)
            raise
        shutil.rmtree(previous_dataset, ignore_errors=True)
        return summary
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        shutil.rmtree(imported_dataset, ignore_errors=True)
        if previous_dataset.exists() and not dataset_dir.exists():
            previous_dataset.replace(dataset_dir)
        else:
            shutil.rmtree(previous_dataset, ignore_errors=True)




def _sanitize_class_name(name):
    name = str(name or "").strip()
    name = re.sub(r"[\\/:*?\"<>|\x00-\x1f]", "_", name)
    name = re.sub(r"\s+", "_", name).strip("._ ")
    if not name:
        raise ValueError("Class name is required.")
    if name in {".", ".."}:
        raise ValueError("Class name is invalid.")
    return name[:64]


def _decode_image_payload(image_data):
    text = str(image_data or "")
    if not text:
        raise ValueError("Missing image_data.")
    if "," in text and text.lower().startswith("data:"):
        header, text = text.split(",", 1)
        mime = header.split(";", 1)[0].replace("data:", "") or "image/jpeg"
    else:
        mime = "image/jpeg"
    try:
        raw = base64.b64decode(text, validate=False)
    except (binascii.Error, ValueError) as exc:
        raise ValueError(f"Invalid base64 image data: {exc}")
    if not raw:
        raise ValueError("Image is empty.")
    if len(raw) > MAX_CAPTURE_IMAGE_BYTES:
        raise ValueError("Image is too large for classroom capture.")
    ext = ".jpg" if mime in {"image/jpeg", "image/jpg"} else ".png"
    return raw, mime, ext


def _dataset_class_names(dataset_dir):
    dataset_dir = Path(dataset_dir)
    names = set()
    for split in ("train", "validation"):
        root = dataset_dir / split
        if root.is_dir():
            for child in root.iterdir():
                if child.is_dir():
                    names.add(child.name)
    # Do not use _current_class_order() here: it falls back to the default class
    # names when no dataset exists, which would block students from creating
    # their own six gesture names from scratch.
    class_map_path = _class_map_path(dataset_dir)
    if class_map_lib is not None and class_map_path.is_file():
        saved = class_map_lib.load_class_order(default=None, path=class_map_path)
        if saved:
            names.update(saved)
    return sorted(names)


def _dataset_counts(dataset_dir):
    dataset_dir = Path(dataset_dir)
    names = _dataset_class_names(dataset_dir)
    return {
        name: {
            "train": _class_image_count(dataset_dir / "train", name),
            "validation": _class_image_count(dataset_dir / "validation", name),
        }
        for name in names
    }


def _refresh_class_map_from_dataset(dataset_dir, extra_class=None):
    """Keep the ACTIVE class set consistent after a capture.

    The dataset may hold many classes; the active set is capped at NUM_CLASSES.
    A newly captured class is auto-activated only if there is a free active slot
    (< NUM_CLASSES); otherwise it is simply collected on disk and the student can
    activate it later by swapping the active set. Never raises on extra classes.
    """
    available = _dataset_class_names(dataset_dir)
    if not available:
        return None
    order = []
    class_map_path = _class_map_path(dataset_dir)
    if class_map_lib is not None:
        order = class_map_lib.load_class_order(default=None, path=class_map_path) or []
    # Drop any active class whose images are gone; keep the rest active.
    order = [c for c in order if c in available]
    if extra_class and extra_class in available and extra_class not in order and len(order) < NUM_CLASSES:
        order.append(extra_class)
    if not order:
        # Bootstrap: first NUM_CLASSES classes alphabetically.
        order = sorted(available)[:NUM_CLASSES]
    if not order:
        return None
    return _write_class_map(order, dataset_dir)


def _dataset_file_count(dataset_dir):
    dataset_dir = Path(dataset_dir)
    if not dataset_dir.is_dir():
        return 0
    return sum(
        1
        for path in dataset_dir.rglob("*")
        if path.is_file() and not path.is_symlink()
    )


def _write_dataset_zip(dataset_dir, zip_path, mode="w"):
    dataset_dir = Path(dataset_dir)
    if _dataset_file_count(dataset_dir) == 0:
        raise ValueError("The active dataset is empty.")
    with zipfile.ZipFile(zip_path, mode=mode, compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(dataset_dir.rglob("*")):
            if path.is_file() and not path.is_symlink():
                archive.write(path, Path("dataset") / path.relative_to(dataset_dir))


def _delete_export_later(path, delay=900):
    def cleanup():
        try:
            Path(path).unlink()
        except FileNotFoundError:
            pass

    timer = threading.Timer(delay, cleanup)
    timer.daemon = True
    timer.start()


def _preprocess_image_bytes_for_keras(raw, image_size=MODEL_INPUT_SIZE):
    try:
        from PIL import Image
        import numpy as np
    except ImportError as exc:
        raise RuntimeError(f"Pillow/numpy missing on the AI PC environment: {exc}")
    with Image.open(io.BytesIO(raw)) as img:
        img = _crop_to_center_square(img).convert("L").resize(image_size)
        arr = np.asarray(img, dtype="float32") / 255.0
    return arr[None, ..., None]


def _load_predict_model(model_id, class_order):
    model_path = _resolve_keras_model_id(model_id)
    if model_path is None or not model_path.is_file():
        raise ValueError("Choose a valid .keras model first.")
    stat = model_path.stat()
    num_classes = len(class_order or CLASS_NAMES)
    key = (str(model_path.resolve()), stat.st_mtime_ns, num_classes)
    cached = MODEL_PREDICT_CACHE.get(key)
    if cached is not None:
        return cached
    MODEL_PREDICT_CACHE.clear()
    try:
        import numpy as np
        import tensorflow as tf
    except ImportError as exc:
        raise RuntimeError(f"TensorFlow/numpy missing on the AI PC environment: {exc}")
    try:
        model = tf.keras.models.load_model(model_path, compile=False)
    except Exception:
        tool_dir = REPO_ROOT / "firmware" / "pc" / "tools"
        sys.path.insert(0, str(tool_dir))
        try:
            import quantize_keras_model as qkm
            model = qkm.load_keras_model_compat(tf, model_path, MODEL_INPUT_SIZE, num_classes)
        finally:
            try:
                sys.path.remove(str(tool_dir))
            except ValueError:
                pass
    cached = (model, np)
    MODEL_PREDICT_CACHE[key] = cached
    return cached

# --------------------------------------------------------------------------- #
# Command builders (allowlisted)
# --------------------------------------------------------------------------- #
def build_training_command(recipe_key, params, dataset_dir):
    recipe = TRAINING_RECIPES.get(recipe_key)
    if recipe is None:
        raise ValueError(f"Unknown training recipe: {recipe_key}")
    dataset_dir = Path(dataset_dir)
    if len(_dataset_class_names(dataset_dir)) < 2:
        raise ValueError("Import or capture at least two dataset classes before training.")

    model_name = str(params.get("model_name") or "").strip()
    model_name = re.sub(r"[^A-Za-z0-9_-]", "_", model_name)
    if not model_name:
        framework = recipe.get("framework", "tf")
        target_dir = PYTORCH_MODELS_DIR if framework == "pytorch" else TF_MODELS_DIR
        max_num = 0
        if target_dir.is_dir():
            pattern = re.compile(r"^model_(\d+)\.(keras|h5)$", re.IGNORECASE)
            for p in list(target_dir.rglob("*.keras")) + list(target_dir.rglob("*.h5")):
                if p.is_file():
                    match = pattern.match(p.name)
                    if match:
                        num = int(match.group(1))
                        if num > max_num:
                            max_num = num
        model_name = f"model_{max_num + 1}"

    runtime_root = ML_RUNTIME_DIR / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}-{recipe_key}-{time.time_ns()}"
    runtime_root.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        MODEL_FINETUNE_DIR,
        runtime_root / "model_finetune",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "dataset"),
    )
    shutil.copytree(dataset_dir, runtime_root / "model_finetune" / "dataset")
    script = (runtime_root / recipe["script"]).resolve()
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
    if "export_onnx" in supports:
        cmd += ["--export-onnx"]
    if "augment_flip" in supports and params.get("augment_flip"):
        cmd += ["--augment-flip"]
    extra = {
        "runtime_root": str(runtime_root),
        "artifact_cutoff_mtime": time.time(),
        "model_name": model_name,
    }
    return recipe, cmd, runtime_root, extra



def build_quantize_command(params, owner_extra):
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
    result_dir = _temporary_artifact_dir(
        ARTIFACT_MODELS_DIR,
        owner_extra.get("owner_id"),
        "quantize",
        owner_extra.get("artifact_token"),
        create=True,
    )
    report_dir = _temporary_artifact_dir(
        ARTIFACT_REPORTS_DIR,
        owner_extra.get("owner_id"),
        "quantize",
        owner_extra.get("artifact_token"),
        create=True,
    )
    model_slug = re.sub(r"[^A-Za-z0-9_.-]", "_", model_path.stem).strip("._-") or "model"
    quant_slug = re.sub(r"[^A-Za-z0-9_.-]", "_", f"{quant_format}_{granularity}")
    output_path = result_dir / f"{model_slug}_{quant_slug}.tflite"
    report_path = report_dir / f"{model_slug}_{quant_slug}_quantization_report.json"
    cmd += ["--output", str(output_path), "--report", str(report_path)]
    if params.get("samples") not in (None, ""):
        cmd += ["--samples", str(_safe_int(params["samples"], "samples", 1, 5000))]
    if params.get("skip_source_validation"):
        cmd += ["--skip-source-validation"]
    return cmd


def _list_serial_ports():
    """Serial ports on THIS (AI PC) machine, or None if pyserial is unavailable."""
    try:
        from serial.tools import list_ports
    except Exception:  # noqa: BLE001
        return None
    return [
        {"device": p.device, "description": p.description or "", "hwid": p.hwid or ""}
        for p in list_ports.comports()
    ]


def _benchmark_dataset_entries(dataset_dir):
    dataset_dir = Path(dataset_dir)
    return [
        *BENCHMARK_SHARED_DATASET_DIRS,
        ("dataset_test", dataset_dir / "test"),
        ("dataset_validation", dataset_dir / "validation"),
        ("dataset_train", dataset_dir / "train"),
    ]


def _benchmark_datasets(dataset_dir):
    """Allowlisted benchmark dataset dirs that actually exist and contain images."""
    out = []
    for key, d in _benchmark_dataset_entries(dataset_dir):
        if d.is_dir():
            count = sum(1 for p in d.rglob("*")
                        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS)
            if count > 0:
                out.append({"id": key, "count": count})
    return out


def _benchmark_dataset_dir(ds_id, dataset_dir):
    """Resolve an allowlisted benchmark dataset id to its directory, or None."""
    for key, d in _benchmark_dataset_entries(dataset_dir):
        if key == ds_id and d.is_dir():
            return d.resolve()
    return None


def _benchmark_images(ds_id, dataset_dir, limit=200):
    """List up to `limit` images under a benchmark dataset, round-robin across class
    folders so every class is represented. Each item: {name (rel), label, url}.

    Images are served to the BROWSER (which flashes/benchmarks the board on the
    student PC), so nothing here touches a serial port on the AI PC.
    """
    root = _benchmark_dataset_dir(ds_id, dataset_dir)
    if root is None:
        return []
    by_class = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS:
            label = p.parent.name
            by_class.setdefault(label, []).append(p)
    # round-robin interleave classes up to `limit`
    ordered, idx = [], 0
    classes = sorted(by_class)
    while len(ordered) < limit and classes:
        progressed = False
        for label in classes:
            files = by_class.get(label) or []
            if idx < len(files):
                ordered.append((label, files[idx]))
                progressed = True
                if len(ordered) >= limit:
                    break
        if not progressed:
            break
        idx += 1
    out = []
    for label, p in ordered:
        rel = str(p.resolve().relative_to(root))
        out.append({
            "label": label,
            "name": rel,
            "url": f"/api/benchmark/image?dataset={ds_id}&name={rel}",
        })
    return out


# --------------------------------------------------------------------------- #
# Output teaching demo: edit-block -> build -> flash
# --------------------------------------------------------------------------- #
def _read_output_source():
    """Return the full app_main.c text, or raise a friendly error."""
    if not OUTPUT_DEMO_SOURCE.is_file():
        raise ValueError("Output demo source file is missing on the AI PC.")
    return OUTPUT_DEMO_SOURCE.read_text(encoding="utf-8")


def _split_teaching_block(text):
    """Return (prefix, block, suffix) split on the teaching-block markers.

    `block` is the text strictly between the marker lines (students edit this);
    prefix/suffix are the fixed surrounding code (kept verbatim). Raises if the
    markers are absent or malformed.
    """
    start = text.find(TEACHING_BLOCK_START)
    if start < 0:
        raise ValueError("Teaching-block start marker not found in firmware source.")
    # include the marker line itself + its trailing newline in the prefix
    start_eol = text.find("\n", start)
    if start_eol < 0:
        raise ValueError("Malformed teaching-block start marker.")
    end = text.find(TEACHING_BLOCK_END, start_eol)
    if end < 0:
        raise ValueError("Teaching-block end marker not found in firmware source.")
    # keep the end marker (and everything after) in the suffix; back up to the
    # start of the end-marker line so the block ends with a clean newline.
    end_line = text.rfind("\n", start_eol, end) + 1
    prefix = text[:start_eol + 1]
    block = text[start_eol + 1:end_line]
    suffix = text[end_line:]
    return prefix, block, suffix


def _read_teaching_block():
    return _split_teaching_block(_read_output_source())[1]


def _default_teaching_block():
    if OUTPUT_DEMO_DEFAULT_BLOCK.is_file():
        return OUTPUT_DEMO_DEFAULT_BLOCK.read_text(encoding="utf-8")
    return _read_teaching_block()


def _validate_teaching_block(code):
    """Light guardrails on student code. It is compiled, not run on the AI PC,
    but we still keep it inside the marked region and bounded in size."""
    if code is None:
        raise ValueError("No code provided.")
    if len(code) > TEACHING_BLOCK_MAX_CHARS:
        raise ValueError(f"Code is too long (max {TEACHING_BLOCK_MAX_CHARS} characters).")
    if TEACHING_BLOCK_START in code or TEACHING_BLOCK_END in code:
        raise ValueError("Do not include the teaching-block markers in your code.")
    if "#include" in code:
        # includes belong in the fixed header, not the teaching block.
        raise ValueError("#include is not allowed in the teaching block.")
    if code.count("{") != code.count("}"):
        raise ValueError("Unbalanced { } braces in your code.")
    return code


def _write_teaching_block(code):
    """Replace the teaching block in app_main.c with `code` (validated)."""
    text = _read_output_source()
    prefix, _old, suffix = _split_teaching_block(text)
    body = code.replace("\r\n", "\n").rstrip("\n") + "\n"
    OUTPUT_DEMO_SOURCE.write_text(prefix + body + suffix, encoding="utf-8")


def build_output_firmware_command():
    """Allowlisted ESP-IDF build for the output teaching demo.

    Fixed command (no student input on the command line) — the edited code is
    written into the source file, then this compiles it. The IDF environment is
    sourced from export.sh; the target dir is the fixed teaching demo folder.
    """
    rel = OUTPUT_DEMO_DIR.resolve().relative_to(REPO_ROOT.resolve())
    script = (
        f'. "{IDF_EXPORT_SH}" >/dev/null 2>&1 && '
        f'idf.py -C "{rel}" build'
    )
    return ["bash", "-lc", script]


# --------------------------------------------------------------------------- #
# Flask app
# --------------------------------------------------------------------------- #
def create_app():
    app = Flask(__name__, template_folder=str(TEMPLATES_DIR))
    app.secret_key = os.environ.get("EECAMP_PORTAL_SECRET", "dev-only-change-me")
    # 2 GiB cap on uploaded dataset zips.
    app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024 * 1024
    jobs = JobManager()
    _prune_expired_temp_artifacts()
    threading.Thread(target=_artifact_cleanup_loop, daemon=True).start()

    @app.errorhandler(HTTPException)
    def _json_error(exc):
        # Return a clean JSON body ({"error": "..."}) instead of Flask's HTML page,
        # so the browser shows a readable message rather than raw HTML.
        return jsonify({"error": exc.description, "status": exc.code}), exc.code

    @app.errorhandler(Exception)
    def _json_unexpected(exc):  # pragma: no cover - safety net for 500s
        return jsonify({"error": "Internal server error.", "status": 500}), 500

    def _logged_in_team():
        team = session.get("team")
        configured_team = _team_from_hostname()
        return team if configured_team is not None and team == configured_team else None

    def _session_user_id():
        user_id = session.get("user_id")
        if not user_id:
            user_id = uuid.uuid4().hex
            session["user_id"] = user_id
        return user_id

    def _ml_job_owner_extra():
        return {
            "owner_id": _session_user_id(),
            "owner_team": _logged_in_team(),
        }

    def _request_dataset():
        owner_id = _session_user_id()
        return _user_dataset_dir(owner_id), _user_dataset_lock(owner_id)

    @app.before_request
    def _require_login():
        path = request.path
        if any(path.startswith(prefix) for prefix in PUBLIC_PATH_PREFIXES):
            return None
        if _logged_in_team() is None:
            if path.startswith("/api/"):
                return jsonify({"error": "login required", "status": 401}), 401
            return redirect(url_for("login", next=path))
        _session_user_id()
        return None

    @app.get("/login")
    def login():
        team = _team_from_hostname()
        return render_template("login.html", team=team, team_display=team or "unknown", next=request.args.get("next", "/"))

    @app.post("/login")
    def login_post():
        team = _team_from_hostname()
        password = request.form.get("password", "")
        nxt = request.form.get("next", "/") or "/"
        if team is None:
            return render_template("login.html", team=team, team_display=team or "unknown", next=nxt, error="Could not determine this AI PC team from the hostname."), 500
        if _team_password_matches(team, password):
            session["team"] = team
            session.setdefault("user_id", uuid.uuid4().hex)
            return redirect(nxt if nxt.startswith("/") else "/")
        return render_template("login.html", team=team, team_display=team or "unknown", next=nxt, error="Invalid password."), 401

    @app.post("/api/change-password")
    def change_password():
        team = _logged_in_team()
        if team is None:
            return jsonify({"error": "login required", "status": 401}), 401
        data = request.get_json(silent=True) or request.form.to_dict()
        current = str(data.get("current_password") or "")
        new_password = str(data.get("new_password") or "")
        confirm = str(data.get("confirm_password") or "")
        if not _team_password_matches(team, current):
            abort(400, "Current password is incorrect.")
        if new_password != confirm:
            abort(400, "New passwords do not match.")
        if len(new_password) < 6 or len(new_password) > 64:
            abort(400, "Password must be 6 to 64 characters.")
        _save_runtime_password_hash(team, new_password)
        return jsonify({"ok": True, "team": team})

    @app.post("/logout")
    def logout():
        session.clear()
        return redirect(url_for("login"))

    @app.get("/")
    @app.get("/home")
    @app.get("/model_finetune")
    @app.get("/deploy")
    @app.get("/output")
    @app.get("/firmware")
    @app.get("/camera_usb")
    @app.get("/drive")
    @app.get("/account")
    def index():
        return render_template("index.html", team=_logged_in_team(), topic=TOPIC_ROUTES.get(request.path, "home"))

    # ---------------- AI PC Drive API (login-gated by _require_login) ----------------
    @app.get("/api/drive/folders")
    def drive_folders():
        _ensure_drive()
        return jsonify({"team": _logged_in_team(), "folders": DRIVE_FOLDERS})

    @app.get("/api/drive/list")
    def drive_list():
        folder = request.args.get("folder", "0_shared")
        try:
            files = _drive_list(folder)
        except ValueError as exc:
            abort(400, str(exc))
        return jsonify({"folder": folder, "files": files})

    @app.post("/api/drive/upload")
    def drive_upload():
        folder = request.form.get("folder", "0_shared")
        if "file" not in request.files:
            abort(400, "No file part named 'file'.")
        upload = request.files["file"]
        if not upload.filename:
            abort(400, "No file selected.")
        try:
            folder_dir = _drive_folder_dir(folder)
        except ValueError as exc:
            abort(400, str(exc))
        safe = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(upload.filename))[:128] or "upload.bin"
        dest = folder_dir / safe
        upload.save(str(dest))
        return jsonify({"ok": True, "folder": folder, "name": dest.name, "size": dest.stat().st_size})

    @app.get("/api/drive/download")
    def drive_download():
        try:
            path = _drive_safe_file(request.args.get("folder", ""), request.args.get("name", ""))
        except ValueError as exc:
            abort(400, str(exc))
        if not path.is_file():
            abort(404, "File not found.")
        return send_file(str(path), as_attachment=True, download_name=path.name)

    @app.get("/api/drive/image")
    def drive_image():
        try:
            path = _drive_safe_file(request.args.get("folder", ""), request.args.get("name", ""))
        except ValueError as exc:
            abort(400, str(exc))
        if not path.is_file() or path.suffix.lower() not in IMAGE_EXTENSIONS:
            abort(404, "Image not found.")
        return send_file(str(path))   # inline preview

    @app.post("/api/drive/delete")
    def drive_delete():
        data = request.get_json(silent=True) or request.form.to_dict()
        try:
            path = _drive_safe_file(data.get("folder", ""), data.get("name", ""))
        except ValueError as exc:
            abort(400, str(exc))
        if path.is_file():
            path.unlink()
        return jsonify({"ok": True, "folder": data.get("folder", ""), "name": path.name})

    @app.get("/api/health")
    def health():
        active = jobs.active_job()
        return jsonify({
            "status": "ok",
            "team": _logged_in_team(),
            "repo_root": str(REPO_ROOT),
            "class_names": list(CLASS_NAMES),
            "active_job": active.id if active else None,
            "max_parallel_ml_jobs": ML_MAX_PARALLEL_JOBS,
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

    @app.get("/api/firmware/meta")
    def firmware_meta():
        """Firmware flash plan for a target, parsed from ESP-IDF's flasher_args.json.

        ?target=model_finetune | deploy_benchmark | main_board (default) | control_board | camera_usb_demo | output_demo. Returns the images (bootloader /
        partition-table / app) with ESP-IDF-generated offsets + flash settings, so
        the browser flasher never hardcodes offsets. If the target is not built,
        returns available=false with a student-friendly message.
        """
        target = request.args.get("target", "main_board")
        spec = FIRMWARE_TARGETS.get(target)
        if spec is None:
            abort(404, f"Unknown firmware target '{target}'.")
        build_dir = spec["build_dir"]
        flasher_args = build_dir / "flasher_args.json"
        not_built = {
            "available": False, "target": target,
            "message": f"{spec['label']} build artifacts were not found. "
                       f"Please build {spec['build_hint']} first.",
        }
        if not flasher_args.is_file():
            return jsonify(not_built)
        try:
            data = json.loads(flasher_args.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            return jsonify({"available": False, "target": target,
                            "message": f"Could not read firmware build metadata: {exc}"})

        settings = data.get("flash_settings", {}) or {}
        chip = (data.get("extra_esptool_args", {}) or {}).get("chip", "esp32s3")
        images = []
        for offset, rel in sorted((data.get("flash_files") or {}).items(),
                                  key=lambda kv: int(kv[0], 16)):
            img = (build_dir / rel).resolve()
            try:
                img.relative_to(build_dir.resolve())
            except ValueError:
                continue  # never serve outside the build dir
            if not img.is_file():
                continue
            images.append({
                "name": Path(rel).name, "rel": rel,
                "offset": offset, "offset_int": int(offset, 16),
                "size": img.stat().st_size,
                "url": f"/api/firmware/download?target={target}&name={rel}",
            })
        if not images:
            return jsonify(not_built)
        return jsonify({
            "available": True, "target": target, "chip": chip,
            "flash_mode": settings.get("flash_mode", "keep"),
            "flash_freq": settings.get("flash_freq", "keep"),
            "flash_size": settings.get("flash_size", "keep"),
            "images": images,
        })

    @app.get("/api/firmware/download")
    def firmware_download():
        """Serve one firmware image from a target's build dir (allowlisted)."""
        target = request.args.get("target", "main_board")
        spec = FIRMWARE_TARGETS.get(target)
        if spec is None:
            abort(404, f"Unknown firmware target '{target}'.")
        build_dir = spec["build_dir"]
        rel = request.args.get("name", "")
        if not rel:
            abort(400, "Missing 'name'.")
        candidate = (build_dir / rel).resolve()
        try:
            candidate.relative_to(build_dir.resolve())
        except ValueError:
            abort(403, "Path outside firmware build directory.")
        if not candidate.is_file() or candidate.suffix.lower() != ".bin":
            abort(404, "Firmware image not found.")
        return send_file(str(candidate), as_attachment=True, download_name=candidate.name)

    @app.get("/api/output/teaching-block")
    def output_teaching_block():
        """Return the current + default editable teaching block for the output demo."""
        try:
            return jsonify({
                "code": _read_teaching_block(),
                "default": _default_teaching_block(),
                "max_chars": TEACHING_BLOCK_MAX_CHARS,
                "source": "firmware/teaching_output_demo/main/app_main.c",
            })
        except ValueError as exc:
            abort(500, str(exc))

    @app.post("/api/output/build")
    def output_build():
        """Write the student's teaching block into app_main.c, then start an
        allowlisted `idf.py build`. Flashing is enabled by the UI only once this
        job succeeds. Returns the job id; logs stream via /api/jobs/<id>/log."""
        data = request.get_json(force=True, silent=True) or {}
        try:
            code = _validate_teaching_block(data.get("code"))
        except ValueError as exc:
            abort(400, str(exc))
        try:
            _write_teaching_block(code)
        except ValueError as exc:
            abort(500, str(exc))
        try:
            cmd = build_output_firmware_command()
            job = jobs.start("build", "build:teaching_output_demo", cmd)
        except JobBusyError as exc:
            return jsonify({"error": "busy", "active_job": exc.active_job.to_dict()}), 409
        return jsonify({"ok": True, "job": job.to_dict()})

    @app.post("/api/output/reset-block")
    def output_reset_block():
        """Restore the teaching block to its shipped default (does not build)."""
        try:
            code = _default_teaching_block()
            _write_teaching_block(code)
        except ValueError as exc:
            abort(500, str(exc))
        return jsonify({"ok": True, "code": code})

    @app.get("/api/reports")
    def quant_reports():
        """Latest quantization report metrics (Deploy tab results view).

        These are the AI-PC-side numbers produced during quantization (source vs
        int8 TFLite accuracy + score similarity). On-device latency/throughput
        require the board and are run with the benchmark CLI where the board lives.
        """
        out = []
        if ARTIFACT_REPORTS_DIR.is_dir():
            for path in sorted(ARTIFACT_REPORTS_DIR.glob("*.json"),
                               key=lambda p: p.stat().st_mtime, reverse=True):
                try:
                    r = json.loads(path.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    continue
                tv = r.get("tflite_validation") or {}
                out.append({
                    "name": path.name,
                    "model": r.get("model_name"),
                    "quant_format": r.get("requested_quant_format") or r.get("quant_format"),
                    "granularity": r.get("quantization_granularity"),
                    "class_order": r.get("class_order"),
                    "tflite_accuracy": tv.get("accuracy"),
                    "tflite_samples": tv.get("samples"),
                    "output_variation": tv.get("output_variation") or r.get("output_variation"),
                    "modified": datetime.fromtimestamp(path.stat().st_mtime, timezone.utc)
                                .strftime("%Y-%m-%dT%H:%M:%SZ"),
                })
        return jsonify({"reports": out})

    @app.get("/api/class-map")
    def get_class_map():
        """Return the current class order + per-class output-action mapping."""
        dataset_dir, dataset_lock = _request_dataset()
        with dataset_lock:
            payload = None
            class_map_path = _class_map_path(dataset_dir)
            if class_map_lib is not None:
                payload = class_map_lib.load_class_map(default_order=None, path=class_map_path)
            if payload is None:
                payload = {
                    "version": 1,
                    "class_order": list(CLASS_NAMES),
                    "num_classes": len(CLASS_NAMES),
                    "classes": [{"index": i, "name": n, "action": None} for i, n in enumerate(CLASS_NAMES)],
                    "default": True,
                }
            file_count = _dataset_file_count(dataset_dir)
            payload["output_actions"] = OUTPUT_ACTIONS
            payload["dataset_counts"] = _dataset_counts(dataset_dir)
            payload["available_classes"] = _dataset_class_names(dataset_dir)
            payload["max_active"] = NUM_CLASSES
            payload["has_dataset"] = file_count > 0
            payload["dataset_file_count"] = file_count
        return jsonify(payload)

    @app.post("/api/class-map")
    def set_class_map():
        """Set the ACTIVE class set and/or the class -> output-action mapping.

        Body:
          {"active": ["classA", "classB", ...]}  optional; the <= NUM_CLASSES
              classes used for training/quantization/inference. Each must exist in
              the dataset. Replaces the current active set (class order).
          {"actions": {"<class name>": "<action>", ...}}  optional; only classes in
              the active set may be mapped, each to one of OUTPUT_ACTIONS (or empty
              to clear).
        At least one of the two must be provided.
        """
        if class_map_lib is None:
            abort(500, "class_map module unavailable on the server.")
        data = request.get_json(silent=True) or {}
        dataset_dir, dataset_lock = _request_dataset()
        with dataset_lock:
            class_map_path = _class_map_path(dataset_dir)
            available = set(_dataset_class_names(dataset_dir))
            active_in = data.get("active", None)

            if active_in is not None:
                if not isinstance(active_in, list) or not active_in:
                    abort(400, "'active' must be a non-empty list of class names.")
                order = []
                for raw_name in active_in:
                    name = str(raw_name)
                    if name not in available:
                        abort(400, f"Class '{name}' is not in the dataset.")
                    if name not in order:
                        order.append(name)
                if len(order) > NUM_CLASSES:
                    abort(400, f"Select at most {NUM_CLASSES} active classes (got {len(order)}).")
                order = sorted(order)
            else:
                order = class_map_lib.load_class_order(default=None, path=class_map_path)
                if not order:
                    abort(400, "No active classes yet - select up to six classes first.")

            active_set = set(order)
            actions = {}
            existing = class_map_lib.load_class_map(default_order=None, path=class_map_path)
            if existing:
                for item in existing.get("classes", []):
                    if item.get("action") and item.get("name") in active_set:
                        actions[item["name"]] = item["action"]
            actions_in = data.get("actions", {})
            if actions_in:
                if not isinstance(actions_in, dict):
                    abort(400, "'actions' must be an object of {class_name: action}.")
                for name, action in actions_in.items():
                    if name not in active_set:
                        abort(400, f"Class '{name}' is not in the active set.")
                    if action in (None, ""):
                        actions.pop(name, None)
                        continue
                    if action not in OUTPUT_ACTIONS:
                        abort(400, f"action '{action}' must be one of {OUTPUT_ACTIONS}.")
                    actions[name] = action

            payload = class_map_lib.save_class_map(order, actions=actions, path=class_map_path)
            payload["output_actions"] = OUTPUT_ACTIONS
            payload["dataset_counts"] = _dataset_counts(dataset_dir)
            payload["available_classes"] = _dataset_class_names(dataset_dir)
            payload["max_active"] = NUM_CLASSES
            payload["has_dataset"] = True
            payload["dataset_file_count"] = _dataset_file_count(dataset_dir)
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
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        upload_dir = _user_upload_dir(owner_id)
        dataset_lock = _user_dataset_lock(owner_id)
        upload_dir.mkdir(parents=True, exist_ok=True)
        safe_name = re.sub(r"[^A-Za-z0-9._-]", "_", upload.filename)
        saved_zip = upload_dir / f"{uuid.uuid4().hex}-{safe_name}"
        upload.save(str(saved_zip))
        try:
            with dataset_lock:
                summary = _import_dataset(saved_zip, dataset_dir, upload_dir)
        except (ValueError, zipfile.BadZipFile) as exc:
            abort(400, str(exc))
        finally:
            try:
                saved_zip.unlink()
            except FileNotFoundError:
                pass
        return jsonify({"ok": True, "zip": safe_name, **summary})

    @app.get("/api/dataset/download")
    def dataset_download():
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        dataset_lock = _user_dataset_lock(owner_id)
        DATASET_EXPORTS_DIR.mkdir(parents=True, exist_ok=True)
        export_path = DATASET_EXPORTS_DIR / f"{_artifact_owner_slug(owner_id)}-{uuid.uuid4().hex}.zip"
        try:
            with dataset_lock:
                _write_dataset_zip(dataset_dir, export_path)
        except ValueError as exc:
            abort(404, str(exc))
        response = send_file(
            str(export_path),
            as_attachment=True,
            download_name="active-dataset.zip",
        )
        response.call_on_close(lambda: export_path.unlink(missing_ok=True))
        _delete_export_later(export_path)
        return response

    @app.post("/api/dataset/save-to-drive")
    def dataset_save_to_drive():
        data = request.get_json(silent=True) or request.form.to_dict()
        folder = str(data.get("folder") or "0_shared")
        requested_name = str(data.get("name") or "active-dataset.zip").strip()
        if (
            not requested_name
            or len(requested_name) > 128
            or requested_name in {".", ".."}
            or "/" in requested_name
            or "\\" in requested_name
        ):
            abort(400, "Invalid file name.")
        suffix = Path(requested_name).suffix.lower()
        if not suffix:
            requested_name += ".zip"
        elif suffix != ".zip":
            abort(400, "Dataset file name must use the .zip extension.")
        try:
            destination = _drive_safe_file(folder, requested_name)
        except ValueError as exc:
            abort(400, str(exc))
        if destination.exists():
            abort(409, f"{destination.name} already exists in {folder}.")

        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        dataset_lock = _user_dataset_lock(owner_id)
        try:
            with dataset_lock:
                _write_dataset_zip(dataset_dir, destination, mode="x")
        except FileExistsError:
            abort(409, f"{destination.name} already exists in {folder}.")
        except ValueError as exc:
            abort(404, str(exc))
        except Exception:
            destination.unlink(missing_ok=True)
            raise
        return jsonify({
            "ok": True,
            "folder": folder,
            "name": destination.name,
            "size": destination.stat().st_size,
        })

    @app.post("/api/dataset/delete")
    def dataset_delete():
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        upload_dir = _user_upload_dir(owner_id)
        dataset_lock = _user_dataset_lock(owner_id)
        with dataset_lock:
            deleted_files = _dataset_file_count(dataset_dir)
            shutil.rmtree(dataset_dir, ignore_errors=True)
            shutil.rmtree(upload_dir, ignore_errors=True)
        return jsonify({"ok": True, "deleted_files": deleted_files})

    @app.post("/api/dataset/delete-class")
    def dataset_delete_class():
        data = request.get_json(silent=True) or request.form.to_dict()
        name = str(data.get("name") or "").strip()
        if not name:
            abort(400, "Class name is required.")
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        dataset_lock = _user_dataset_lock(owner_id)
        with dataset_lock:
            # Whitelist against the dataset's own class list (not a raw path join)
            # so a crafted name can't escape into an arbitrary directory.
            available = _dataset_class_names(dataset_dir)
            if name not in available:
                abort(404, f"Class '{name}' is not in the dataset.")
            deleted_files = 0
            for split in ("train", "validation"):
                class_dir = Path(dataset_dir) / split / name
                if class_dir.is_dir():
                    deleted_files += sum(1 for p in class_dir.rglob("*") if p.is_file())
                    shutil.rmtree(class_dir, ignore_errors=True)
            # Don't use _refresh_class_map_from_dataset here: it's meant for the
            # "add a class after capture" case, and _dataset_class_names() unions
            # in whatever this class_map.json still says on disk -- since we
            # haven't rewritten it yet, that would resurrect the class we just
            # deleted. Recompute the active order ourselves, explicitly excluding it.
            class_map_path = _class_map_path(dataset_dir)
            remaining_available = [c for c in available if c != name]
            if not remaining_available:
                class_map_path.unlink(missing_ok=True)
                class_map = None
            else:
                order = []
                if class_map_lib is not None:
                    order = class_map_lib.load_class_order(default=None, path=class_map_path) or []
                order = [c for c in order if c != name]
                if not order:
                    order = sorted(remaining_available)[:NUM_CLASSES]
                class_map = _write_class_map(order, dataset_dir)
        return jsonify({
            "ok": True,
            "deleted_class": name,
            "deleted_files": deleted_files,
            "class_map": class_map,
        })

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
        owner_extra = _ml_job_owner_extra()
        dataset_dir = _user_dataset_dir(owner_extra["owner_id"])
        dataset_lock = _user_dataset_lock(owner_extra["owner_id"])
        try:
            with dataset_lock:
                recipe, cmd, cwd, extra = build_training_command(
                    recipe_key,
                    data,
                    dataset_dir,
                )
        except ValueError as exc:
            abort(400, str(exc))
        extra.update(owner_extra)
        extra["artifact_token"] = uuid.uuid4().hex
        try:
            job = jobs.start("train", f"train:{recipe_key}", cmd, cwd=cwd, extra=extra)
        except JobBusyError as exc:
            shutil.rmtree(cwd, ignore_errors=True)
            return jsonify({"error": "busy", "active_job": exc.active_job.to_dict()}), 409
        return jsonify(job.to_dict()), 202

    @app.post("/api/quantize")
    def quantize():
        data = request.get_json(silent=True) or request.form.to_dict()
        extra = _ml_job_owner_extra()
        extra["artifact_token"] = uuid.uuid4().hex
        try:
            cmd = build_quantize_command(data, extra)
        except ValueError as exc:
            abort(400, str(exc))
        try:
            job = jobs.start(
                "quantize",
                f"quantize:{data.get('model_name')}",
                cmd,
                extra=extra,
            )
        except JobBusyError as exc:
            _remove_temporary_result(
                extra["owner_id"],
                "quantize",
                extra["artifact_token"],
            )
            return jsonify({"error": "busy", "active_job": exc.active_job.to_dict()}), 409
        return jsonify(job.to_dict()), 202

    @app.post("/api/model/ov2640/capture")
    def model_ov2640_capture():
        """Save a browser-local or OV2640 frame into the training dataset."""
        data = request.get_json(silent=True) or request.form.to_dict()
        try:
            class_name = _sanitize_class_name(data.get("class_name"))
            split = str(data.get("split") or "train").strip().lower()
            if split not in {"train", "validation"}:
                raise ValueError("split must be 'train' or 'validation'.")
            source = str(data.get("source") or "camera").strip().lower()
            prefix = "webcam" if source == "local_webcam" else "ov2640"
            raw, _mime, _ext = _decode_image_payload(data.get("image_data"))
        except ValueError as exc:
            abort(400, str(exc))
        dataset_dir, dataset_lock = _request_dataset()
        filename = f"{prefix}_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}.png"
        try:
            with dataset_lock:
                target_dir = dataset_dir / split / class_name
                target_dir.mkdir(parents=True, exist_ok=True)
                target = target_dir / filename
                _save_processed_model_image_bytes(raw, target)
                class_map_payload = _refresh_class_map_from_dataset(
                    dataset_dir,
                    class_name,
                )
                counts = _dataset_counts(dataset_dir).get(class_name, {})
                target_size = target.stat().st_size
        except ValueError as exc:
            abort(400, str(exc))
        return jsonify({
            "ok": True,
            "class_name": class_name,
            "split": split,
            "filename": filename,
            "path": str(Path("dataset") / split / class_name / filename),
            "size_bytes": target_size,
            "preprocess": "grayscale_96x96_png",
            "counts": counts,
            "class_map": class_map_payload,
        })

    @app.post("/api/dataset/create-class")
    def dataset_create_class():
        data = request.get_json(silent=True) or request.form.to_dict()
        try:
            class_name = _sanitize_class_name(data.get("class_name"))
        except ValueError as exc:
            abort(400, str(exc))
        dataset_dir, dataset_lock = _request_dataset()
        try:
            with dataset_lock:
                (dataset_dir / "train" / class_name).mkdir(parents=True, exist_ok=True)
                (dataset_dir / "validation" / class_name).mkdir(parents=True, exist_ok=True)
                class_map_payload = _refresh_class_map_from_dataset(dataset_dir, class_name)
        except Exception as exc:
            abort(500, f"Failed to create class: {str(exc)}")
        return jsonify({
            "ok": True,
            "class_name": class_name,
            "class_map": class_map_payload
        })

    @app.post("/api/dataset/auto-split")
    def dataset_auto_split():
        owner_extra = _ml_job_owner_extra()
        dataset_dir, dataset_lock = _request_dataset()
        
        try:
            with dataset_lock:
                classes = set()
                train_dir = dataset_dir / "train"
                val_dir = dataset_dir / "validation"
                if train_dir.is_dir():
                    classes.update(p.name for p in train_dir.iterdir() if p.is_dir())
                if val_dir.is_dir():
                    classes.update(p.name for p in val_dir.iterdir() if p.is_dir())
                
                all_counts = {}
                for class_name in classes:
                    class_train = train_dir / class_name
                    class_val = val_dir / class_name
                    
                    images = []
                    if class_train.is_dir():
                        images.extend(class_train.glob("*"))
                    if class_val.is_dir():
                        images.extend(class_val.glob("*"))
                    
                    images = [img for img in images if img.is_file() and img.suffix.lower() in IMAGE_EXTENSIONS]
                    total = len(images)
                    if total == 0:
                        continue
                    
                    import random
                    random.seed(42)
                    random.shuffle(images)
                    
                    val_count = total // 5
                    if val_count == 0 and total >= 2:
                        val_count = 1
                    
                    val_images = set(images[:val_count])
                    
                    temp_dir = dataset_dir / "temp_split" / class_name
                    temp_dir.mkdir(parents=True, exist_ok=True)
                    
                    for img in images:
                        shutil.move(str(img), str(temp_dir / img.name))
                    
                    shutil.rmtree(str(class_train), ignore_errors=True)
                    shutil.rmtree(str(class_val), ignore_errors=True)
                    
                    class_train.mkdir(parents=True, exist_ok=True)
                    class_val.mkdir(parents=True, exist_ok=True)
                    
                    for img_path in temp_dir.iterdir():
                        if img_path.is_file():
                            is_val = any(v_img.name == img_path.name for v_img in val_images)
                            dest_dir = class_val if is_val else class_train
                            shutil.move(str(img_path), str(dest_dir / img_path.name))
                    
                    shutil.rmtree(str(temp_dir), ignore_errors=True)
                
                shutil.rmtree(str(dataset_dir / "temp_split"), ignore_errors=True)
                
                counts_all = _dataset_counts(dataset_dir)
                for class_name in classes:
                    all_counts[class_name] = counts_all.get(class_name, {"train": 0, "validation": 0})
                
                class_map_payload = {}
                if classes:
                    class_map_payload = _refresh_class_map_from_dataset(dataset_dir, list(classes)[0])
                
        except Exception as exc:
            abort(500, f"Split failed: {str(exc)}")
            
        return jsonify({
            "ok": True,
            "counts": all_counts,
            "class_map": class_map_payload
        })

    @app.get("/api/dataset/list-images")
    def dataset_list_images():
        class_name = request.args.get("class_name", "")
        try:
            class_name = _sanitize_class_name(class_name)
        except ValueError as exc:
            abort(400, str(exc))
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        
        result = {"train": [], "validation": []}
        for split in ("train", "validation"):
            class_dir = dataset_dir / split / class_name
            if class_dir.is_dir():
                files = []
                for p in class_dir.iterdir():
                    if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS:
                        files.append({
                            "name": p.name,
                            "mtime": p.stat().st_mtime
                        })
                files.sort(key=lambda x: x["mtime"], reverse=True)
                result[split] = [f["name"] for f in files]
        return jsonify({
            "ok": True,
            "class_name": class_name,
            "images": result
        })

    @app.get("/api/dataset/image")
    def dataset_image():
        split = request.args.get("split", "train")
        class_name = request.args.get("class_name", "")
        filename = request.args.get("filename", "")
        if split not in {"train", "validation"}:
            abort(400, "Invalid split.")
        try:
            class_name = _sanitize_class_name(class_name)
        except ValueError as exc:
            abort(400, str(exc))
        if not filename or "/" in filename or "\\" in filename or filename in {".", ".."}:
            abort(400, "Invalid filename.")
            
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        img_path = dataset_dir / split / class_name / filename
        if not img_path.is_file() or img_path.suffix.lower() not in IMAGE_EXTENSIONS:
            abort(404, "Image not found.")
        return send_file(str(img_path))

    @app.post("/api/dataset/delete-image")
    def dataset_delete_image():
        data = request.get_json(silent=True) or request.form.to_dict()
        split = data.get("split", "train")
        class_name = data.get("class_name", "")
        filename = data.get("filename", "")
        if split not in {"train", "validation"}:
            abort(400, "Invalid split.")
        try:
            class_name = _sanitize_class_name(class_name)
        except ValueError as exc:
            abort(400, str(exc))
        if not filename or "/" in filename or "\\" in filename or filename in {".", ".."}:
            abort(400, "Invalid filename.")
            
        owner_id = _session_user_id()
        dataset_dir = _user_dataset_dir(owner_id)
        img_path = dataset_dir / split / class_name / filename
        dataset_lock = _user_dataset_lock(owner_id)
        
        try:
            with dataset_lock:
                if img_path.is_file():
                    img_path.unlink()
                _refresh_class_map_from_dataset(dataset_dir, class_name)
        except Exception as exc:
            abort(500, f"Delete failed: {str(exc)}")
            
        return jsonify({"ok": True, "filename": filename})

    @app.post("/api/model/keras/predict")
    def model_keras_predict():
        """Run one browser-provided camera frame through a source .keras model."""
        data = request.get_json(silent=True) or request.form.to_dict()
        try:
            model_id = str(data.get("model_name") or "").strip()
            raw, _mime, _ext = _decode_image_payload(data.get("image_data"))
            x = _preprocess_image_bytes_for_keras(raw)
            dataset_dir, dataset_lock = _request_dataset()
            with dataset_lock:
                order = _current_class_order(dataset_dir) or CLASS_NAMES
            model, np = _load_predict_model(model_id, order)
            y = model.predict(x, verbose=0)
            scores = np.asarray(y)[0].astype(float).tolist()
            pred = int(np.argmax(scores)) if scores else -1
            class_name = order[pred] if 0 <= pred < len(order) else f"class_{pred}"
            raw_pred = pred
            raw_class_name = class_name
            # Most training models output probabilities; if they output logits,
            # normalize them so the portal has one consistent confidence gate.
            confidence = None
            if scores and 0 <= raw_pred < len(scores):
                arr = np.asarray(scores, dtype="float64")
                if np.all(np.isfinite(arr)):
                    total = float(arr.sum())
                    if arr.min() >= 0.0 and 0.99 <= total <= 1.01:
                        probs = arr
                    else:
                        shifted = arr - float(arr.max())
                        exp = np.exp(shifted)
                        probs = exp / float(exp.sum()) if float(exp.sum()) else exp
                    confidence = float(probs[raw_pred])
            if confidence is not None and confidence < 0.70:
                pred = -1
                class_name = "NULL"
        except (ValueError, RuntimeError) as exc:
            abort(400, str(exc))
        return jsonify({
            "ok": True,
            "model_name": model_id,
            "prediction": pred,
            "class_name": class_name,
            "confidence": confidence,
            "raw_prediction": raw_pred,
            "raw_class_name": raw_class_name,
            "scores": scores,
            "class_order": order,
        })
    @app.get("/api/serial-ports")
    def serial_ports():
        ports = _list_serial_ports()
        if ports is None:
            return jsonify({"ports": [], "error": "pyserial not installed"})
        return jsonify({"ports": ports})

    @app.get("/api/benchmark/options")
    def benchmark_options():
        """Datasets the portal offers for the browser Web Serial benchmark.

        The benchmark runs in the STUDENT's browser over Web Serial against the
        board on the student PC — the AI PC only serves the dataset images, so no
        AI-PC serial port is involved.
        """
        dataset_dir, dataset_lock = _request_dataset()
        with dataset_lock:
            datasets = _benchmark_datasets(dataset_dir)
        return jsonify({"datasets": datasets})

    @app.get("/api/benchmark/images")
    def benchmark_images():
        """List dataset images (label + URL) for the browser benchmark to fetch."""
        ds_id = request.args.get("dataset", "")
        try:
            limit = max(1, min(2000, int(request.args.get("limit", "120"))))
        except ValueError:
            limit = 120
        dataset_dir, dataset_lock = _request_dataset()
        with dataset_lock:
            if _benchmark_dataset_dir(ds_id, dataset_dir) is None:
                abort(404, "Unknown or empty benchmark dataset.")
            images = _benchmark_images(ds_id, dataset_dir, limit=limit)
        return jsonify({"dataset": ds_id, "count": len(images), "images": images})

    @app.get("/api/benchmark/image")
    def benchmark_image():
        """Serve one dataset image (allowlisted to the dataset dir, image types only)."""
        ds_id = request.args.get("dataset", "")
        dataset_dir, dataset_lock = _request_dataset()
        rel = request.args.get("name", "")
        if not rel:
            abort(400, "Missing 'name'.")
        with dataset_lock:
            root = _benchmark_dataset_dir(ds_id, dataset_dir)
            if root is None:
                abort(404, "Unknown benchmark dataset.")
            candidate = (root / rel).resolve()
            try:
                candidate.relative_to(root)
            except ValueError:
                abort(403, "Path outside dataset directory.")
            if not candidate.is_file() or candidate.suffix.lower() not in IMAGE_EXTENSIONS:
                abort(404, "Image not found.")
            data = candidate.read_bytes()
        return send_file(io.BytesIO(data), download_name=candidate.name)

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
        _prune_expired_temp_artifacts()
        items = []
        categories = [
            ("keras_source", TF_MODELS_DIR, {".keras", ".h5", ".onnx"}),
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
                        "path": rel.as_posix(),
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

    @app.post("/api/artifacts/save-to-drive")
    def save_artifact_to_drive():
        data = request.get_json(silent=True) or request.form.to_dict()
        rel = str(data.get("path") or "").strip()
        if not rel:
            abort(400, "Missing artifact path.")
        candidate = (REPO_ROOT / rel).resolve() if not os.path.isabs(rel) else Path(rel)
        source = _resolve_within_roots(candidate)
        if source is None or not source.is_file():
            abort(404, "Artifact not found or outside allowed directories.")
        if source.suffix.lower() not in ARTIFACT_EXTENSIONS:
            abort(403, "File type cannot be saved to AI PC Drive.")

        folder = str(data.get("folder") or "0_shared")
        requested_name = str(data.get("name") or source.name).strip()
        if (
            not requested_name
            or len(requested_name) > 128
            or requested_name in {".", ".."}
            or "/" in requested_name
            or "\\" in requested_name
        ):
            abort(400, "Invalid file name.")
        requested_suffix = Path(requested_name).suffix.lower()
        if not requested_suffix:
            requested_name += source.suffix
        elif requested_suffix != source.suffix.lower():
            abort(400, f"File name must keep the {source.suffix} extension.")
        try:
            destination = _drive_safe_file(folder, requested_name)
        except ValueError as exc:
            abort(400, str(exc))
        if destination.exists():
            abort(409, f"{destination.name} already exists in {folder}.")
        try:
            with source.open("rb") as source_fh, destination.open("xb") as destination_fh:
                shutil.copyfileobj(source_fh, destination_fh)
        except FileExistsError:
            abort(409, f"{destination.name} already exists in {folder}.")
        except FileNotFoundError:
            abort(404, "Artifact expired before it could be saved.")
        return jsonify({
            "ok": True,
            "folder": folder,
            "name": destination.name,
            "size": destination.stat().st_size,
        })

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
    _ensure_drive()   # create 0_shared/ and 1/..12/ for the AI PC Drive

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
