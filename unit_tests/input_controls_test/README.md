# Input Controls Unit Test

Independent ESP-IDF project for the input group. It tests the rotary encoder and push button only.

## Hardware

- ESP32-S3 board.
- Rotary encoder wired to PCB-assigned pins:
  - CLK: GPIO21
  - DT: GPIO47
  - SW: GPIO48
- Optional second button is disabled by default (`BUTTON2_GPIO = -1`).

## Build / Flash

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\input_controls_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Expected Result

The monitor prints `INPUT_STATUS` every 250 ms. Rotate the encoder and press the encoder switch:

- `encoder` should increase/decrease when rotating.
- `encoder_button` should increment when pressing SW.
- `clk`, `dt`, and `sw_level` show live GPIO levels.
