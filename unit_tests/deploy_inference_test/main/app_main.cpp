#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "DEPLOY_UNIT_TEST";
static constexpr const char *MODEL_PARTITION_LABEL = "model";
static constexpr int TENSOR_ARENA_SIZE = 1024 * 1024;

static uint8_t *g_tensor_arena = nullptr;
static const void *g_mapped_model = nullptr;
static esp_partition_mmap_handle_t g_mmap_handle = 0;

static int tensor_element_count(const TfLiteTensor *tensor) {
    int count = 1;
    for (int i = 0; i < tensor->dims->size; ++i) {
        count *= tensor->dims->data[i];
    }
    return count;
}

static void print_tensor_info(const char *name, const TfLiteTensor *tensor) {
    printf("%s,type=%d,dims=[", name, tensor->type);
    for (int i = 0; i < tensor->dims->size; ++i) {
        printf("%s%d", i ? " " : "", tensor->dims->data[i]);
    }
    printf("],scale=%f,zero_point=%ld\n", (double)tensor->params.scale, (long)tensor->params.zero_point);
}

static const uint8_t *map_model_partition(size_t *mapped_size) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        MODEL_PARTITION_LABEL);
    if (partition == nullptr) {
        ESP_LOGE(TAG, "Model partition '%s' not found", MODEL_PARTITION_LABEL);
        return nullptr;
    }

    ESP_LOGI(TAG, "Model partition: label=%s offset=0x%lx size=%lu bytes",
             partition->label,
             (unsigned long)partition->address,
             (unsigned long)partition->size);

    esp_err_t err = esp_partition_mmap(partition,
                                       0,
                                       partition->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &g_mapped_model,
                                       &g_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    *mapped_size = partition->size;
    return (const uint8_t *)g_mapped_model;
}

static bool fill_synthetic_input(TfLiteTensor *input) {
    const int elements = tensor_element_count(input);
    if (elements <= 0) {
        return false;
    }

    if (input->type == kTfLiteInt8) {
        for (int i = 0; i < elements; ++i) {
            const float normalized = (float)(i % 256) / 255.0f;
            int q = (int)lroundf(normalized / input->params.scale) + input->params.zero_point;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            input->data.int8[i] = (int8_t)q;
        }
        return true;
    }
    if (input->type == kTfLiteUInt8) {
        for (int i = 0; i < elements; ++i) {
            const float normalized = (float)(i % 256) / 255.0f;
            int q = (int)lroundf(normalized / input->params.scale) + input->params.zero_point;
            if (q < 0) q = 0;
            if (q > 255) q = 255;
            input->data.uint8[i] = (uint8_t)q;
        }
        return true;
    }
    if (input->type == kTfLiteFloat32) {
        for (int i = 0; i < elements; ++i) {
            input->data.f[i] = (float)(i % 256) / 255.0f;
        }
        return true;
    }

    ESP_LOGE(TAG, "Unsupported input tensor type: %d", input->type);
    return false;
}

static int output_argmax(const TfLiteTensor *output) {
    const int elements = tensor_element_count(output);
    int best = 0;
    if (output->type == kTfLiteInt8) {
        for (int i = 1; i < elements; ++i) {
            if (output->data.int8[i] > output->data.int8[best]) best = i;
        }
    } else if (output->type == kTfLiteUInt8) {
        for (int i = 1; i < elements; ++i) {
            if (output->data.uint8[i] > output->data.uint8[best]) best = i;
        }
    } else if (output->type == kTfLiteFloat32) {
        for (int i = 1; i < elements; ++i) {
            if (output->data.f[i] > output->data.f[best]) best = i;
        }
    }
    return best;
}

static void print_output_scores(const TfLiteTensor *output) {
    const int elements = tensor_element_count(output);
    printf("SCORES");
    for (int i = 0; i < elements; ++i) {
        if (output->type == kTfLiteInt8) {
            printf(",%d", (int)output->data.int8[i]);
        } else if (output->type == kTfLiteUInt8) {
            printf(",%u", (unsigned)output->data.uint8[i]);
        } else if (output->type == kTfLiteFloat32) {
            printf(",%.6f", (double)output->data.f[i]);
        }
    }
    printf("\n");
}

extern "C" void app_main(void) {
    printf("READY,DEPLOY_INFERENCE_TEST\n");

    size_t mapped_size = 0;
    const uint8_t *model_data = map_model_partition(&mapped_size);
    if (model_data == nullptr) {
        printf("ERROR,model_partition_missing\n");
        return;
    }
    ESP_LOGI(TAG, "Mapped TFLite model partition size: %u bytes", (unsigned)mapped_size);

    const tflite::Model *model = tflite::GetModel(model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "TFLite schema mismatch: model=%lu expected=%d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        printf("ERROR,tflite_schema_mismatch\n");
        return;
    }

    g_tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_tensor_arena == nullptr) {
        ESP_LOGW(TAG, "PSRAM arena allocation failed; trying internal RAM");
        g_tensor_arena = (uint8_t *)heap_caps_malloc(256 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (g_tensor_arena == nullptr) {
        printf("ERROR,tensor_arena_alloc_failed\n");
        return;
    }
    const int arena_size = heap_caps_get_allocated_size(g_tensor_arena);
    ESP_LOGI(TAG, "Tensor arena allocated: %d bytes", arena_size);

    static tflite::MicroMutableOpResolver<16> resolver;
    resolver.AddAdd();
    resolver.AddAveragePool2D();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddDequantize();
    resolver.AddFullyConnected();
    resolver.AddMaxPool2D();
    resolver.AddMean();
    resolver.AddMul();
    resolver.AddPad();
    resolver.AddQuantize();
    resolver.AddRelu();
    resolver.AddReshape();
    resolver.AddSoftmax();

    static tflite::MicroInterpreter interpreter(model, resolver, g_tensor_arena, arena_size);
    if (interpreter.AllocateTensors() != kTfLiteOk) {
        printf("ERROR,allocate_tensors_failed\n");
        return;
    }

    TfLiteTensor *input = interpreter.input(0);
    TfLiteTensor *output = interpreter.output(0);
    print_tensor_info("INPUT_TENSOR", input);
    print_tensor_info("OUTPUT_TENSOR", output);

    if (!fill_synthetic_input(input)) {
        printf("ERROR,input_fill_failed\n");
        return;
    }

    const int64_t start_us = esp_timer_get_time();
    if (interpreter.Invoke() != kTfLiteOk) {
        printf("ERROR,invoke_failed\n");
        return;
    }
    const int64_t latency_us = esp_timer_get_time() - start_us;
    const int pred = output_argmax(output);
    printf("RESULT,pred=%d,latency_us=%lld\n", pred, (long long)latency_us);
    print_output_scores(output);
}
