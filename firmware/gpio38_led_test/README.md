# GPIO38 LED Test Firmware

Standalone ESP-IDF project for testing a single LED on GPIO38. This is separate from ESP1 and ESP2 firmware.

## Build And Flash

```powershell
cd firmware\gpio38_led_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Serial Commands

```text
on      turn GPIO38 LED on
off     turn GPIO38 LED off
toggle  toggle LED state
blink   blink LED every 500 ms
stop    stop blinking and turn LED off
status  print current state
help    print commands
```

If the LED wiring is active-low, change `LED_ACTIVE_HIGH` in `main/app_main.c` from `1` to `0`.
