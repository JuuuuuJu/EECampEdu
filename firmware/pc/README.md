# PC Tools

PC-side code is used for model verification, benchmark, dataset handling, and
deploy-side quantization on the AI PC.

## Layout

```text
benchmark/      ESP benchmark and PC TFLite reference comparison
model_pipeline/ Model-finetune handoff contract and examples
tools/          debugging and model validation helpers
artifacts/      generated local files; ignored by git
```

Expected artifacts:

```text
artifacts/models/*_int8.tflite
artifacts/datasets/
artifacts/reports/
```

## Requirements

`requirements.txt` is the single PC dependency file for deploy integration,
benchmark transport, image conversion, flashing tools, and PC-side TFLite
reference inference. Install it through the Windows conda setup script:

```powershell
cd EECampEdu
python firmware\scripts\setup_pc_env.py
conda activate eecampedu
```

The default environment uses Python 3.10 for native Windows TensorFlow
compatibility.

## Quantize Keras Source Model

The current deploy pipeline reads `.keras` source models, not float32 `.tflite`
files. Default source models are expected under:

```text
model_finetune/models/
```

Generate the int8 deploy model:

```powershell
cd EECampEdu
python firmware\pc\tools\quantize_keras_model.py --model-name Separable_CNN
```

Useful alternatives:

```powershell
python firmware\pc\tools\quantize_keras_model.py --model-name Baseline_CNN
python firmware\pc\tools\quantize_keras_model.py --keras model_finetune\models\Separable_CNN.keras --model-name Separable_CNN
```
