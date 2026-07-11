# Output Team Reference

Reference source:

```text
robotic_arm.ino
```

The Arduino sketch used `ESP32Servo`. It has been ported into ESP-IDF as:

```text
esp/main/include/output_controls.hpp
esp/main/src/output_controls.cpp
```

Current status:

- Disabled by default with `ENABLE_ROBOT_ARM_OUTPUT=false`.
- The original base servo pin `GPIO18` conflicts with the current OV2640 `Y7`
  camera pin, so the pin map must be finalized before enabling output on the
  integrated board.
- Gesture mapping is currently:
  - `up`: pitch servo up
  - `down`: pitch servo down
  - `right`: base servo right
  - `left`: base servo left
  - `null`: no movement
