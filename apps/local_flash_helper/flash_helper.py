"""Local flash helper for the STUDENT PC.

Why this exists
---------------
The ESP32-S3 is plugged into the student's own lab PC, not into the AI PC that
runs the training portal. A web server on the AI PC therefore has no access to
the student PC's USB serial port. This tiny helper runs *on the student PC*,
listens on localhost only, downloads the chosen .tflite artifact from the AI PC
portal, and flashes it locally with esptool.

Endpoints (localhost only):
    GET  /health   -> helper + esptool status
    GET  /ports    -> list local serial ports
    POST /flash     -> download a .tflite from the portal and flash it

Run (on the student PC, board plugged in):
    conda activate eecampedu
    python apps/local_flash_helper/flash_helper.py

Security notes:
  * Binds 127.0.0.1 by default (not reachable from the network).
  * esptool is invoked as `python -m esptool` (no PATH assumption), shell=False.
  * Only http/https artifact URLs are accepted; the downloaded file must be a
    .tflite and is flashed to the model partition offset (default 0x310000).
"""

import argparse
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from urllib.parse import urlparse

from flask import Flask, jsonify, request

DEFAULT_FLASH_OFFSET = "0x310000"   # ESP32-S3 model partition (per project layout)
DEFAULT_BAUD = 921600
DEFAULT_CHIP = "esp32s3"
FLASH_TIMEOUT_SECONDS = 240
DOWNLOAD_TIMEOUT_SECONDS = 120
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024  # a model partition image is well under this


def esptool_version():
    try:
        out = subprocess.run(
            [sys.executable, "-m", "esptool", "version"],
            capture_output=True, text=True, timeout=30,
        )
        text = (out.stdout or out.stderr).strip().splitlines()
        return text[-1] if text else "unknown"
    except Exception:  # noqa: BLE001
        return None


def list_serial_ports():
    """Enumerate local serial ports via pyserial."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None, "pyserial is not installed (pip install pyserial / esptool pulls it in)"
    ports = []
    for p in list_ports.comports():
        ports.append({
            "device": p.device,
            "description": p.description or "",
            "hwid": p.hwid or "",
        })
    return ports, None


def parse_offset(value):
    """Accept '0x310000' or a decimal string; return a normalized 0x-hex string."""
    if value in (None, ""):
        return DEFAULT_FLASH_OFFSET
    value = str(value).strip()
    try:
        number = int(value, 16) if value.lower().startswith("0x") else int(value)
    except ValueError:
        raise ValueError(f"Invalid flash offset: {value!r}")
    if number < 0 or number > 0x1000000:  # 16 MiB ceiling, generous
        raise ValueError(f"Flash offset out of range: {value!r}")
    return hex(number)


def download_artifact(artifact_url, dest_path):
    parsed = urlparse(artifact_url)
    if parsed.scheme not in ("http", "https"):
        raise ValueError("artifact_url must be an http/https URL")
    total = 0
    with urllib.request.urlopen(artifact_url, timeout=DOWNLOAD_TIMEOUT_SECONDS) as resp:  # noqa: S310
        with open(dest_path, "wb") as fh:
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_ARTIFACT_BYTES:
                    raise ValueError("Artifact exceeds maximum allowed size")
                fh.write(chunk)
    return total


def create_app(chip=DEFAULT_CHIP, default_baud=DEFAULT_BAUD):
    app = Flask(__name__)

    # -- CORS: the portal page is served from the AI PC (a different origin),
    #    so the browser needs permission to call this localhost helper. --------
    @app.after_request
    def add_cors_headers(resp):
        resp.headers["Access-Control-Allow-Origin"] = "*"
        resp.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type"
        # Chrome Private Network Access preflight for public-site -> localhost.
        resp.headers["Access-Control-Allow-Private-Network"] = "true"
        return resp

    @app.route("/flash", methods=["OPTIONS"])
    @app.route("/ports", methods=["OPTIONS"])
    @app.route("/health", methods=["OPTIONS"])
    def cors_preflight():
        return ("", 204)

    @app.get("/health")
    def health():
        return jsonify({
            "status": "ok",
            "chip": chip,
            "default_offset": DEFAULT_FLASH_OFFSET,
            "default_baud": default_baud,
            "esptool_version": esptool_version(),
            "python": sys.version.split()[0],
        })

    @app.get("/ports")
    def ports():
        found, err = list_serial_ports()
        if err:
            return jsonify({"ports": [], "error": err}), 200
        return jsonify({"ports": found})

    @app.post("/flash")
    def flash():
        data = request.get_json(silent=True) or {}
        artifact_url = data.get("artifact_url")
        port = data.get("port")
        if not artifact_url:
            return jsonify({"ok": False, "error": "artifact_url is required"}), 400
        if not port:
            return jsonify({"ok": False, "error": "port is required"}), 400
        try:
            offset = parse_offset(data.get("offset"))
        except ValueError as exc:
            return jsonify({"ok": False, "error": str(exc)}), 400
        baud = data.get("baud", default_baud)
        try:
            baud = int(baud)
        except (TypeError, ValueError):
            return jsonify({"ok": False, "error": "baud must be an integer"}), 400

        with tempfile.TemporaryDirectory() as tmp:
            model_path = Path(tmp) / "model.tflite"
            try:
                size = download_artifact(artifact_url, model_path)
            except Exception as exc:  # noqa: BLE001
                return jsonify({"ok": False, "error": f"download failed: {exc}"}), 502

            cmd = [
                sys.executable, "-m", "esptool",
                "--chip", chip,
                "--port", str(port),
                "--baud", str(baud),
                "write-flash",
                "--flash-size", "keep",
                offset, str(model_path),
            ]
            try:
                proc = subprocess.run(
                    cmd, capture_output=True, text=True,
                    timeout=FLASH_TIMEOUT_SECONDS, shell=False,
                )
            except FileNotFoundError:
                return jsonify({"ok": False, "error": "Could not run 'python -m esptool'. Is esptool installed?"}), 500
            except subprocess.TimeoutExpired:
                return jsonify({"ok": False, "error": "esptool timed out"}), 504

            output = (
                f"$ {' '.join(cmd)}\n"
                f"# downloaded {size} bytes -> model.tflite\n"
                f"{'-' * 60}\n"
                f"{proc.stdout}\n{proc.stderr}"
            )
            return jsonify({
                "ok": proc.returncode == 0,
                "returncode": proc.returncode,
                "offset": offset,
                "bytes": size,
                "output": output,
            }), (200 if proc.returncode == 0 else 500)

    return app


def main():
    parser = argparse.ArgumentParser(description="EECampEdu student-PC local flash helper.")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host. Default: 127.0.0.1 (localhost only)")
    parser.add_argument("--port", type=int, default=8765, help="Bind port. Default: 8765")
    parser.add_argument("--chip", default=DEFAULT_CHIP, help="Target chip. Default: esp32s3")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Flash baud. Default: {DEFAULT_BAUD}")
    args = parser.parse_args()

    app = create_app(chip=args.chip, default_baud=args.baud)
    print(f"[flash_helper] chip        : {args.chip}")
    print(f"[flash_helper] default off : {DEFAULT_FLASH_OFFSET}")
    print(f"[flash_helper] listening   : http://{args.host}:{args.port}")
    print("[flash_helper] Keep this window open while flashing from the portal page.")
    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == "__main__":
    main()
