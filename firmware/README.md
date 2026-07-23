# Firmware

This repository has multiple firmware targets because each teaching stage needs a different board behavior.

## Projects

| Path | Board | Purpose |
|---|---|---|
| `firmware/main_board/` | ESP32-S3 | Full system: OV2640 camera, USB CDC/MSC, TFLite Micro inference, encoder/button input, and control-board bridge. |
| `firmware/model_finetune/` | ESP32-S3 | Camera firmware for collecting model-training images and previewing model predictions. |
| `firmware/camera_usb_demo/` | ESP32-S3 | Camera + USB demo: streaming, capture, exposure controls, ESP flash storage, and MSC. |
| `firmware/deploy_benchmark/` | ESP32-S3 | Benchmark firmware. Receives frames over serial and returns `RESULT`. No physical OV2640 camera path. |
| `firmware/control_board/` | ESP32 | Separate servo control board. Receives action commands and drives the arm servos. |
| `firmware/teaching_output_demo/` | ESP32-S3 | Output teaching demo with a bounded editable code block. |

## Main Board Pin Summary

Input controls:

| Signal | GPIO |
|---|---:|
| Encoder CLK | 21 |
| Encoder DT | 47 |
| Encoder SW | 48 |
| Shutter button OUT | 39 |

OV2640 camera:

| Signal | GPIO |
|---|---:|
| SIOD / SCCB SDA | 4 |
| SIOC / SCCB SCL | 5 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |
| XCLK | 15 |
| Y9 / D7 | 16 |
| Y8 / D6 | 17 |
| Y7 / D5 | 18 |
| Y6 / D4 | 12 |
| Y5 / D3 | 10 |
| Y4 / D2 | 8 |
| Y3 / D1 | 9 |
| Y2 / D0 | 11 |

The shutter module should use 3.3 V logic: `VCC -> 3.3 V`, `GND -> GND`, `OUT -> GPIO39`.

## Model Partition

The model is a `.tflite` file written directly to the model partition. It is not converted into a model `.bin`.

`TFLiteGesture_esp.bin` is the ESP-IDF app firmware for the main board. It contains code, not model weights.

## Photo Storage Behavior

Expected folders on ESP flash:

- `model_finetune` for model-data captures
- `camera_usb` for camera + USB demo captures
- `main_inference` for explicit main-board captures

Main-board continuous inference should not save a photo for every prediction and should not repeatedly mount/unmount the MSC drive.

## Control Board Behavior

The control board is a separate ESP32 that drives servos. The portal connects to both boards when needed:

1. Main board predicts a gesture.
2. Browser/PC receives the prediction.
3. Browser/PC forwards the mapped action to the control board serial port.
4. Control board updates servo positions.

If confidence is below 70%, the prediction is treated as `null` / idle.

The action mapping is configured in the portal. A typical set is:

- up
- down
- left
- right
- clamp
- release

## Camera Debug

If camera initialization fails across `main_board`, `model_finetune`, and `camera_usb_demo` with the same code and power, debug the OV2640 hardware path first:

- Reseat the FPC cable.
- Fully power-cycle the board.
- Verify camera power rails and common ground.
- Probe XCLK GPIO15, SCCB GPIO4/GPIO5, PCLK GPIO13, and data lines.
- Use `camera_usb_demo` as the first camera sanity test.

Do not use `deploy_benchmark` for camera debug. It has no camera capture path.
