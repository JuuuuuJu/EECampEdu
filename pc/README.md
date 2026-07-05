# PC Tools

PC-side code is used for model verification, benchmark, dataset handling, and
future fine-tuning automation on the AI PC.

## Layout

```text
benchmark/      ESP benchmark and PC TFLite reference comparison
model_pipeline/ Model-finetune handoff contract and examples
tools/          debugging and model validation helpers
artifacts/      generated local files; ignored by git
```

Expected artifacts:

```text
artifacts/models/*.tflite
artifacts/datasets/
artifacts/reports/
```

## Requirements

`requirements.txt` is the default modern environment for deploy integration,
benchmark transport, image conversion, and flashing tools.

`requirements-legacy-onnx.txt` keeps the old pinned ONNX debug stack. Use it
only with Python 3.7-3.10 when those legacy ONNX scripts are needed.
