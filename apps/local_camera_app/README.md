# Local Camera / Preview / Control App (student PC)

A small localhost app that runs on the **student PC** (where the ESP32-S3 is
plugged in) to show the live gesture result and drive the robot arm. It is
separate from the AI PC training portal on purpose — only the student PC can
reach the board's USB serial port.

## Three-app architecture (evaluation)

| App | Runs on | Needs local USB? | Purpose |
|-----|---------|------------------|---------|
| Training portal (`apps/training_portal`) | **AI PC** | no | upload dataset, train, quantize, download artifacts |
| Local flash helper (`apps/local_flash_helper`) | **student PC** | yes (flash) | flash the `.tflite` to the ESP32-S3 |
| Camera/control app (`apps/local_camera_app`) | **student PC** | yes (serial) | live gesture result + class→action mapping + ESP2 forward |

**Why not put camera/preview in the AI PC server?** Live preview and reading the
inference `RESULT` line require the board's USB port, which physically lives on
the student PC. The AI PC has no access to it, so this must stay local. Keeping
it a separate localhost app (rather than merging into the flash helper) keeps
each concern independent; they *may* be merged later since both are student-PC
localhost services.

## Run (student PC, board plugged in)

```bash
conda activate eecampedu
python apps/local_camera_app/preview_app.py   # http://127.0.0.1:8770
```

Then open `http://127.0.0.1:8770/` for the live view.

## Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | status + available actions |
| GET | `/ports` | list local serial ports |
| POST | `/class-map` | load the `class_map.json` downloaded from the portal |
| POST | `/connect` | `{esp1_port, esp2_port?, baud?}` — open serial + start reader |
| GET | `/status` | latest gesture: index, scores, class name, mapped action |

The reader parses ESP1 `RESULT,<index>,<t0>,<t1>,<t2>,<score...>` (index-only
firmware), maps `index → class name → action` via the class map, and forwards
the action string to ESP2.

## Class → action mapping

The mapping comes from the training portal's `class_map.json` (the same file
that records the six class folder names). Download it from the portal
(`/api/class-map`) and `POST` it here, or preload with `--class-map path.json`.
Actions are `up/down/left/right/clamp/release`, index-aligned with the ESP2
output firmware.

## Camera preview status (scope)

Live JPEG-over-USB-CDC **camera preview** is currently provided by the native
Dear ImGui app in [`apps/esp32_cam_input_app`](../esp32_cam_input_app/). Serving
those frames as MJPEG from this local web app is a **documented next step**: it
needs the same ESP CDC JPEG framing the native app already implements. This
scaffold deliberately implements the firmware-protocol-stable gesture/result +
action-control path first, and does not fake camera frames.
