# Model Fine-Tune Reference

Integrated source:

```text
../model_finetune
```

Important local models:

| Model | Size | Status |
| --- | ---: | --- |
| `Separable_CNN_int8.tflite` | ~99 KB | current default |
| `Baseline_CNN_int8.tflite` | ~102 KB | compatible candidate |
| `Mini_ResNet_int8.tflite` | ~121 KB | requires enough tensor arena |
| `MobileNetV1_0.25_int8.tflite` | ~308 KB | compatible candidate |
| `MobileNetV2_0.35_int8.tflite` | ~630 KB | compatible candidate, heavier |

Integration output expected from model-finetune:

```text
model.tflite
class_mapping.json
preprocess_config.json
benchmark_report.json
```

The deploy contract is fixed to `96 x 96 x 1 int8` input and
`up/down/right/left/null` output order unless all teams agree to change it.
