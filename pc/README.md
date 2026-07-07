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

`requirements.txt` is the single PC dependency file for deploy integration,
benchmark transport, image conversion, flashing tools, and PC-side TFLite
reference inference. Install it through the Windows conda setup script:

```powershell
cd D:\0706\EECampEdu
python scripts\setup_pc_env.py
conda activate eecampedu
```

The default environment uses Python 3.10 for native Windows TensorFlow
compatibility.
