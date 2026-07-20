# Student Guide — EECampEdu Portal

Everything happens in your **browser**. Use **Chrome or Edge**.

## Open the portal

Open your team's address (ask your instructor for the exact one), e.g.:

```
https://140.112.194.42:8081
```

If you see a certificate warning, click **Advanced → Proceed** once. The portal
has four tabs — one per class day.

---

## Output demo (GPIO / LED / PWM)

**Hardware:** ESP32-S3 board with its onboard LED, plugged into your PC's USB.

1. **Flash output demo firmware** → *Connect & flash output firmware*, pick the
   board in the browser's port dialog, wait for “succeeded”.
2. **Connect for control**, then:
   - **LED ON / LED OFF** — digital output.
   - **Brightness slider** — PWM brightness (0–255); drag it and watch the LED.
   - **Blink** — repeating blink.

*Expect:* the LED turns on/off and dims/brightens as you drag the slider.

---

## Model demo (PC webcam)

**Hardware:** none — just your PC **webcam**.

The demo uses your local camera, so it runs on your PC (not in the browser).
Copy the command from the **Model demo** tab and run it in a terminal:

```
conda activate eecampedu
python model_finetune/pytorch/webcam_demo.py
```

*Expect:* a webcam window that shows the predicted gesture live.

---

## Deploy & benchmark (ESP32-S3)

**Hardware:** ESP32-S3 main board, plugged into your PC's USB.

1. (If needed) **Quantize** a trained model → produces a `.tflite`.
2. **Flash model** → *Connect ESP32-S3* → *Flash model*. This updates only the
   model — not the firmware.
3. **Run benchmark** → pick the model, dataset, and serial port, then click
   **Run benchmark**. The live log appears in *Live job log*, and a summary
   (images processed, label accuracy, latencies, throughput/FPS, and score
   similarity — Top-1, MAE, Max Error, Cosine) shows in the Results table.

*Expect:* “Flash succeeded — model updated”, and benchmark numbers in the table.

> The benchmark runs on the **AI PC server**, so the board must be plugged into
> the **AI PC** for it. If your board is on your **own PC**, the AI PC can't reach
> it — from your PC you can still do **Flash model** in the browser, but not this
> benchmark. The portal tells you when no AI-PC board is detected.

---

## Main board firmware

Use this once to put the full main board firmware (camera + inference) on a new
board: **Connect & flash firmware** → pick the board → wait for “succeeded”.
This is separate from flashing a model.

---

## If flashing doesn't connect

- Use **Chrome or Edge** (Web Serial is not in Firefox/Safari).
- The portal must be **https://** (or the instructor enabled it for your PC).
- Close any other serial monitor (Arduino IDE, `idf.py monitor`, PuTTY).
- Hold **BOOT**, click connect, tap **RESET/EN**, release BOOT after ~1s.
- Use a **data** USB cable (not charge-only).
