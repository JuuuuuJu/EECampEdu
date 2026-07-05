# System Overview

## Team Boundaries

| Team | Responsibility | Integration Output |
| --- | --- | --- |
| model-finetune | Foundation model, user-defined gesture dataset, fine-tuning, quantization | `.tflite`, class mapping, preprocessing metadata |
| deploy | ESP32-S3 model loading, preprocessing, inference, benchmark | firmware, model flashing tool, latency/accuracy report |
| camera | OV2640 capture and image storage | frame buffer or stored image |
| input-interface | USB CDC/MSC command channel and PC tool | command protocol and host UI/tool |
| mechanism/output | Robotic arm movement | motion API driven by gesture class |

## Runtime Flow

```text
CAPTURE command or local trigger
  -> OV2640 frame capture
  -> save image to photos flash partition
  -> read latest image
  -> convert to grayscale if needed
  -> crop hand region on ESP32-S3
  -> resize to 96 x 96
  -> quantize using TFLite input scale / zero-point
  -> TFLite Micro Invoke
  -> map class index to action
  -> drive mechanism
```

## Camera Format Decision

OV2640 is not restricted to a single output format. In the current camera
reference code, the firmware can switch between:

- `PIXFORMAT_GRAYSCALE`
- `PIXFORMAT_RGB565`
- `PIXFORMAT_YUV422`
- `PIXFORMAT_JPEG`

For the first stable integration, use grayscale capture when possible. JPEG is
useful for higher resolution and smaller flash storage size, but it requires a
JPEG decode step before grayscale/crop/model input.

## Recommended First Milestone

1. Keep `TEST_MODE_UART_FRAME` working without OV2640.
2. ESP receives a grayscale frame from the PC benchmark.
3. ESP runs crop + TFLite inference.
4. ESP returns `RESULT` over USB/serial.
5. Add OV2640 capture and `photos` partition storage behind `CAMERA_FLASH_MODE`.
6. Mechanism team maps `up/down/right/left/null` to arm actions.
