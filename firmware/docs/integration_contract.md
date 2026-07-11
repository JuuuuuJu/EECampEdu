# Integration Contract

## Model Contract

The default deploy model is generated from
`model_finetune/models/Separable_CNN.keras` by deploy-side calibration
quantization. The flashed model is `Separable_CNN_int8.tflite`.

| Field | Value |
| --- | --- |
| Input shape | `1 x 96 x 96 x 1` |
| Input type | `int8` |
| Input source | ESP grayscale/crop/resize output |
| Output shape | `1 x 5` |
| Output type | `int8` |
| Class order | `0=up`, `1=down`, `2=right`, `3=left`, `4=null` |

Changing input shape, output class count, class order, preprocessing, or
operators requires firmware validation and possibly code changes.

Deploy quantization source contract:

```text
.keras source model
  -> representative calibration images
  -> int8 input / int8 output TFLite
  -> flash model partition
```

Float32 `.tflite` and prebuilt int8 `.tflite` files are not used as source
models in the current deploy pipeline.

## Image Contract

The deploy pipeline expects the model runner to receive a grayscale frame before
crop. Supported camera sources should normalize into one of these internal
formats:

| Source format | First integration support | Required conversion |
| --- | --- | --- |
| Grayscale raw | Yes | direct crop/resize |
| RGB565 | Later | RGB565 -> grayscale |
| YUV422 | Later | Y channel or YUV -> grayscale |
| JPEG | Later | JPEG decode -> RGB565/grayscale |

Default test mode:

```text
PC benchmark
  -> 160 x 160 x 1 uint8 grayscale frame over UART
  -> ESP crop/resize
  -> 96 x 96 x 1 int8 model input
```

Legacy photo flash test mode:

```text
PC image file
  -> esp/flash_photo.py
  -> raw flash test storage
  -> ESP reads latest stored grayscale frame
  -> ESP crop/resize
  -> 96 x 96 x 1 int8 model input
```

## Flash Contract

| Partition | Size | Purpose |
| --- | ---: | --- |
| `factory` | 3 MB | firmware app |
| `model` | 1 MB | active `.tflite` model |
| `storage` | remaining flash from `0x410000` | FAT USB MSC drive for captured JPEG frames |

The current model set fits in a 1 MB model partition except larger experimental
models such as full MobileNet variants. Increase `model` if those must be
flashed directly.

## Result Contract

Firmware should expose the final inference result in this semantic form:

```text
prediction_index: int
prediction_name : up | down | right | left | null
scores          : int8[5]
preprocess_us   : int
model_us        : int
device_us       : int
```

`null` means no clear gesture or no action.

## Cross-Team Runtime Contract

1. Model team trains and exports `.keras` only. Deploy owns calibration and int8
   TFLite generation.
2. Deploy flashes firmware plus the active `.tflite` model, then runs UART
   benchmark and output-similarity checks.
3. Camera team provides OV2640 capture. USB camera mode captures VGA JPEG.
4. USB camera integration follows `0711_integration/camera_usb/EECampEdu`.
   CDC can stream base64 image payloads for live preview / UI debugging through
   `firmware/pc/tools/camera_controller.py`; MSC can expose the FAT `/usb`
   storage partition to the host PC for file inspection.
5. Input team owns the PC UI path. The integrated app is `apps/esp32_cam_input_app`, based on Dear ImGui + SDL3. It exposes the ImGui demo window, input-control placeholders, USB CDC command buttons, and a serial monitor for the TinyUSB camera firmware.
6. Output team controls the robotic arm. Firmware contains an ESP-IDF LEDC
   output module, but it is disabled until servo GPIOs no longer conflict with
   OV2640 camera pins.

Note: USB MSC is a mass-storage refresh path, not a true webcam protocol. For
silky continuous video, UVC or a dedicated bulk streaming protocol would be more
appropriate than repeatedly mounting and reading JPEG files from FAT storage.


