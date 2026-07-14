# Deploy Inference Unit Test

Independent ESP-IDF project for the deploy group. It tests only:

1. external int8 TFLite model stored in the `model` flash partition,
2. `esp_partition_mmap`,
3. TFLite Micro `AllocateTensors`,
4. model-only `Invoke()` latency and throughput.

It does not use OV2640, USB preview, input controls, or output servos.

## Build / Flash Firmware

```powershell
cd D:\0711_integration\EECampEdu\unit_tests\deploy_inference_test
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Flash Model Partition

This project expects an int8 `.tflite` model in the `model` partition at `0x310000`.

```powershell
esptool --chip esp32s3 -p COMx -b 460800 write-flash 0x310000 D:\0711_integration\EECampEdu\firmware\pc\artifacts\models\Mini_ResNet_finetuned_96_int8.tflite
```

## Expected Result

Monitor output should include:

- `READY,DEPLOY_INFERENCE_TEST`
- `INPUT_TENSOR,...`
- `OUTPUT_TENSOR,...`
- warm-up `RESULT,pred=<class>,latency_us=<time>,warmup=1`
- `SCORES,...`
- `--- Deploy Inference Benchmark ---`
- `Average Latency`, `Min Latency`, `Max Latency`, `Model Throughput`
- `BENCHMARK_RESULT,iterations=50,avg_us=...,min_us=...,max_us=...,fps=...,pred=...`

Example metric meaning:

- `Average Latency`: average TFLite Micro `Invoke()` time across 50 iterations.
- `Model Throughput`: model-only FPS, computed from average inference latency.
- `BENCHMARK_RESULT`: machine-readable line for reports or scripts.

This verifies deploy runtime health and inference speed. It is not an accuracy benchmark because the input is synthetic.
