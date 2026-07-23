# EECampEdu

ESP32-S3 gesture-recognition robotic arm teaching system.

Students mainly use the AI PC web portal. The portal handles dataset import, model training, quantization, browser-based flashing, camera preview, benchmark, output demo, and team file storage. The ESP32-S3 main board runs camera capture, USB CDC/MSC, preprocessing, and TFLite Micro inference. The separate control board drives the servos.

## Classroom Access

Each team uses one AI PC.

| Team | Portal URL | SSH |
|---:|---|---|
| 1 | `https://140.112.194.42:4431` | `ssh -p 221 eecamp@140.112.194.42` |
| 2 | `https://140.112.194.42:4432` | `ssh -p 222 eecamp@140.112.194.42` |
| 3 | `https://140.112.194.42:4433` | `ssh -p 223 eecamp@140.112.194.42` |
| 4 | `https://140.112.194.42:4434` | `ssh -p 224 eecamp@140.112.194.42` |
| 5 | `https://140.112.194.42:4435` | `ssh -p 225 eecamp@140.112.194.42` |
| 6 | `https://140.112.194.42:4436` | `ssh -p 226 eecamp@140.112.194.42` |
| 7 | `https://140.112.194.42:4437` | `ssh -p 227 eecamp@140.112.194.42` |
| 8 | `https://140.112.194.42:4438` | `ssh -p 228 eecamp@140.112.194.42` |
| 9 | `https://140.112.194.42:4439` | `ssh -p 229 eecamp@140.112.194.42` |
| 10 | `https://140.112.194.42:4440` | `ssh -p 230 eecamp@140.112.194.42` |

Formula: portal port is `4430 + team number`; SSH port is `220 + team number`.

## Main Guides

- Student portal guide: `docs/STUDENT_README.md`
- AI PC server guide: `docs/AIPC_SERVER_README.md`
- Local fallback / legacy CLI guide: `docs/LOCAL_LEGACY_README.md`
- Website feature guide in Chinese: `docs/WEBSITE_INTERNAL_GUIDE.zh-TW.md`
- Website feature guide in English: `docs/WEBSITE_INTERNAL_GUIDE.en.md`
- Portal implementation notes: `apps/training_portal/README.md`
- Firmware projects: `firmware/README.md`
- Model training scripts: `model_finetune/README.md`

## Repository Layout

| Path | Purpose |
|---|---|
| `apps/training_portal/` | Flask web portal served on the AI PC. |
| `deploy/` | Linux service files and deployment helpers. |
| `firmware/main_board/` | Full ESP32-S3 main board firmware: camera, USB, inference, input controls, control-board bridge. |
| `firmware/model_finetune/` | ESP32-S3 camera firmware for collecting training images and previewing model behavior. |
| `firmware/camera_usb_demo/` | Camera + USB demo firmware. |
| `firmware/deploy_benchmark/` | UART-frame benchmark firmware. No OV2640 camera is used here. |
| `firmware/control_board/` | Separate ESP32 control board firmware for servos. |
| `firmware/teaching_output_demo/` | Output-class editable LED/PWM teaching demo. |
| `firmware/pc/` | PC-side benchmark and quantization tools used by the portal. |
| `model_finetune/` | Training scripts, dataset placeholders, class mapping, webcam fallback demo. |

## Important Notes

- Passwords are not committed in plaintext. Runtime secrets live in `deploy/eecamp-portal.env`, which is git-ignored.
- A `.tflite` model is flashed as raw `.tflite` bytes into the model partition. It is not converted into another `.bin`.
- `TFLiteGesture_esp.bin` is the ESP-IDF application firmware image, not the model.
- Browser flashing uses Web Serial. Use Chrome or Edge over HTTPS.
- The default flash baud is `921600`; lower it from the portal only if flashing is unstable.
