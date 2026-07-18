# Local Flash Helper (student PC)

A tiny localhost web service that flashes the ESP32-S3 from the **student's own
PC**.

## Why it is needed

The AI PC runs the [training portal](../training_portal/) and produces the
`.tflite` model, but the ESP32-S3 board is connected to the **student PC's** USB
port. A web server on the AI PC cannot reach that USB port. This helper runs on
the student PC, downloads the chosen `.tflite` from the portal, and flashes it
locally with `esptool`.

## Run (on the student PC, board plugged in)

```bash
conda activate eecampedu
python apps/local_flash_helper/flash_helper.py
```

It listens on `http://127.0.0.1:8765` (localhost only). Keep the window open
while flashing from the portal page.

## Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | Helper + esptool status |
| GET | `/ports` | List local serial ports |
| POST | `/flash` | Download a `.tflite` from the portal and flash it |

`POST /flash` body:

```json
{ "artifact_url": "http://140.112.194.42:8081/api/artifacts/download?path=...",
  "port": "/dev/ttyUSB0", "offset": "0x310000", "baud": 460800 }
```

The browser fills `artifact_url` automatically from the portal's origin, so
gateway port-forwarding is handled transparently.

## Details

- **Default flash offset:** `0x310000` (ESP32-S3 model partition).
- **esptool invocation:** `python -m esptool …` — no assumption that an
  `esptool` executable is on `PATH`. Runs `write-flash --flash-size keep`.
- **CORS:** enabled (`Access-Control-Allow-Origin: *` + Private Network Access
  preflight) so the portal page (served from the AI PC / gateway origin) may
  call this localhost helper.

## Browser policy caveats

Because the page is served from the gateway origin while this helper is on
`127.0.0.1`, the call is cross-origin **and** public→localhost. It works in the
common lab setup (both sides plain `http`), but note:

- **Mixed content:** if the portal is ever served over `https`, the browser will
  block calls to `http://127.0.0.1`. Keep the portal on `http`, or run the
  helper over the same scheme.
- **Private Network Access (Chrome):** recent Chrome may require the PNA
  preflight (already sent by this helper). If a browser/enterprise policy blocks
  public→localhost entirely, use the CLI fallback below.

## CLI fallback (no browser)

```bash
conda activate eecampedu
# download the model from the portal, then:
python -m esptool --chip esp32s3 --port <PORT> --baud 460800 \
    write-flash --flash-size keep 0x310000 model.tflite
```
