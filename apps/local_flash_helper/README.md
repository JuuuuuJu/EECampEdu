# Local Flash Helper

Legacy fallback only. The normal classroom flow flashes from the AI PC portal using browser Web Serial over HTTPS.

Use this helper only if browser Web Serial is blocked and a teaching assistant decides to run a local fallback on the student PC.

## Run

```bash
conda activate eecampedu
python apps/local_flash_helper/flash_helper.py
```

It listens only on:

```text
http://127.0.0.1:8765
```

## What It Does

- Lists serial ports on the local student PC.
- Downloads a selected artifact from the AI PC portal.
- Runs `python -m esptool` locally.

## Notes

- Default model partition offset is hidden from students in the normal portal flow.
- Normal browser flashing default baud is `921600`; lower it only if flashing is unstable.
- A `.tflite` model is written directly to the model partition. It is not converted into a `.bin`.
