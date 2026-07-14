# Deploy Inference Unit Test

Independent ESP-IDF project for the deploy group. It tests only:

1. external TFLite model stored in the `model` flash partition,
2. `esp_partition_mmap`,
3. TFLite Micro `AllocateTensors`,
4. one synthetic-input `Invoke()`.

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
- `RESULT,pred=<class>,latency_us=<time>`
- `SCORES,...`

This verifies deploy runtime health. It is not an accuracy benchmark because the input is synthetic.
