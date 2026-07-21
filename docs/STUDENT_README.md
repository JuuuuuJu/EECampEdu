# Student Guide

Use **Chrome or Edge**. Your instructor gives you a team URL and account.

Example:

```text
https://140.112.194.42:8081
```

If the browser shows a certificate warning, choose **Advanced -> Proceed** once. After login, use the top menu to open each page.

## Model Finetune Page

Purpose: collect/check gesture data, train a model, and preview camera/model behavior.

1. Upload a dataset zip when provided. Accepted layout: one folder per gesture class such as `up/`, `ok/`, `thumb/`, or any custom names. You can include **more than six** classes — collect extras or replace gestures freely.
2. **Choose the active classes** (up to six). Your dataset can hold many classes, but each training / inference run uses the active set you tick. Click **Save active classes**. You can also capture new gesture frames (any class name) and activate them later by swapping the active set.
3. Map each active class to a robot action: `up`, `down`, `left`, `right`, `clamp`, `release`.
4. Choose TensorFlow or PyTorch and a model recipe.
5. Click **Start training** and watch the live job log.
6. Flash the model/full camera firmware shown on the page if the instructor asks you to update it.
7. For OV2640 preview, plug the ESP32-S3 main board into your PC, then click **Connect OV2640 preview**. The browser borrows the serial port, sends camera stream commands, displays incoming JPEG frames, and shows live prediction/result lines. No Python or terminal command is needed.

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

## Camera + USB Demo Page

Purpose: explore the OV2640 camera and on-board USB storage with the full control set.

Requires the **main board firmware** (flash it on the Firmware page). Plug the ESP32-S3 into **your PC**, click **Connect camera**, then use the settings: pixel format, resolution, brightness/contrast/saturation, auto exposure/gain/white balance, mirror/flip. You can capture frames to the board's flash, list them, and expose the storage to your PC as a **USB drive**. No Python or terminal needed.

## Flashing / Serial Checklist

- Use Chrome or Edge over HTTPS.
- Close Arduino Serial Monitor, `idf.py monitor`, PuTTY, or any other program using the port.
- Only one serial workflow runs at a time. After one finishes, wait for the status to say released before starting the next one — no page refresh needed.
- If auto-reset fails while flashing: hold BOOT, click connect, tap RESET/EN, release BOOT after about 1 second.
- **Flash baud:** the header has a **Flash baud** selector used for all flashing (model + firmware). Leave it at **460800**; if flashing is unreliable, lower it to **230400** or **115200** and try again. (Connecting always syncs at 115200 first, so this only affects write speed.)
- Use a data-capable USB cable.
- **Benchmark:** pick the ESP32-S3 **UART-bridge** COM port (not the USB-JTAG one) — the `READY` handshake only appears there. If the portal reports "no serial data," the port is wrong or in use; if it reports "talking but no READY," flash the deploy benchmark firmware and pick the UART-bridge port.
