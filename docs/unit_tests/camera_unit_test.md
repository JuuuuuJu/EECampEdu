# Camera Unit Test

目標：確認 OV2640 可以初始化、拍照、寫入 storage，並提供 firmware 推論需要的 grayscale frame。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | Yes |
| ESP32-S3 | Yes |
| Deploy model | Optional |
| Robotic arm | No |
| PC camera | No |

## Firmware Mode For Camera-Only Storage Test

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraFlash;
```

Build and flash:

```powershell
cd firmware\esp
idf.py build
idf.py -p COM6 flash monitor
```

## Expected Log

```text
CAMERA_CAPTURE: OV2640 camera initialized.
PHOTO_STORAGE: Stored latest photo
DEVICE_FRAME_DUMP
CROP
DEVICE_MODEL_DUMP
RESULT
```

In `kCameraFlash`, the firmware should not print:

```text
Camera stream task started.
Captured format is not grayscale.
```

If `/usb/latest.meta` cannot be opened, run the USB storage format command once in USB mode or check whether the storage partition is currently mounted by the PC.

## Firmware Mode For Live Preview

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

Use the PC app or camera controller to request preview frames over USB CDC.

## Pass Criteria

- OV2640 initializes.
- Captured frame has expected resolution and format.
- Storage write succeeds.
- Crop output is stable.
- Grayscale inference path receives resized `96 x 96 x 1` input.

