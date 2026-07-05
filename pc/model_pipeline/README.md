# Model Pipeline Handoff

This folder defines what deploy expects from the model-finetune pipeline. The
training team can choose the internal framework and foundation model strategy,
as long as the exported artifacts follow this contract.

## Expected Workflow On AMD AI PC

```text
foundation model
  -> student-defined gesture dataset
  -> fine-tune
  -> int8 quantization
  -> export deploy artifacts
  -> flash .tflite to ESP32-S3 model partition
```

## Required Output Artifacts

```text
artifacts/models/<model_name>.tflite
artifacts/models/<model_name>.class_mapping.json
artifacts/models/<model_name>.preprocess_config.json
artifacts/reports/<model_name>.quantization_report.json
```

## Deploy Compatibility Contract

| Field | Required value |
| --- | --- |
| Input shape | `1 x 96 x 96 x 1` |
| Input type | `int8` |
| Output shape | `1 x 5` |
| Output type | `int8` |
| Class order | `up`, `down`, `right`, `left`, `null` |
| Input semantic | grayscale hand image after ESP-side crop/resize |

If the model team changes any of these fields, deploy must update firmware and
benchmark tools before flashing the model.

## Recommended Foundation Model Assumption

Use a small grayscale gesture foundation model as the base, not a large RGB
general vision model. The ESP32-S3 deploy target is optimized for:

- low-resolution grayscale input
- small int8 CNN/TFLite Micro models
- operator sets already registered in firmware
- fast local inference under tight RAM constraints

Students can define custom gestures by collecting a dataset with the same input
preprocessing contract, then fine-tuning the final classifier or compact CNN
head before quantization.
