# USB Unit Test

目標：確認 USB CDC command channel 和 USB MSC storage 都能工作。

## Requirements

| Item | Needed |
| --- | --- |
| OV2640 | For live preview: Yes. For command-only: No |
| ESP32-S3 | Yes |
| Deploy model | Optional |
| Robotic arm | No |
| PC camera | No |

## Firmware Mode

Set:

```cpp
constexpr RuntimeMode RUNTIME_MODE = RuntimeMode::kCameraUsbMsc;
```

Build and flash:

```powershell
cd firmware\esp
idf.py build
idf.py -p COM6 flash monitor
```

## CDC Test

Open a serial terminal on the ESP32-S3 CDC port and send commands:

```text
D 0       disable continuous stream
C         capture one frame
W         write latest frame to /usb
L         list /usb files
usb       expose storage as USB MSC
format    format FAT storage and reboot
```

## MSC Test

After sending:

```text
usb
```

The PC should detect the ESP32-S3 storage partition as a removable drive.

Expected files after capture/write:

```text
latest.raw
latest.meta
```

## Live Preview Note

Live preview is CDC frame streaming, not UVC. The PC app reconstructs each JPEG frame from CDC messages, so it behaves like video but is still frame-by-frame transport.

## Pass Criteria

- CDC commands receive text responses.
- MSC drive appears on PC.
- Stored photo metadata and payload are visible.
- CDC streaming does not corrupt command parsing.

