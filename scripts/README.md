# Scripts

Suggested script entry points:

```text
setup_pc_env.sh       install PC benchmark/fine-tune dependencies
build_firmware.sh     run ESP-IDF build
flash_firmware.sh     flash ESP firmware
flash_model.sh        flash only the TFLite model partition
flash_photo.sh        convert an image and flash it into the photos partition
run_benchmark.sh      run benchmark against ESP32-S3
```

Keep scripts thin wrappers around the documented commands.
