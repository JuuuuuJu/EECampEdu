# AI PC Website Guide

Internal guide for staff. Students should use the browser portal and should not type CLI commands during normal class flow.

## Access

One AI PC serves one team.

- Team N portal: `https://140.112.194.42:4430+N`
- Team N SSH: `ssh -p 220+N eecamp@140.112.194.42`

Example: Team 1 uses `https://140.112.194.42:4431` and `ssh -p 221 eecamp@140.112.194.42`.

## Services

Portal:

```bash
systemctl --user restart eecamp-portal
systemctl --user status eecamp-portal
journalctl --user -u eecamp-portal -f
```

Optional camera helper:

```bash
systemctl --user restart eecamp-camera-app
systemctl --user status eecamp-camera-app
journalctl --user -u eecamp-camera-app -f
```

Restart the portal after editing `server.py`, `templates/index.html`, static portal files, or `deploy/eecamp-portal.env`.

## Pages

| Page | Purpose |
|---|---|
| Home | Landing page and repo link. |
| Model finetune | OV2640 dataset capture, active class selection, training, quantization, model preview. |
| Deploy & benchmark | Flash benchmark firmware and model, then run browser benchmark. |
| Output demo | Student edits a bounded C block, AI PC builds it, successful build unlocks flashing. |
| Main board firmware | Flash full main board, stream camera, show prediction, and forward actions to the control board. |
| Camera + USB demo | Camera preview, capture, ESP flash storage, and USB MSC demo. |
| AI PC Drive | Small team file area on the AI PC. Zip large uploads. |

## Model vs Firmware

- `.tflite` is the model and is flashed directly into the model partition.
- There is no `.tflite -> .bin` conversion.
- `TFLiteGesture_esp.bin` is the ESP-IDF application firmware, not the model.
- Model flashing and firmware flashing are separate operations.

## Prediction And Control Board

Any prediction with confidence below 70% is treated as `null` / idle.

For the full system, the browser connects to both:

1. ESP32-S3 main board
2. ESP32 control board

The browser receives prediction from the main board, maps it to an action, and sends the action to the control board. The control board drives the servos.

## Camera Debug

If all camera firmwares fail with `ESP_ERR_NOT_SUPPORTED`, `camera_fb_null`, or no frame buffer while input logs still work:

1. Power-cycle the board and camera.
2. Reseat the OV2640 FPC cable.
3. Verify camera power rails and common ground.
4. Probe GPIO15 XCLK, GPIO4/GPIO5 SCCB, GPIO13 PCLK, and data pins.
5. Test with Camera + USB demo first.
6. Do not debug camera with Deploy benchmark; that firmware has no physical camera path.
