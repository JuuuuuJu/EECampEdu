# Apps

`apps/` contains PC-side applications. UI and web code stay here, not inside firmware.

## Current Apps

| Path | Purpose |
|---|---|
| `training_portal/` | Main AI PC web portal. Normal student entry point. |
| `local_camera_app/` | Legacy localhost camera/control helper for a student PC. Not the normal portal flow. |
| `local_flash_helper/` | Legacy localhost flashing fallback. Browser Web Serial is preferred. |
| `esp32_cam_input_app/` | Native Dear ImGui / SDL3 prototype kept for reference and local experiments. |

## Normal Classroom Flow

Students open the AI PC portal:

```text
https://140.112.194.42:4430+team_number
```

The browser uses Web Serial to access boards connected to the student's PC. The AI PC hosts the page, artifacts, logs, and training jobs; it does not physically own the student's USB port.

## Legacy Native App

`esp32_cam_input_app/` is a Windows prototype with live preview and control panels. It is useful for local debugging, but the teaching flow should prefer the web portal.

Build from repository root:

```powershell
conda activate eecampedu
python scripts\build_input_app.py --clean
```

Run:

```powershell
apps\esp32_cam_input_app\build\eecampedu_input_demo.exe
```
