# Training Portal

Flask web portal for the AI PC. It gives students a browser GUI for model finetune, quantization, flashing, benchmark, camera preview, output demo, main-board firmware, and shared AI PC Drive.

## Routes

| Route | Purpose |
|---|---|
| `/` or `/home` | Home page. |
| `/model_finetune` | Dataset capture/import, active class selection, training, quantization, model preview. |
| `/deploy` | Deploy benchmark firmware, flash model, run browser benchmark. |
| `/output` | Output teaching demo: edit bounded C block, build, flash, test. |
| `/firmware` | Main board firmware and model flashing, live camera/prediction, control-board bridge. |
| `/camera_usb` | Camera + USB demo. |
| `/drive` | Team AI PC Drive. |
| `/account` | Password change. |

## Public Access

The classroom gateway uses HTTPS ports `4431` to `4440`:

- Team N portal: `https://140.112.194.42:4430+N`
- Team N SSH: `ssh -p 220+N eecamp@140.112.194.42`

The portal should be served through HTTPS because browser Web Serial requires a secure context.

## Run As A Service

```bash
cd ~/EECampEdu
bash deploy/install_services.sh
systemctl --user restart eecamp-portal
journalctl --user -u eecamp-portal -f
```

Restart after changing `server.py`, `templates/index.html`, portal static assets, or `deploy/eecamp-portal.env`.

## Web Serial Flashing

The browser talks directly to the board plugged into the student's PC. The AI PC serves artifacts and JavaScript; it does not physically access the student's USB port.

All low-level offsets are hidden from students:

- Firmware flashing uses ESP-IDF `flasher_args.json` and build outputs.
- Model flashing writes the selected `.tflite` to the model partition.
- Default baud is `921600`; users can lower it if flashing is flaky.

## Dataset Rules

The dataset may contain many class folders. One training/quantization run uses at most 6 active classes. The active set is stored in the portal class map and used consistently for training, quantization, benchmark, preview, and class-to-action mapping.

Accepted image formats include common `.jpg`, `.jpeg`, `.png`, and `.bmp` layouts under class folders. Large uploads should be zipped.

## Runtime Data

Ignored runtime data:

- Uploaded datasets
- Job logs
- AI PC Drive files
- Generated web metadata

Do not commit generated datasets, model artifacts, service env files, or portal run logs.
