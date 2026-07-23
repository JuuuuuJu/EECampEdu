# Local Camera App

Legacy localhost helper for a student PC. The normal classroom flow now uses the AI PC portal pages directly through browser Web Serial.

Use this only when the portal camera/control path is unavailable and a teaching assistant needs a local fallback.

## Run

```bash
conda activate eecampedu
python apps/local_camera_app/preview_app.py
```

Open:

```text
http://127.0.0.1:8770
```

## Scope

This helper can read local serial ports and display gesture/control state. It is not the source of truth for training, quantization, or artifact management. Those live in `apps/training_portal/`.

## Main Limitation

`127.0.0.1` always means the student's own PC. The AI PC cannot access a board plugged into a student PC unless the browser grants Web Serial access or a local helper is running on that student PC.
