#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "camera_capture.hpp"
#include "model_config.hpp"
#include "photo_storage.hpp"

static const char *TAG = "TFLM_GESTURE";
static constexpr int kExpectedTfliteSchemaVersion = 3;

static constexpr int kClassCount = 5;
static constexpr const char *kClassNames[kClassCount] = {
    "up",
    "down",
    "right",
    "left",
    "null",
};

static uint8_t *g_raw_frame = nullptr;
static uint8_t *g_model_frame = nullptr;
static uint8_t *g_tensor_arena = nullptr;
static int g_tensor_arena_size = 0;
static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;
static const void *g_mapped_model = nullptr;
static esp_partition_mmap_handle_t g_model_mmap_handle = 0;

static const char *runtime_mode_name() {
    switch (RUNTIME_MODE) {
        case RuntimeMode::kTestUartFrame:
            return "TEST_MODE_UART_FRAME";
        case RuntimeMode::kPhotoFlashTest:
            return "PHOTO_FLASH_TEST_MODE";
        case RuntimeMode::kCameraFlash:
            return "CAMERA_FLASH_MODE";
    }
    return "UNKNOWN";
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

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
        printf("%s%d", i == 0 ? "" : " ", tensor->dims->data[i]);
    }
    printf("],scale=%f,zero_point=%ld\n",
           (double)tensor->params.scale,
           (long)tensor->params.zero_point);
}

static void log_memory_status() {
#ifdef CONFIG_SPIRAM
    ESP_LOGI(TAG,
             "CONFIG_SPIRAM enabled. PSRAM total=%u free=%u largest=%u; internal free=%u largest=%u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
    ESP_LOGW(TAG,
             "CONFIG_SPIRAM disabled. Larger models may fail unless they fit in internal RAM.");
#endif
}

static void dump_frame(const char *name, const uint8_t *frame, int width, int height) {
    const int total = width * height;
    const int center_idx = (height / 2) * width + (width / 2);
    uint64_t sum = 0;
    for (int i = 0; i < total; ++i) {
        sum += frame[i];
    }

    printf("%s,first10=[", name);
    for (int i = 0; i < 10 && i < total; ++i) {
        printf("%s%u", i == 0 ? "" : " ", (unsigned)frame[i]);
    }
    printf("],center[%d]=%u,sum=%llu\n",
           center_idx,
           (unsigned)frame[center_idx],
           (unsigned long long)sum);
}

static bool is_foreground_pixel(uint8_t pixel, int threshold) {
    return pixel < threshold;
}

static bool find_hand_crop_box(const uint8_t *frame,
                               int *crop_x0,
                               int *crop_y0,
                               int *crop_x1,
                               int *crop_y1,
                               int *best_area_out,
                               int *threshold_out) {
    const int total = FRAME_WIDTH * FRAME_HEIGHT;
    uint64_t sum = 0;
    for (int i = 0; i < total; ++i) {
        sum += frame[i];
    }

    const int mean = (int)(sum / total);
    const int threshold = clamp_int(mean - HAND_CROP_DARK_DELTA,
                                    HAND_CROP_MIN_THRESHOLD,
                                    HAND_CROP_MAX_THRESHOLD);

    int area = 0;
    int x0 = FRAME_WIDTH - 1;
    int y0 = FRAME_HEIGHT - 1;
    int x1 = 0;
    int y1 = 0;

    for (int y = 0; y < FRAME_HEIGHT; ++y) {
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            if (!is_foreground_pixel(frame[y * FRAME_WIDTH + x], threshold)) {
                continue;
            }
            ++area;
            if (x < x0) {
                x0 = x;
            }
            if (x > x1) {
                x1 = x;
            }
            if (y < y0) {
                y0 = y;
            }
            if (y > y1) {
                y1 = y;
            }
        }
    }

    const int min_area = total * HAND_CROP_MIN_AREA_PERCENT / 100;
    const bool found = area >= min_area;
    if (!found) {
        x0 = 0;
        y0 = 0;
        x1 = FRAME_WIDTH - 1;
        y1 = FRAME_HEIGHT - 1;
    }

    const int crop_w = x1 - x0 + 1;
    const int crop_h = y1 - y0 + 1;
    const int margin_x = crop_w * HAND_CROP_MARGIN_PERCENT / 100;
    const int margin_y = crop_h * HAND_CROP_MARGIN_PERCENT / 100;

    *crop_x0 = clamp_int(x0 - margin_x, 0, FRAME_WIDTH - 1);
    *crop_y0 = clamp_int(y0 - margin_y, 0, FRAME_HEIGHT - 1);
    *crop_x1 = clamp_int(x1 + margin_x, 0, FRAME_WIDTH - 1);
    *crop_y1 = clamp_int(y1 + margin_y, 0, FRAME_HEIGHT - 1);
    *best_area_out = area;
    *threshold_out = threshold;
    return found;
}

static void resize_crop_to_model_input(const uint8_t *src,
                                       uint8_t *dst,
                                       int x0,
                                       int y0,
                                       int x1,
                                       int y1) {
    const int crop_w = x1 - x0 + 1;
    const int crop_h = y1 - y0 + 1;
    for (int oy = 0; oy < INPUT_HEIGHT; ++oy) {
        const int sy_fp = (INPUT_HEIGHT == 1) ? 0 : (oy * (crop_h - 1) * 1024) / (INPUT_HEIGHT - 1);
        const int sy = sy_fp >> 10;
        const int fy = sy_fp & 1023;
        const int sy1 = (sy + 1 < crop_h) ? sy + 1 : sy;
        for (int ox = 0; ox < INPUT_WIDTH; ++ox) {
            const int sx_fp = (INPUT_WIDTH == 1) ? 0 : (ox * (crop_w - 1) * 1024) / (INPUT_WIDTH - 1);
            const int sx = sx_fp >> 10;
            const int fx = sx_fp & 1023;
            const int sx1 = (sx + 1 < crop_w) ? sx + 1 : sx;

            const int p00 = src[(y0 + sy) * FRAME_WIDTH + (x0 + sx)];
            const int p01 = src[(y0 + sy) * FRAME_WIDTH + (x0 + sx1)];
            const int p10 = src[(y0 + sy1) * FRAME_WIDTH + (x0 + sx)];
            const int p11 = src[(y0 + sy1) * FRAME_WIDTH + (x0 + sx1)];

            const int wx0 = 1024 - fx;
            const int wy0 = 1024 - fy;
            const int value = p00 * wx0 * wy0 +
                              p01 * fx * wy0 +
                              p10 * wx0 * fy +
                              p11 * fx * fy;
            dst[oy * INPUT_WIDTH + ox] = (uint8_t)((value + (1 << 19)) >> 20);
        }
    }
}

static bool preprocess_frame_to_model_input(const uint8_t *frame, uint8_t *model_frame) {
    if (INPUT_CHANNEL != 1 || FRAME_CHANNEL != 1) {
        ESP_LOGE(TAG, "Only grayscale input is supported by the current preprocessing path.");
        return false;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = FRAME_WIDTH - 1;
    int y1 = FRAME_HEIGHT - 1;
    int area = FRAME_WIDTH * FRAME_HEIGHT;
    int threshold = 0;
    bool found = false;

    if (ENABLE_HAND_CROP) {
        found = find_hand_crop_box(frame, &x0, &y0, &x1, &y1, &area, &threshold);
    }

    resize_crop_to_model_input(frame, model_frame, x0, y0, x1, y1);
    printf("CROP,%d,%d,%d,%d,%d,%d,%d\n",
           x0, y0, x1, y1, area, threshold, found ? 1 : 0);
    return true;
}

static bool fill_input_tensor_from_raw_frame(const uint8_t *frame) {
    const int expected_elements = INPUT_HEIGHT * INPUT_WIDTH * INPUT_CHANNEL;
    const int input_elements = tensor_element_count(g_input);
    if (input_elements != expected_elements) {
        ESP_LOGE(TAG, "Input size mismatch. Model expects %d elements, firmware sends %d.",
                 input_elements, expected_elements);
        return false;
    }

    if (g_input->type == kTfLiteInt8) {
        for (int i = 0; i < expected_elements; ++i) {
            const float normalized = frame[i] / 255.0f;
            const int quantized = (int)lroundf(normalized / g_input->params.scale)
                                + g_input->params.zero_point;
            g_input->data.int8[i] = (int8_t)clamp_int(quantized, -128, 127);
        }
        return true;
    }

    if (g_input->type == kTfLiteUInt8) {
        for (int i = 0; i < expected_elements; ++i) {
            const float normalized = frame[i] / 255.0f;
            const int quantized = (int)lroundf(normalized / g_input->params.scale)
                                + g_input->params.zero_point;
            g_input->data.uint8[i] = (uint8_t)clamp_int(quantized, 0, 255);
        }
        return true;
    }

    if (g_input->type == kTfLiteFloat32) {
        for (int i = 0; i < expected_elements; ++i) {
            g_input->data.f[i] = frame[i] / 255.0f;
        }
        return true;
    }

    ESP_LOGE(TAG, "Unsupported input tensor type: %d", g_input->type);
    return false;
}

static bool read_output_scores(int *scores, int *pred_index) {
    const int output_elements = tensor_element_count(g_output);
    if (output_elements < kClassCount) {
        ESP_LOGE(TAG, "Output has only %d elements, expected at least %d.",
                 output_elements, kClassCount);
        return false;
    }

    int best_score = INT32_MIN;
    int best_index = 0;
    for (int i = 0; i < kClassCount; ++i) {
        int score = 0;
        if (g_output->type == kTfLiteInt8) {
            score = g_output->data.int8[i];
        } else if (g_output->type == kTfLiteUInt8) {
            score = g_output->data.uint8[i];
        } else if (g_output->type == kTfLiteFloat32) {
            score = (int)lroundf(g_output->data.f[i] * 1000000.0f);
        } else {
            ESP_LOGE(TAG, "Unsupported output tensor type: %d", g_output->type);
            return false;
        }

        scores[i] = score;
        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }

    *pred_index = best_index;
    return true;
}

static const uint8_t *map_tflite_model_from_flash(size_t *mapped_size) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        MODEL_PARTITION_LABEL);
    if (partition == nullptr) {
        ESP_LOGE(TAG, "Model partition '%s' not found.", MODEL_PARTITION_LABEL);
        return nullptr;
    }

    ESP_LOGI(TAG, "Model partition: label=%s offset=0x%lx size=%lu bytes",
             partition->label,
             (unsigned long)partition->address,
             (unsigned long)partition->size);

    esp_err_t err = esp_partition_mmap(
        partition,
        0,
        partition->size,
        ESP_PARTITION_MMAP_DATA,
        &g_mapped_model,
        &g_model_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        return nullptr;
    }

    if (mapped_size != nullptr) {
        *mapped_size = partition->size;
    }
    return static_cast<const uint8_t *>(g_mapped_model);
}

static bool init_tflite_micro() {
    size_t mapped_size = 0;
    const uint8_t *model_data = map_tflite_model_from_flash(&mapped_size);
    if (model_data == nullptr) {
        return false;
    }
    ESP_LOGI(TAG, "Mapped TFLite model partition size: %u bytes", (unsigned)mapped_size);

    const tflite::Model *model = tflite::GetModel(model_data);
    if (model->version() != kExpectedTfliteSchemaVersion) {
        ESP_LOGE(TAG, "TFLite schema mismatch. Model=%lu Expected=%d",
                 (unsigned long)model->version(), kExpectedTfliteSchemaVersion);
        return false;
    }

    static tflite::MicroMutableOpResolver<16> resolver;
    if (resolver.AddConv2D() != kTfLiteOk ||
        resolver.AddDepthwiseConv2D() != kTfLiteOk ||
        resolver.AddAdd() != kTfLiteOk ||
        resolver.AddAveragePool2D() != kTfLiteOk ||
        resolver.AddMaxPool2D() != kTfLiteOk ||
        resolver.AddFullyConnected() != kTfLiteOk ||
        resolver.AddMean() != kTfLiteOk ||
        resolver.AddReshape() != kTfLiteOk ||
        resolver.AddSoftmax() != kTfLiteOk) {
        ESP_LOGE(TAG, "Failed to register TFLite Micro operators.");
        return false;
    }

    g_tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_tensor_arena_size = TENSOR_ARENA_SIZE;
    if (g_tensor_arena == nullptr) {
        ESP_LOGW(TAG,
                 "PSRAM tensor arena allocation failed (%d bytes). Trying %d-byte internal fallback.",
                 TENSOR_ARENA_SIZE,
                 FALLBACK_TENSOR_ARENA_SIZE);
        g_tensor_arena = (uint8_t *)heap_caps_malloc(FALLBACK_TENSOR_ARENA_SIZE, MALLOC_CAP_8BIT);
        g_tensor_arena_size = FALLBACK_TENSOR_ARENA_SIZE;
    }
    if (g_tensor_arena == nullptr) {
        ESP_LOGW(TAG, "heap_caps_malloc allocation failed. Trying plain malloc.");
        g_tensor_arena = (uint8_t *)malloc(FALLBACK_TENSOR_ARENA_SIZE);
        g_tensor_arena_size = FALLBACK_TENSOR_ARENA_SIZE;
    }
    if (g_tensor_arena == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate tensor arena (%d bytes).", FALLBACK_TENSOR_ARENA_SIZE);
        return false;
    }
    ESP_LOGI(TAG, "Tensor arena allocated: %d bytes.", g_tensor_arena_size);

    static tflite::MicroInterpreter interpreter(model, resolver, g_tensor_arena, g_tensor_arena_size);
    g_interpreter = &interpreter;

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG,
                 "AllocateTensors() failed with %d-byte arena. Larger models need PSRAM and TENSOR_ARENA_SIZE=%d.",
                 g_tensor_arena_size,
                 TENSOR_ARENA_SIZE);
        return false;
    }

    g_input = g_interpreter->input(0);
    g_output = g_interpreter->output(0);
    print_tensor_info("INPUT_TENSOR", g_input);
    print_tensor_info("OUTPUT_TENSOR", g_output);

    printf("CLASS_MAP,0=%s,1=%s,2=%s,3=%s,4=%s\n",
           kClassNames[0], kClassNames[1], kClassNames[2], kClassNames[3], kClassNames[4]);
    return true;
}

static bool allocate_frame_buffers() {
    const int expected_bytes = FRAME_HEIGHT * FRAME_WIDTH * FRAME_CHANNEL;
    const int model_bytes = INPUT_HEIGHT * INPUT_WIDTH * INPUT_CHANNEL;

    if (g_raw_frame == nullptr) {
        g_raw_frame = (uint8_t *)heap_caps_malloc(expected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_raw_frame == nullptr) {
            g_raw_frame = (uint8_t *)heap_caps_malloc(expected_bytes, MALLOC_CAP_8BIT);
        }
    }
    if (g_raw_frame == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate raw input frame (%d bytes).", expected_bytes);
        return false;
    }

    if (g_model_frame == nullptr) {
        g_model_frame = (uint8_t *)heap_caps_malloc(model_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_model_frame == nullptr) {
            g_model_frame = (uint8_t *)heap_caps_malloc(model_bytes, MALLOC_CAP_8BIT);
        }
    }
    if (g_model_frame == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate model input frame (%d bytes).", model_bytes);
        return false;
    }

    return true;
}

static bool run_inference_on_grayscale_frame(const uint8_t *frame) {
    dump_frame("DEVICE_FRAME_DUMP", frame, FRAME_WIDTH, FRAME_HEIGHT);
    const int64_t preprocess_start_time = esp_timer_get_time();
    if (!preprocess_frame_to_model_input(frame, g_model_frame)) {
        printf("ERROR,crop_preprocess_failed\n");
        fflush(stdout);
        return false;
    }
    if (!fill_input_tensor_from_raw_frame(g_model_frame)) {
        printf("ERROR,input_preprocess_failed\n");
        fflush(stdout);
        return false;
    }
    const int64_t preprocess_end_time = esp_timer_get_time();
    dump_frame("DEVICE_MODEL_DUMP", g_model_frame, INPUT_WIDTH, INPUT_HEIGHT);

    const int64_t start_time = esp_timer_get_time();
    const TfLiteStatus invoke_status = g_interpreter->Invoke();
    const int64_t end_time = esp_timer_get_time();
    if (invoke_status != kTfLiteOk) {
        printf("ERROR,invoke_failed\n");
        fflush(stdout);
        return false;
    }

    int scores[kClassCount] = {0};
    int pred_index = 0;
    if (!read_output_scores(scores, &pred_index)) {
        printf("ERROR,output_read_failed\n");
        fflush(stdout);
        return false;
    }

    const int64_t preprocess_us = preprocess_end_time - preprocess_start_time;
    const int64_t invoke_us = end_time - start_time;
    const int64_t device_compute_us = preprocess_us + invoke_us;
    printf("RESULT,%d,%lld,%lld,%lld",
           pred_index,
           (long long)invoke_us,
           (long long)preprocess_us,
           (long long)device_compute_us);
    for (int i = 0; i < kClassCount; ++i) {
        printf(",%d", scores[i]);
    }
    printf("\n");
    fflush(stdout);
    return true;
}

static void resize_grayscale_to_runtime_frame(const uint8_t *src,
                                              int src_width,
                                              int src_height,
                                              uint8_t *dst) {
    for (int y = 0; y < FRAME_HEIGHT; ++y) {
        const int src_y = (FRAME_HEIGHT == 1) ? 0 : (y * (src_height - 1)) / (FRAME_HEIGHT - 1);
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            const int src_x = (FRAME_WIDTH == 1) ? 0 : (x * (src_width - 1)) / (FRAME_WIDTH - 1);
            dst[y * FRAME_WIDTH + x] = src[src_y * src_width + src_x];
        }
    }
}

static void uart_test_inference_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "TFLite Micro inference task initialized in TEST_MODE_UART_FRAME.");
    ESP_LOGI(TAG, "Waiting for %dx%dx%d grayscale frames from PC benchmark over UART.",
             FRAME_WIDTH,
             FRAME_HEIGHT,
             FRAME_CHANNEL);
    uart_driver_install(UART_NUM_0, 1024 * 32, 0, 0, NULL, 0);

    const int expected_bytes = FRAME_HEIGHT * FRAME_WIDTH * FRAME_CHANNEL;
    if (!allocate_frame_buffers()) {
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        printf("READY\n");
        fflush(stdout);

        int rx_bytes = 0;
        while (rx_bytes < expected_bytes) {
            const int len = uart_read_bytes(UART_NUM_0,
                                            g_raw_frame + rx_bytes,
                                            expected_bytes - rx_bytes,
                                            pdMS_TO_TICKS(1000));
            if (len > 0) {
                rx_bytes += len;
            } else if (rx_bytes == 0) {
                printf("READY\n");
                fflush(stdout);
            }
        }

        run_inference_on_grayscale_frame(g_raw_frame);
    }
}

static void photo_flash_test_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "TFLite Micro inference task initialized in PHOTO_FLASH_TEST_MODE.");
    ESP_LOGI(TAG, "Reading one preloaded grayscale photo from the '%s' partition.", PHOTOS_PARTITION_LABEL);

    if (!allocate_frame_buffers()) {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = photo_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Photo storage init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    StoredPhotoMetadata metadata = {};
    err = photo_storage_read_latest(g_raw_frame,
                                    FRAME_HEIGHT * FRAME_WIDTH * FRAME_CHANNEL,
                                    &metadata);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Photo read failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Use esp/flash_photo.py to preload a test image into the photos partition.");
        vTaskDelete(NULL);
        return;
    }

    if (metadata.format != 0 ||
        metadata.width != FRAME_WIDTH ||
        metadata.height != FRAME_HEIGHT ||
        metadata.bytes != FRAME_HEIGHT * FRAME_WIDTH * FRAME_CHANNEL) {
        ESP_LOGE(TAG,
                 "Unsupported stored photo. format=%u width=%u height=%u bytes=%lu; expected grayscale %dx%d bytes=%d.",
                 (unsigned)metadata.format,
                 (unsigned)metadata.width,
                 (unsigned)metadata.height,
                 (unsigned long)metadata.bytes,
                 FRAME_WIDTH,
                 FRAME_HEIGHT,
                 FRAME_HEIGHT * FRAME_WIDTH * FRAME_CHANNEL);
        vTaskDelete(NULL);
        return;
    }

    run_inference_on_grayscale_frame(g_raw_frame);
    ESP_LOGI(TAG, "PHOTO_FLASH_TEST_MODE completed one inference pass.");
    vTaskDelete(NULL);
}

static void camera_flash_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "TFLite Micro inference task initialized in CAMERA_FLASH_MODE.");
    ESP_LOGI(TAG, "Camera mode uses OV2640 capture, photo flash storage, grayscale resize, and TFLite inference.");

    esp_err_t err = photo_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Photo storage init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = camera_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Camera init failed: %s. Switch RUNTIME_MODE to kTestUartFrame when testing without OV2640.",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        CameraFrame frame = {};
        err = camera_capture_frame(&frame);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera capture failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        err = photo_storage_write_latest(frame);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Photo storage write failed: %s", esp_err_to_name(err));
        }

        if (frame.format == CameraFrameFormat::kGrayscale) {
            if (!allocate_frame_buffers()) {
                camera_capture_release(&frame);
                vTaskDelete(NULL);
                return;
            }
            resize_grayscale_to_runtime_frame(frame.data, frame.width, frame.height, g_raw_frame);
            run_inference_on_grayscale_frame(g_raw_frame);
        } else {
            ESP_LOGW(TAG,
                     "Captured format is not grayscale. Stored photo only; inference waits for grayscale/JPEG decode support.");
        }

        camera_capture_release(&frame);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "Runtime mode: %s", runtime_mode_name());
    log_memory_status();
    if (!init_tflite_micro()) {
        ESP_LOGE(TAG, "Halt: TFLite Micro initialization failed.");
        return;
    }

    if (RUNTIME_MODE == RuntimeMode::kTestUartFrame) {
        xTaskCreatePinnedToCore(uart_test_inference_task,
                                "uart_test_inference_task",
                                16 * 1024,
                                NULL,
                                5,
                                NULL,
                                1);
        return;
    }

    if (RUNTIME_MODE == RuntimeMode::kPhotoFlashTest) {
        xTaskCreatePinnedToCore(photo_flash_test_task,
                                "photo_flash_test_task",
                                16 * 1024,
                                NULL,
                                5,
                                NULL,
                                1);
        return;
    }

    xTaskCreatePinnedToCore(camera_flash_task,
                            "camera_flash_task",
                            16 * 1024,
                            NULL,
                            5,
                            NULL,
                            1);
}
