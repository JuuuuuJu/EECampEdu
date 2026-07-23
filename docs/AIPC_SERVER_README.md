# AI PC Server Guide

This guide is for teaching assistants and developers who maintain one AI PC for one team.

## Network Mapping

Classroom gateway:

- SSH: `ssh -p <220 + team> eecamp@140.112.194.42`
- Portal: `https://140.112.194.42:<4430 + team>`

Examples:

- Team 1: SSH `221`, portal `https://140.112.194.42:4431`
- Team 10: SSH `230`, portal `https://140.112.194.42:4440`

The portal process on the AI PC should listen on the configured internal port. The gateway must forward the public HTTPS port to that AI PC. Confirm both ends when access fails.

## Setup

```bash
cd ~/EECampEdu
conda activate eecampedu
pip install -r requirements.txt
```

If ESP-IDF is needed for firmware builds, make sure `idf.py` works in the same shell. The portal service reads `IDF_EXPORT_SH` from `deploy/eecamp-portal.env` if your ESP-IDF install is not in the default location.

## Runtime Secrets

Do not commit real passwords.

Use:

```bash
cp deploy/eecamp-portal.env.example deploy/eecamp-portal.env
nano deploy/eecamp-portal.env
```

`deploy/eecamp-portal.env` is git-ignored and should contain the real portal secret and team passwords.

## Services

Install user services:

```bash
cd ~/EECampEdu
bash deploy/install_services.sh
```

If the optional camera helper service is needed:

```bash
bash deploy/install_services.sh --with-camera
```

Start, stop, restart, and inspect:

```bash
systemctl --user start eecamp-portal
systemctl --user restart eecamp-portal
systemctl --user status eecamp-portal
journalctl --user -u eecamp-portal -f
```

Optional camera helper:

```bash
systemctl --user start eecamp-camera-app
systemctl --user restart eecamp-camera-app
systemctl --user status eecamp-camera-app
journalctl --user -u eecamp-camera-app -f
```

To keep services alive after logout, ask for sudo once:

```bash
sudo loginctl enable-linger $USER
```

## When To Restart

Restart `eecamp-portal` after editing:

- `apps/training_portal/server.py`
- `apps/training_portal/templates/index.html`
- Portal static files
- `deploy/eecamp-portal.env`
- Any firmware/PC tool path that the portal loads at startup

You do not need to restart the portal after only changing firmware source files, unless the page needs to rescan build artifacts.

## Health Checks

On the AI PC:

```bash
curl -k https://127.0.0.1:8080/api/health
curl -k https://140.112.194.42:4431/api/health
```

Use the correct team port. If localhost works but gateway does not, the problem is gateway forwarding or firewall policy, not Flask.

## Build And Flash Artifacts

Firmware flashing uses ESP-IDF build outputs:

- `bootloader.bin`
- `partition-table.bin`
- app firmware `.bin`, for example `TFLiteGesture_esp.bin`

Model flashing uses the `.tflite` file directly. There is no `.tflite -> .bin` conversion. The `.tflite` is written as raw bytes into the model partition, and the firmware maps that partition at runtime.

## Logs

Portal jobs write runtime logs under ignored folders in `apps/training_portal/runs/`.

Service logs:

```bash
journalctl --user -u eecamp-portal -n 200
journalctl --user -u eecamp-portal -f
```

Use these logs first when students report upload, train, quantize, build, flash metadata, or benchmark problems.
