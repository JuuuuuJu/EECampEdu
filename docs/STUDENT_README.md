# Student Guide

Use **Chrome or Edge**. Your instructor gives you a team URL and account.

Example:

```text
https://140.112.194.42:8081
```

If the browser shows a certificate warning, choose **Advanced -> Proceed** once. After login, use the top menu to open each page.

## Model Finetune Page

Purpose: collect/check gesture data, train a model, and preview camera/model behavior.

1. Upload a dataset zip when provided. Accepted layout: class folders such as `up/`, `ok/`, `thumb/`, or any six custom names.
2. Map each class to a robot action: `up`, `down`, `left`, `right`, `clamp`, `release`.
3. Choose TensorFlow or PyTorch and a model recipe.
4. Click **Start training** and watch the live job log.
5. Flash the model/full camera firmware shown on the page if the instructor asks you to update it.
6. For OV2640 preview, plug the ESP32-S3 main board into your PC, then click **Connect OV2640 preview**. The browser borrows the serial port, sends camera stream commands, displays incoming JPEG frames, and shows live prediction/result lines. No Python or terminal command is needed.

Expected result: a trained source model appears in the artifact list, and OV2640 preview shows frames plus current prediction when the full main board firmware is running.

## Deploy Page

Purpose: quantize, flash model, and benchmark deployment quality.

1. Flash **deploy benchmark firmware** from the Deploy page if the instructor asks you to update it.
2. Pick a trained `.keras` model.
3. Choose a deployable quantization format, usually `int8 / per-channel`.
4. Click **Start quantization** and wait for a `.tflite` artifact and report.
5. Plug the ESP32-S3 main board into **your PC**, then use **Flash model**. The model partition offset is hidden.
6. Pick a **dataset** and how many **images to test**, then click **Connect ESP32-S3 & run benchmark**. This runs
   **in your browser over Web Serial** against the board on your PC (the AI PC only serves the images) — pick your
   board in the port chooser. The portal shows label accuracy, model latency, preprocess latency, device compute,
   and throughput/FPS. (Flash the **deploy benchmark firmware** first so the board answers with `RESULT` lines.)

Expected result: model flashing succeeds and benchmark results appear in the page table + log. Use **Chrome or Edge**
over the `https://` portal address. You never plug the board into the AI PC and never run Python.

## Output Page

Purpose: test GPIO/PWM output independently from the full robot arm.

1. Flash the output demo firmware from the page.
2. Connect for control.
3. Use LED ON/OFF, brightness slider, blink, and status controls.

Expected result: LED digital output and PWM brightness respond immediately.

## Firmware Page

Purpose: flash the full ESP32-S3 main board firmware.

Use this for a new board or when the instructor updates camera/USB/continuous-inference firmware. The page flashes the ESP-IDF build images automatically; students do not type offsets or esptool commands. After flashing, use **Connect camera stream** to see OV2640 live preview and the latest prediction result.

## Flashing / Serial Checklist

- Use Chrome or Edge over HTTPS.
- Close Arduino Serial Monitor, `idf.py monitor`, PuTTY, or any other program using the port.
- If auto-reset fails while flashing: hold BOOT, click connect, tap RESET/EN, release BOOT after about 1 second.
- Use a data-capable USB cable.
- After one serial workflow finishes, wait for the status to say done/released before starting the next one.
