# Student Guide

Use the AI PC portal from Chrome or Edge. Do not run Python commands unless a teaching assistant asks you to use the local fallback.

## Login

Open your team URL:

| Team | URL |
|---:|---|
| 1 | `https://140.112.194.42:4431` |
| 2 | `https://140.112.194.42:4432` |
| 3 | `https://140.112.194.42:4433` |
| 4 | `https://140.112.194.42:4434` |
| 5 | `https://140.112.194.42:4435` |
| 6 | `https://140.112.194.42:4436` |
| 7 | `https://140.112.194.42:4437` |
| 8 | `https://140.112.194.42:4438` |
| 9 | `https://140.112.194.42:4439` |
| 10 | `https://140.112.194.42:4440` |

Your teacher will provide the team password. You can change it from the Account settings page.

## Portal Pages

| Page | What You Do |
|---|---|
| Home | Landing page and project link. |
| Model finetune | Capture OV2640 images, choose active gesture classes, train, quantize, and preview inference. |
| Deploy & benchmark | Flash benchmark firmware and model, then run on-device benchmark in the browser. |
| Output demo | Edit a small code block, build output firmware, flash it, and test LED/PWM behavior. |
| Main board firmware | Flash full main-board firmware and model, preview camera, see prediction, and connect the control board. |
| Camera + USB demo | Test camera streaming, capture, ESP flash storage, and USB MSC behavior. |
| AI PC Drive | Upload/download team files stored on this AI PC. Zip large file groups before upload. |

## Normal Full Flow

1. Go to Model finetune.
2. Capture photos or upload a dataset zip.
3. Select up to 6 active gesture classes.
4. Train a model.
5. Quantize it to a deployable `.tflite`.
6. Go to Deploy & benchmark and flash the benchmark firmware.
7. Flash the `.tflite` model.
8. Run the benchmark and check accuracy, latency, throughput, MAE, Max Error, and Cosine Similarity.
9. Go to Main board firmware.
10. Flash full main-board firmware and flash the chosen model again if needed.
11. Connect the main board and control board. The browser forwards predictions to the control board.

## Hardware Connections

Main board input pins:

| Signal | GPIO | Meaning |
|---|---:|---|
| Encoder CLK | 21 | Rotary encoder phase A. |
| Encoder DT | 47 | Rotary encoder phase B. |
| Encoder SW | 48 | Encoder push button. |
| Shutter button OUT | 39 | Physical capture button. |

The shutter module has `VCC`, `GND`, and `OUT`:

- `VCC` -> 3.3 V
- `GND` -> board GND
- `OUT` -> GPIO39

Use common ground. The encoder should also use 3.3 V logic, not 5 V logic.

## Camera Behavior

- Model finetune captures training images and stores them under the selected dataset class if one is selected.
- Camera + USB demo captures photos into the `camera_usb` folder on ESP flash.
- Main board captures photos into the `main_inference` folder on ESP flash when explicitly triggered.
- Continuous main-board inference should not mount/unmount the USB disk or save a photo for every prediction.

## Flashing Checklist

If flashing is stuck at `Connecting...` or `Board not responding`:

1. Use Chrome or Edge.
2. Use the HTTPS team URL, not plain HTTP.
3. Close Arduino Serial Monitor, `idf.py monitor`, PuTTY, VS Code monitor, or any other program using the same COM port.
4. Try a lower flash baud from the top bar, for example `460800` or `115200`.
5. Hold BOOT, click Connect/Flash, tap RESET/EN, then release BOOT after about 1 second.
6. Use a data-capable USB cable and try another USB port.
7. After a flash finishes, wait until the portal says the serial port is released before flashing the next item.

## Camera Troubleshooting

If all camera firmwares suddenly fail with `ESP_ERR_NOT_SUPPORTED`, `camera_fb_null`, or `Failed to acquire camera frame buffer`, but encoder/button logs still work, treat it as a camera hardware or camera-bus issue first:

1. Fully power-cycle the board and camera.
2. Reseat the OV2640 FPC cable.
3. Check the camera power rails and common ground.
4. Check XCLK on GPIO15, SCCB on GPIO4/GPIO5, PCLK on GPIO13, and data pins.
5. Try Camera + USB demo before debugging main-board inference.
6. Do not use Deploy benchmark to debug the camera. That firmware has no OV2640 camera path.
