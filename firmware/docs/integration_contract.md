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

Photo flash test mode:

```text
PC image file
  -> esp/flash_photo.py
  -> photos partition
  -> ESP reads latest stored grayscale frame
  -> ESP crop/resize
  -> 96 x 96 x 1 int8 model input
```

## Flash Contract

| Partition | Size | Purpose |
| --- | ---: | --- |
| `factory` | 3 MB | firmware app |
| `model` | 1 MB | active `.tflite` model |
| `photos` | remaining flash from `0x410000` | captured image / future storage area |

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
