# PC Tools

PC-side code is used for model verification, benchmark, dataset handling, and deploy-side quantization on the AI PC.

## Layout

```text
benchmark/       ESP benchmark and PC TFLite reference comparison
model_pipeline/  Model-finetune handoff contract and examples
tools/           debugging/model helpers plus camera_controller.py for USB CDC preview
artifacts/       generated local files; ignored by git
```

The desktop UI is intentionally outside firmware at:

```text
../../apps/esp32_cam_input_app/
```

## Requirements

`requirements.txt` is the single Python dependency file for deploy integration, benchmark transport, image conversion, flashing tools, and PC-side TFLite reference inference. Install it through the Windows conda setup script from the repository root:

```powershell
python scripts\\setup_env.py
conda activate eecampedu
```

The setup script also installs native build packages for the top-level ImGui app (`cmake`, `ninja`, `sdl3`). Dear ImGui is fetched by CMake, not vendored into this repository.

## Quantize Keras Source Model

The current deploy pipeline reads `.keras` source models, not float32 `.tflite` files. Default source models are expected under:

```text
model_finetune/models/
```

Generate the int8 deploy model:

```powershell
cd EECampEdu
python firmware\pc\tools\quantize_keras_model.py --model-name Separable_CNN
```

## USB Camera Controller

`tools/camera_controller.py` talks to the ESP32-S3 TinyUSB CDC port, receives base64 image frames for live preview, and sends camera commands such as capture, streaming, format, and resolution changes. USB MSC storage can expose the FAT storage partition to the host PC for file inspection.
