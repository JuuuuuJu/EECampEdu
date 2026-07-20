"""Student-PC local camera / preview / control app (localhost).

Why this runs on the STUDENT PC (not the AI PC)
------------------------------------------------
The ESP32-S3 is plugged into the student's own PC, so anything that needs the
board's USB serial port must run locally: live camera preview, reading the
inference ``RESULT`` line, and forwarding the chosen robot action to the control board
output board. The AI PC training portal cannot reach this USB port.

This is a small Flask app on ``127.0.0.1:8770`` that:
  * lists local serial ports (``/ports``),
  * connects to main board (inference board) and optionally control board (robot-arm board),
  * parses main board ``RESULT,<index>,<timings...>,<scores...>`` lines,
  * maps the predicted output INDEX -> class name -> robot action using the
    ``class_mapping.json`` produced by the training portal (index-only firmware),
  * forwards the mapped action to control board,
  * serves a tiny browser page showing the live gesture + action.

Camera preview: live JPEG-over-USB-CDC preview is currently provided by the
native app in ``apps/esp32_cam_input_app`` (Dear ImGui). Streaming those frames
as MJPEG from here is a documented next step (see ``camera_preview`` below); it
needs the same ESP CDC JPEG framing the native app already implements. This
scaffold intentionally focuses on the gesture-result + action-control path,
which is firmware-protocol-stable and testable without re-implementing the
camera transport.

Run (student PC, board plugged in):
    conda activate eecampedu
    python apps/local_camera_app/preview_app.py
"""

import argparse
import json
import sys
import threading
from pathlib import Path

from flask import Flask, jsonify, request

DEFAULT_BAUD = 115200
# Robot-arm actions, index-aligned with the control board output firmware apply_action() table.
OUTPUT_ACTIONS = ["up", "down", "left", "right", "clamp", "release"]

# Shared latest-inference state, updated by the main board reader thread.
_state_lock = threading.Lock()
_state = {
    "connected": False,
    "main_board_port": None,
    "control_board_port": None,
    "last_index": None,
    "last_scores": [],
    "last_class": None,
    "last_action": None,
    "raw": None,
    "error": None,
}
# class index -> {"name": str, "action": str|None}
_class_map = {"class_order": [], "classes": []}
_serial_handles = {"main_board": None, "control_board": None}
_reader_thread = None


def list_serial_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    return [
        {"device": p.device, "description": p.description or "", "hwid": p.hwid or ""}
        for p in list_ports.comports()
    ]


def _index_to_action():
    """Return dict index -> (name, action) from the loaded class map."""
    mapping = {}
    for entry in _class_map.get("classes", []):
        try:
            mapping[int(entry["index"])] = (entry.get("name"), entry.get("action"))
        except (KeyError, TypeError, ValueError):
            continue
    if not mapping:
        for i, name in enumerate(_class_map.get("class_order", [])):
            mapping[i] = (name, None)
    return mapping


def _parse_result_line(line):
    """Parse 'RESULT,<index>,<t0>,<t1>,<t2>,<score0>,...' -> (index, scores)."""
    parts = line.strip().split(",")
    if not parts or parts[0] != "RESULT" or len(parts) < 2:
        return None
    try:
        index = int(parts[1])
    except ValueError:
        return None
    scores = []
    for tok in parts[5:]:
        try:
            scores.append(int(tok))
        except ValueError:
            pass
    return index, scores


def _reader_loop(main_board):
    mapping = _index_to_action()
    while True:
        try:
            raw = main_board.readline().decode("utf-8", "replace").strip()
        except Exception as exc:  # noqa: BLE001 - serial disconnect etc.
            with _state_lock:
                _state["error"] = f"serial read failed: {exc}"
                _state["connected"] = False
            return
        if not raw:
            continue
        parsed = _parse_result_line(raw)
        if parsed is None:
            continue
        index, scores = parsed
        mapping = _index_to_action()  # re-read in case the class map was updated
        name, action = mapping.get(index, (None, None))
        with _state_lock:
            _state.update(last_index=index, last_scores=scores,
                          last_class=name, last_action=action, raw=raw)
        if action and _serial_handles["control_board"] is not None:
            try:
                _serial_handles["control_board"].write((action + "\n").encode("utf-8"))
            except Exception:  # noqa: BLE001 - best-effort forward
                pass


INDEX_HTML = """<!doctype html><meta charset=utf-8>
<title>Student PC — gesture control</title>
<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;padding:24px}
.big{font-size:40px;font-weight:700}.muted{color:#94a3b8}code{background:#1e293b;padding:1px 5px;border-radius:4px}</style>
<h1>🎥 Student PC — gesture control</h1>
<p class=muted>Reads main board <code>RESULT</code> over serial, maps the output index to a robot action
via <code>class_mapping.json</code>, forwards it to control board. Live camera preview: use the native app in
<code>apps/esp32_cam_input_app</code> (documented next step here).</p>
<div class=big id=g>—</div><div id=a class=muted>action: —</div><div id=s class=muted></div>
<script>
async function tick(){try{const r=await(await fetch('/status')).json();
document.getElementById('g').textContent = r.last_class ? (r.last_class+' (#'+r.last_index+')') : 'waiting…';
document.getElementById('a').textContent = 'action: '+(r.last_action||'—');
document.getElementById('s').textContent = (r.last_scores||[]).join('  ');}catch(e){}}
setInterval(tick,300);tick();
</script>"""


def create_app():
    app = Flask(__name__)

    @app.after_request
    def cors(resp):
        resp.headers["Access-Control-Allow-Origin"] = "*"
        resp.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type"
        resp.headers["Access-Control-Allow-Private-Network"] = "true"
        return resp

    @app.route("/connect", methods=["OPTIONS"])
    @app.route("/class-map", methods=["OPTIONS"])
    def _preflight():
        return ("", 204)

    @app.get("/")
    def index():
        return INDEX_HTML

    @app.get("/health")
    def health():
        return jsonify({"status": "ok", "output_actions": OUTPUT_ACTIONS,
                        "connected": _state["connected"], "python": sys.version.split()[0]})

    @app.get("/ports")
    def ports():
        found = list_serial_ports()
        if found is None:
            return jsonify({"ports": [], "error": "pyserial not installed"}), 200
        return jsonify({"ports": found})

    @app.post("/class-map")
    def set_class_map():
        """Load the class_mapping.json downloaded from the training portal."""
        data = request.get_json(silent=True) or {}
        order = data.get("class_order")
        classes = data.get("classes")
        if not isinstance(order, list) or not order:
            return jsonify({"ok": False, "error": "class_order (list) required"}), 400
        global _class_map
        _class_map = {"class_order": [str(x) for x in order],
                      "classes": classes if isinstance(classes, list) else
                                 [{"index": i, "name": n, "action": None} for i, n in enumerate(order)]}
        return jsonify({"ok": True, "class_order": _class_map["class_order"]})

    @app.post("/connect")
    def connect():
        try:
            import serial
        except ImportError:
            return jsonify({"ok": False, "error": "pyserial not installed"}), 500
        data = request.get_json(silent=True) or {}
        main_board_port = data.get("main_board_port")
        control_board_port = data.get("control_board_port")
        baud = int(data.get("baud", DEFAULT_BAUD))
        if not main_board_port:
            return jsonify({"ok": False, "error": "main_board_port required"}), 400
        global _reader_thread
        try:
            main_board = serial.Serial(main_board_port, baud, timeout=1)
            _serial_handles["main_board"] = main_board
            if control_board_port:
                _serial_handles["control_board"] = serial.Serial(control_board_port, baud, timeout=1)
        except Exception as exc:  # noqa: BLE001 - open failure
            return jsonify({"ok": False, "error": f"open failed: {exc}"}), 502
        with _state_lock:
            _state.update(connected=True, main_board_port=main_board_port, control_board_port=control_board_port, error=None)
        _reader_thread = threading.Thread(target=_reader_loop, args=(main_board,), daemon=True)
        _reader_thread.start()
        return jsonify({"ok": True, "main_board_port": main_board_port, "control_board_port": control_board_port})

    @app.get("/status")
    def status():
        with _state_lock:
            return jsonify(dict(_state))

    return app


def main():
    parser = argparse.ArgumentParser(description="EECampEdu student-PC camera/control app.")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host. Default: 127.0.0.1")
    parser.add_argument("--port", type=int, default=8770, help="Bind port. Default: 8770")
    parser.add_argument("--class-map", help="Optional path to a class_mapping.json to preload.")
    args = parser.parse_args()

    if args.class_map:
        try:
            data = json.loads(Path(args.class_map).read_text(encoding="utf-8"))
            global _class_map
            _class_map = {"class_order": data.get("class_order", []),
                          "classes": data.get("classes", [])}
        except (OSError, ValueError) as exc:
            print(f"[preview_app] could not load class map: {exc}")

    app = create_app()
    print(f"[preview_app] listening : http://{args.host}:{args.port}")
    print("[preview_app] POST /connect with {main_board_port, control_board_port?} after listing /ports.")
    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == "__main__":
    main()
