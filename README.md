# EECampEdu

Teaching workspace for a gesture-controlled robotic arm: train a gesture model,
quantize it to int8 TFLite, flash it to an **ESP32-S3 main board** (camera + USB +
TFLite Micro inference), and drive a **control board** (robot-arm servos). A
browser portal on the classroom **AI PC** runs the student workflow.

## Documentation

Pick the guide for your role:

- **[docs/STUDENT_README.md](docs/STUDENT_README.md)** — students using the browser
  portal. Which tab to use per class day (Output / Model / Deploy), what hardware
  you need, what to expect.
- **[docs/AIPC_SERVER_README.md](docs/AIPC_SERVER_README.md)** — teaching staff
  running the AI PC: environment, starting/stopping the portal server, logs,
  training, quantization, and building firmware artifacts for flashing.
- **[docs/LOCAL_LEGACY_README.md](docs/LOCAL_LEGACY_README.md)** — running the full
  pipeline from the CLI on a personal PC (train → quantize → flash → benchmark →
  camera/output), no portal.

Classroom networking (gateway port-forwarding, HTTPS) is in
[apps/training_portal/DEPLOYMENT.md](apps/training_portal/DEPLOYMENT.md).

## Layout

```text
model_finetune/                 training scripts, class_map.py, source-model handoff
firmware/main_board/            ESP32-S3 main board firmware (camera + inference)
firmware/control_board/         robot-arm servo output firmware
firmware/teaching_output_demo/  Output-class GPIO/LED/PWM demo firmware
firmware/pc/                    quantization + benchmark tools
apps/training_portal/           AI PC browser portal (train/quantize/flash tabs)
apps/local_camera_app/          student-PC webcam/gesture control app
apps/local_flash_helper/        student-PC flash helper (fallback only)
docs/                           the three guides above
```

Datasets, trained models, build output, and `runs/` are git-ignored and
regenerated locally — never committed.
