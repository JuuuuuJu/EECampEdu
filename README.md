# EECampEdu

Gesture-recognition robotic arm teaching repo.

Students use the AI PC browser portal to collect/train model data, quantize a deployable TFLite model, flash ESP32-S3 firmware/model images, run benchmark jobs, and test output hardware without typing project CLI commands.

## Guides

- [Student guide](docs/STUDENT_README.md): browser-only classroom flow.
- [AI PC server guide](docs/AIPC_SERVER_README.md): teacher/developer setup, background services, logs, and firmware artifacts.
- [Local legacy guide](docs/LOCAL_LEGACY_README.md): full CLI workflow on a personal machine.

## Main Areas

```text
model_finetune/                 training scripts, class mapping, source model outputs
firmware/main_board/            ESP32-S3 main board firmware: camera + USB + preprocessing + TFLite Micro
firmware/control_board/         servo control board firmware
firmware/teaching_output_demo/  standalone GPIO/LED/PWM output teaching firmware
firmware/pc/                    quantization, artifacts, reports, benchmark scripts
apps/training_portal/           AI PC web portal served to students
apps/local_camera_app/          optional local camera/control helper for the PC with USB hardware
apps/local_flash_helper/        fallback-only local flashing helper
docs/                           role-specific guides
```

Runtime data is ignored by git: datasets, generated models, firmware build output, portal runs/logs, uploaded zips, and benchmark artifacts.