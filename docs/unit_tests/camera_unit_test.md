# Camera Unit Test

**Goal**: Verify that the OV2640 camera initializes, captures frames, writes them to the FATFS flash storage partition, and provides the grayscale frame buffer required for on-device TFLite Micro model inference.

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 Camera | Yes |
| ESP32-S3 Board | Yes |
| Deploy Model | Optional (A dummy/fallback inference runs if the model partition is empty) |
| Robotic Arm | Optional (Servos will move if `ENABLE_ROBOT_ARM_OUTPUT` is set to `true`) |
| PC Camera Controller | Yes (Required for live preview mode testing) |

---

## Mode 1: Firmware Mode For Camera-Only Storage Test (`kCameraFlash`)

In this mode, the firmware performs continuous local camera capture (once per second), captures grayscale QVGA frames, writes them to flash storage, and runs model inference directly.

### Configuration
Set the runtime mode in [model_config.hpp](../../firmware/esp/main/include/model_config.hpp):
```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraFlash;
```

### Build and Flash
```powershell
cd firmware\esp
idf.py build
idf.py -p <UART PORT> flash monitor
```

### Expected Log Output
```text
I (720) PHOTO_STORAGE: Stored latest photo: 19200 bytes, 160x120, format=0.
I (730) TFLM_GESTURE: DEVICE_FRAME_DUMP
...
I (760) TFLM_GESTURE: DEVICE_MODEL_DUMP
...
RESULT,4,20456,1234,21690,0,0,0,0,255
```

In `kCameraFlash`, the firmware should NOT print:
```text
Camera stream task started.
Captured format is not grayscale.
```

---

## Mode 2: Firmware Mode For Live Preview & Software Inference (`kCameraUsbMsc`)

In this mode, the firmware runs the TinyUSB CDC stream and MSC storage stack concurrently, allowing a host PC controller to stream live preview frames and trigger CV inference.

### Configuration
Set the runtime mode in [model_config.hpp](../../firmware/esp/main/include/model_config.hpp):
```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

### Build and Run PC Controller
1. Flash the ESP32-S3 firmware (using the USB-to-UART port).
2. Connect the host PC to the **native USB OTG port** (usually labeled **"USB"** or **"OTG"**) on the ESP32-S3 board.
3. Launch the python-based PC controller:
   ```powershell
   cd firmware\pc\tools
   python camera_controller.py
   ```
3. In the GUI window, press `d1` to start the stream. You should see a smooth live video preview (VGA JPEG format).
4. Press `c` (or the spacebar) to trigger the capture and inference workflow.

### Expected Log Output (ESP32 Monitor)
When you press `c` on the PC tool, the ESP32 console should log:
```text
[System] Image saved to flash! File: /usb/img_20260712_225022_fmt4_w640_h480.jpg
[System] Inference started on live capture.
RESULT,4,20456,34522,54978,0,0,0,0,255
```
*Note*:
* `fmt4` indicates `PIXFORMAT_JPEG` (JPEG image format from the camera).
* The ESP32 uses an on-device software decoder (`decode_jpeg_to_high_fidelity_grayscale`) to convert this JPEG to grayscale and downscales it to 96x96 for model inference.
* The processed inference input frame is saved to `/usb/latest.raw`, `/usb/latest.meta`, and `/usb/latest.bmp`.

---

## Core Commands Testing

The majority of command interactions occur in the PC host controller terminal (running `camera_controller.py`). Please test the following core commands:

1. **`format` (Format Flash Drive)**:
   * **Action**: Type `format` and press Enter.
   * **Expectation**: The ESP32 console will log `[System] Formatting storage partition... please wait...`, erase the partition range, and trigger a clean restart to rebuild the FAT table. The PC tool will print `[Python UI] ESP32 is rebooting. Shutting down controller...` and exit cleanly.
   
2. **`w` (Write Image to Flash)**:
   * **Action**: Type `w` and press Enter.
   * **Expectation**: Captures a frame and saves it to the flash storage as `img_[timestamp]...`. It will automatically remount the flash partition to the PC once the write is complete.
   
3. **`l` (List Files)**:
   * **Action**: Type `l` and press Enter.
   * **Expectation**: Prints the filenames, sizes, and total usage of all images stored in the flash drive, then remounts the partition back to the PC.
   
4. **`c` (Capture & Run Inference)**:
   * **Action**: Type `c` (or press spacebar in the GUI window).
   * **Expectation**: Safely pauses streaming, captures a camera frame, saves it as a timestamped file `/usb/img_[timestamp]...`, decodes it to grayscale, downscales to 96x96, writes to the Ghost files (`latest.raw`, `latest.meta`, `latest.bmp`), runs on-device model inference, remounts to the PC, and resumes streaming.
   
5. **`i <idx/filename>` (Offline File Inference)**:
   * **Action**: Type `i 0` (or the specific file name/index).
   * **Expectation**: Reads the selected image file from the flash drive, decodes the format in software, scales to 96x96, writes to `latest.*` Ghost files, executes inference, logs the `RESULT` scores, remounts back to the PC, and resumes streaming.

---

## Pass Criteria

- **Successful Initialization**: The OV2640 camera initializes without any errors.
- **Successful Storage**: Files `/usb/latest.raw`, `/usb/latest.meta`, and `/usb/latest.bmp` are written successfully and have the correct format IDs (format `0` for Grayscale, format `3` for JPEG metadata).
- **Responsive Inference**: In `kCameraFlash`, inference runs periodically. In `kCameraUsbMsc`, sending a capture command triggers software decoding, resizing, and outputs the `RESULT` string within `100ms`.
- **Reliable Storage Access**: The host PC can mount and read the files from the `/usb` drive without filesystem corruption.
