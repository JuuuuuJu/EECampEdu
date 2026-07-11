#include <math.h>
#include <stdint.h>
#include <stdarg.h>
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
#include "input_controls.hpp"
#include "output_controls.hpp"
#include "model_config.hpp"
#include "photo_storage.hpp"
#include "usb_composite.hpp"
#include "esp_camera.h"
#include "img_converters.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "TFLM_GESTURE";

static uint8_t *g_raw_frame = nullptr;
static uint8_t *g_model_frame = nullptr;
static uint8_t *g_tensor_arena = nullptr;
static bool g_tflite_ready = false;

// Forward declarations for functions defined further down in app_main.cpp
static bool allocate_frame_buffers();
static void resize_grayscale_to_runtime_frame(const uint8_t *src, int src_width, int src_height, uint8_t *dst);
static esp_err_t capture_grayscale_inference_frame();
static bool run_inference_on_grayscale_frame(const uint8_t *g_raw_frame);

static bool streaming_mode = CAMERA_USB_CONTINUOUS_CAPTURE;
static SemaphoreHandle_t camera_mutex = NULL;
static pixformat_t g_current_format = PIXFORMAT_JPEG;
static framesize_t g_current_size = FRAMESIZE_VGA;
static TaskHandle_t g_input_controls_task_handle = nullptr;
static TaskHandle_t g_uart_test_task_handle = nullptr;
static TaskHandle_t g_camera_stream_task_handle = nullptr;
static TaskHandle_t g_usb_cdc_command_task_handle = nullptr;
static TaskHandle_t g_photo_flash_task_handle = nullptr;
static TaskHandle_t g_camera_flash_task_handle = nullptr;
static TaskHandle_t g_output_controls_task_handle = nullptr;

static void dual_printf(const char *format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    printf("%s", buffer);
    fflush(stdout);
    
    usb_cdc_printf("%s", buffer);
}

static void send_frame_to_pc(const CameraFrame &frame) {
    usb_cdc_printf("---START_IMAGE:%d:%d:%d:%d---\n", (int)frame.format, frame.width, frame.height, (int)frame.size);
    usb_cdc_write_base64(frame.data, frame.size);
    usb_cdc_printf("---END_IMAGE---\n");
}

static void usb_list_files() {
    usb_cdc_printf("\n--- LOCAL FLASH FILE LIST ---\n");
    usb_msc_mount_to_app();
    vTaskDelay(pdMS_TO_TICKS(50)); // Wait for VFS mount
    
    DIR *dir = opendir("/usb");
    if (!dir) {
        usb_cdc_printf("ERROR: Failed to open root directory.\n");
        return;
    }
    
    struct dirent *entry;
    int count = 0;
    size_t total_size = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "/usb/%s", entry->d_name);
            struct stat st;
            if (stat(filepath, &st) == 0) {
                usb_cdc_printf("  - /usb/%s (%d bytes)\n", entry->d_name, (int)st.st_size);
                total_size += st.st_size;
                count++;
            }
        }
    }
    closedir(dir);
    
    const esp_partition_t *storage = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        "storage");
    if (storage != nullptr) {
        usb_cdc_printf("Total: %d files, used space: %d bytes (storage partition: %llu bytes)\n",
                       count,
                       (int)total_size,
                       (unsigned long long)storage->size);
    } else {
        usb_cdc_printf("Total: %d files, used space: %d bytes\n", count, (int)total_size);
    }
    usb_cdc_printf("-----------------------------\n\n");
}

static void camera_stream_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "Camera stream task started.");
    
    while (true) {
        if (streaming_mode && usb_cdc_is_connected()) {
            if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                CameraFrame frame = {};
                esp_err_t err = camera_capture_frame(&frame);
                if (err == ESP_OK) {
                    send_frame_to_pc(frame);
                    
                    // Also run inference if it's grayscale
                    if (g_tflite_ready && frame.format == CameraFrameFormat::kGrayscale) {
                        if (allocate_frame_buffers()) {
                            resize_grayscale_to_runtime_frame(frame.data, frame.width, frame.height, g_raw_frame);
                            run_inference_on_grayscale_frame(g_raw_frame);
                        }
                    }
                    camera_capture_release(&frame);
                }
                xSemaphoreGive(camera_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(15)); // Stream task rate limiter
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void usb_cdc_command_task(void *pvParameters) {
    (void)pvParameters;
    usb_cdc_msg_t msg;
    QueueHandle_t q = usb_cdc_get_queue();
    char cmd_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    
    while (true) {
        if (xQueueReceive(q, &msg, portMAX_DELAY)) {
            if (msg.buf_len > 0) {
                // Null-terminate the command
                memcpy(cmd_buf, msg.buf, msg.buf_len);
                cmd_buf[msg.buf_len] = '\0';
                
                // Strip trailing newlines or carriage returns
                while (msg.buf_len > 0 && (cmd_buf[msg.buf_len - 1] == '\n' || cmd_buf[msg.buf_len - 1] == '\r')) {
                    cmd_buf[msg.buf_len - 1] = '\0';
                    msg.buf_len--;
                }
                
                if (strlen(cmd_buf) == 0) continue;
                
                // Parse command
                if (strcasecmp(cmd_buf, "format") == 0) {
                    dual_printf("Formatting storage partition... please wait...\n");
                    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        esp_camera_deinit();
                        xSemaphoreGive(camera_mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                    
                    usb_msc_mount_to_app();
                    esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl("/usb", "storage");
                    if (err == ESP_OK) {
                        dual_printf("FATFS partition formatted successfully! Rebooting device to sync...\n");
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    } else {
                        dual_printf("ERROR: FATFS partition formatting failed.\n");
                    }
                    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        camera_capture_init();
                        xSemaphoreGive(camera_mutex);
                    }
                    continue;
                }
                
                if (strcasecmp(cmd_buf, "usb") == 0) {
                    streaming_mode = false;
                    dual_printf("[System] Streaming: DISABLED (USB Mode)\n");
                    dual_printf("Exposing storage partition to host PC as USB drive...\n");
                    usb_msc_mount_to_pc();
                    continue;
                }
                
                char action = cmd_buf[0];
                const char *argStr = cmd_buf + 1;
                while (*argStr == ' ' || *argStr == '\t') {
                    argStr++;
                }
                int val = atoi(argStr);
                
                switch (action) {
                    case 'c':
                    case 'C': {
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                            CameraFrame frame = {};
                            esp_err_t err = camera_capture_frame(&frame);
                            if (err == ESP_OK) {
                                send_frame_to_pc(frame);
                                camera_capture_release(&frame);
                            } else {
                                dual_printf("ERROR: Capture failed: %s\n", esp_err_to_name(err));
                            }

                            // Camera-controller unit test uses c/Space. Keep preview as VGA JPEG,
                            // but run model inference from a temporary grayscale capture path.
                            if (g_tflite_ready) {
                                capture_grayscale_inference_frame();
                            }
                            xSemaphoreGive(camera_mutex);
                        } else {
                            dual_printf("ERROR: Camera busy; capture skipped.\n");
                        }
                        break;
                    }
                    case 'd':
                    case 'D': {
                        streaming_mode = (val == 1);
                        dual_printf("[System] Streaming: %s\n", streaming_mode ? "ENABLED" : "DISABLED");
                        break;
                    }
                    case 'w':
                    case 'W': {
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                            CameraFrame frame = {};
                            esp_err_t err = camera_capture_frame(&frame);
                            if (err == ESP_OK) {
                                err = photo_storage_write_latest(frame);
                                if (err == ESP_OK) {
                                    dual_printf("[System] Saved frame to flash successfully.\n");
                                } else {
                                    dual_printf("ERROR: Save failed: %s\n", esp_err_to_name(err));
                                }
                                camera_capture_release(&frame);
                            }
                            xSemaphoreGive(camera_mutex);
                        }
                        break;
                    }
                    case 'l':
                    case 'L': {
                        usb_list_files();
                        break;
                    }
                    case 'k':
                    case 'K': {
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(50));
                        unlink("/usb/latest.raw");
                        unlink("/usb/latest.meta");
                        unlink("/usb/latest.bmp");
                        dual_printf("[System] Cleared files in storage partition.\n");
                        break;
                    }
                    case 'i':
                    case 'I': {
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(4000)) == pdTRUE) {
                            capture_grayscale_inference_frame();
                            xSemaphoreGive(camera_mutex);
                        } else {
                            dual_printf("ERROR: Camera busy; inference capture skipped.\n");
                        }
                        break;
                    }
                    case 'f':
                    case 'F': {
                        int format_val = PIXFORMAT_GRAYSCALE;
                        if (val == 1) format_val = PIXFORMAT_RGB565;
                        else if (val == 2) format_val = PIXFORMAT_YUV422;
                        else if (val == 3) format_val = PIXFORMAT_JPEG;
                        
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                            esp_err_t err = camera_capture_reinit(format_val, (int)g_current_size);
                            if (err == ESP_OK) {
                                g_current_format = (pixformat_t)format_val;
                                dual_printf("[System] Pixel Format updated successfully.\n");
                            } else {
                                dual_printf("ERROR: Reinit format failed: %s\n", esp_err_to_name(err));
                            }
                            xSemaphoreGive(camera_mutex);
                        }
                        break;
                    }
                    case 's':
                    case 'S': {
                        int size_val = FRAMESIZE_96X96;
                        if (val == 1) size_val = FRAMESIZE_QQVGA;
                        else if (val == 2) size_val = FRAMESIZE_QVGA;
                        else if (val == 3) size_val = FRAMESIZE_VGA;
                        else if (val == 4) size_val = FRAMESIZE_SVGA;
                        else if (val == 5) size_val = FRAMESIZE_UXGA;
                        
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                            esp_err_t err = camera_capture_reinit((int)g_current_format, size_val);
                            if (err == ESP_OK) {
                                g_current_size = (framesize_t)size_val;
                                dual_printf("[System] Resolution updated successfully.\n");
                            } else {
                                dual_printf("ERROR: Reinit resolution failed: %s\n", esp_err_to_name(err));
                            }
                            xSemaphoreGive(camera_mutex);
                        }
                        break;
                    }
                    case 'e':
                    case 'E': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_exposure_ctrl(s, val);
                            dual_printf("[System] Exposure Control updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'g':
                    case 'G': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_gain_ctrl(s, val);
                            dual_printf("[System] Gain Control updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'v':
                    case 'V': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_aec_value(s, val);
                            dual_printf("[System] Manual Exposure AEC value updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'a':
                    case 'A': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_agc_gain(s, val);
                            dual_printf("[System] Manual Gain AGC value updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'b':
                    case 'B': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_brightness(s, val);
                            dual_printf("[System] Brightness updated to %d.\n", val);
                        }
                        break;
                    }
                    case 't':
                    case 'T': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_contrast(s, val);
                            dual_printf("[System] Contrast updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'x':
                    case 'X': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_saturation(s, val);
                            dual_printf("[System] Saturation updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'm':
                    case 'M': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_hmirror(s, val);
                            dual_printf("[System] Horizontal Mirror updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'p':
                    case 'P': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_vflip(s, val);
                            dual_printf("[System] Vertical Flip updated to %d.\n", val);
                        }
                        break;
                    }
                }
            }
        }
    }
}
static constexpr int kExpectedTfliteSchemaVersion = 3;

static constexpr int kClassCount = 5;
static constexpr const char *kClassNames[kClassCount] = {
    "up",
    "down",
    "right",
    "left",
    "null",
};


static int g_tensor_arena_size = 0;
static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;
static const void *g_mapped_model = nullptr;
static esp_partition_mmap_handle_t g_model_mmap_handle = 0;

static void input_controls_monitor_task(void *pvParameters) {
    (void)pvParameters;
    InputControlsSnapshot previous = input_controls_get_snapshot();
    while (true) {
        const InputControlsSnapshot current = input_controls_get_snapshot();
        if (current.encoder_position != previous.encoder_position ||
            current.encoder_button_presses != previous.encoder_button_presses ||
            current.button2_presses != previous.button2_presses) {
            ESP_LOGI(TAG,
                     "INPUT_CONTROL,encoder=%ld,delta=%ld,encoder_button=%lu,button2=%lu",
                     (long)current.encoder_position,
                     (long)(current.encoder_position - previous.encoder_position),
                     (unsigned long)current.encoder_button_presses,
                     (unsigned long)current.button2_presses);
            previous = current;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void maybe_start_input_controls() {
    if (!ENABLE_INPUT_CONTROLS) {
        ESP_LOGI(TAG, "Input controls disabled.");
        return;
    }

    esp_err_t err = input_controls_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Input controls init failed: %s", esp_err_to_name(err));
        return;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(input_controls_monitor_task,
                                                       "input_controls_monitor",
                                                       4 * 1024,
                                                       NULL,
                                                       3,
                                                       &g_input_controls_task_handle,
                                                       0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create input_controls_monitor task.");
        g_input_controls_task_handle = nullptr;
    }
}

static void maybe_start_output_controls() {
    if (!ENABLE_ROBOT_ARM_OUTPUT) {
        ESP_LOGI(TAG, "Robot arm output disabled.");
        return;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(output_controls_task,
                                                       "output_controls_task",
                                                       4 * 1024,
                                                       NULL,
                                                       3,
                                                       &g_output_controls_task_handle,
                                                       0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create output_controls_task.");
        g_output_controls_task_handle = nullptr;
    }
}

static const char *runtime_mode_name() {
    switch (RUNTIME_MODE) {
        case RuntimeMode::kTestUartFrame:
            return "TEST_MODE_UART_FRAME";
        case RuntimeMode::kPhotoFlashTest:
            return "PHOTO_FLASH_TEST_MODE";
        case RuntimeMode::kCameraFlash:
            return "CAMERA_FLASH_MODE";
        case RuntimeMode::kInputOutputSelfTest:
            return "INPUT_OUTPUT_SELF_TEST";
        case RuntimeMode::kCameraUsbMsc:
            return "CAMERA_USB_MSC_MODE";
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

struct CropCandidate {
    int area;
    int x0;
    int y0;
    int x1;
    int y1;
    int threshold;
    bool bright_foreground;
};

static CropCandidate find_foreground_candidate(const uint8_t *frame, int threshold, bool bright_foreground) {
    CropCandidate candidate = {};
    candidate.area = 0;
    candidate.x0 = FRAME_WIDTH - 1;
    candidate.y0 = FRAME_HEIGHT - 1;
    candidate.x1 = 0;
    candidate.y1 = 0;
    candidate.threshold = threshold;
    candidate.bright_foreground = bright_foreground;

    for (int y = 0; y < FRAME_HEIGHT; ++y) {
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            const uint8_t pixel = frame[y * FRAME_WIDTH + x];
            const bool is_foreground = bright_foreground ? (pixel > threshold) : (pixel < threshold);
            if (!is_foreground) {
                continue;
            }
            ++candidate.area;
            if (x < candidate.x0) candidate.x0 = x;
            if (x > candidate.x1) candidate.x1 = x;
            if (y < candidate.y0) candidate.y0 = y;
            if (y > candidate.y1) candidate.y1 = y;
        }
    }
    return candidate;
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
    const int dark_threshold = clamp_int(mean - HAND_CROP_DARK_DELTA,
                                         HAND_CROP_MIN_THRESHOLD,
                                         HAND_CROP_MAX_THRESHOLD);
    const int bright_threshold = clamp_int(mean + HAND_CROP_DARK_DELTA,
                                           HAND_CROP_MIN_THRESHOLD,
                                           HAND_CROP_MAX_THRESHOLD);

    CropCandidate dark = find_foreground_candidate(frame, dark_threshold, false);
    CropCandidate bright = find_foreground_candidate(frame, bright_threshold, true);

    const int min_area = total * HAND_CROP_MIN_AREA_PERCENT / 100;
    const bool dark_ok = dark.area >= min_area;
    const bool bright_ok = bright.area >= min_area;

    CropCandidate chosen = dark;
    bool found = false;
    if (dark_ok && bright_ok) {
        // Choose the smaller foreground region; it is usually the hand rather than the background.
        chosen = (dark.area <= bright.area) ? dark : bright;
        found = true;
    } else if (dark_ok) {
        chosen = dark;
        found = true;
    } else if (bright_ok) {
        chosen = bright;
        found = true;
    }

    if (!found) {
        chosen.area = total;
        chosen.x0 = 0;
        chosen.y0 = 0;
        chosen.x1 = FRAME_WIDTH - 1;
        chosen.y1 = FRAME_HEIGHT - 1;
        chosen.threshold = mean;
    }

    const int crop_w = chosen.x1 - chosen.x0 + 1;
    const int crop_h = chosen.y1 - chosen.y0 + 1;
    const int margin_x = crop_w * HAND_CROP_MARGIN_PERCENT / 100;
    const int margin_y = crop_h * HAND_CROP_MARGIN_PERCENT / 100;

    *crop_x0 = clamp_int(chosen.x0 - margin_x, 0, FRAME_WIDTH - 1);
    *crop_y0 = clamp_int(chosen.y0 - margin_y, 0, FRAME_HEIGHT - 1);
    *crop_x1 = clamp_int(chosen.x1 + margin_x, 0, FRAME_WIDTH - 1);
    *crop_y1 = clamp_int(chosen.y1 + margin_y, 0, FRAME_HEIGHT - 1);
    *best_area_out = chosen.area;
    *threshold_out = chosen.bright_foreground ? bright_threshold : dark_threshold;
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

    if (PREFER_INTERNAL_TENSOR_ARENA) {
        g_tensor_arena = (uint8_t *)heap_caps_malloc(FALLBACK_TENSOR_ARENA_SIZE,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        g_tensor_arena_size = FALLBACK_TENSOR_ARENA_SIZE;
        if (g_tensor_arena == nullptr) {
            ESP_LOGW(TAG,
                     "Internal tensor arena allocation failed (%d bytes). Trying %d-byte PSRAM arena.",
                     FALLBACK_TENSOR_ARENA_SIZE,
                     TENSOR_ARENA_SIZE);
        }
    }

    if (g_tensor_arena == nullptr) {
        g_tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_tensor_arena_size = TENSOR_ARENA_SIZE;
        if (g_tensor_arena == nullptr) {
            ESP_LOGW(TAG,
                     "PSRAM tensor arena allocation failed (%d bytes). Trying %d-byte internal fallback.",
                     TENSOR_ARENA_SIZE,
                     FALLBACK_TENSOR_ARENA_SIZE);
        }
    }

    if (g_tensor_arena == nullptr && !PREFER_INTERNAL_TENSOR_ARENA) {
        g_tensor_arena = (uint8_t *)heap_caps_malloc(FALLBACK_TENSOR_ARENA_SIZE,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
    ESP_LOGI(TAG,
             "Tensor arena allocated: %d bytes (%s preferred).",
             g_tensor_arena_size,
             PREFER_INTERNAL_TENSOR_ARENA ? "internal" : "PSRAM");

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
    g_tflite_ready = true;
    return true;
}

static bool allocate_frame_buffers() {
    const int expected_bytes = FRAME_HEIGHT * FRAME_WIDTH * FRAME_CHANNEL;
    const int model_bytes = INPUT_HEIGHT * INPUT_WIDTH * INPUT_CHANNEL;

    if (g_raw_frame == nullptr) {
        g_raw_frame = (uint8_t *)heap_caps_malloc(expected_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_raw_frame == nullptr) {
            g_raw_frame = (uint8_t *)heap_caps_malloc(expected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    if (g_raw_frame == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate raw input frame (%d bytes).", expected_bytes);
        return false;
    }

    if (g_model_frame == nullptr) {
        g_model_frame = (uint8_t *)heap_caps_malloc(model_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_model_frame == nullptr) {
            g_model_frame = (uint8_t *)heap_caps_malloc(model_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    if (g_model_frame == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate model input frame (%d bytes).", model_bytes);
        return false;
    }

    return true;
}

static bool run_inference_on_grayscale_frame(const uint8_t *frame) {
    if (!g_tflite_ready || g_interpreter == nullptr || g_input == nullptr || g_output == nullptr) {
        dual_printf("ERROR,TFLITE_NOT_READY\n");
        return false;
    }
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
    dual_printf("RESULT,%d,%lld,%lld,%lld",
           pred_index,
           (long long)invoke_us,
           (long long)preprocess_us,
           (long long)device_compute_us);
    for (int i = 0; i < kClassCount; ++i) {
        dual_printf(",%d", scores[i]);
    }
    dual_printf("\n");
    output_controls_enqueue(static_cast<OutputGestureAction>(pred_index));
    return true;
}

static void resize_grayscale_to_runtime_frame(const uint8_t *src,
                                              int src_width,
                                              int src_height,
                                              uint8_t *dst) {
    const int side = (src_width < src_height) ? src_width : src_height;
    const int crop_x0 = (src_width - side) / 2;
    const int crop_y0 = (src_height - side) / 2;

    for (int y = 0; y < FRAME_HEIGHT; ++y) {
        const int src_y = crop_y0 + ((FRAME_HEIGHT == 1) ? 0 : (y * (side - 1)) / (FRAME_HEIGHT - 1));
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            const int src_x = crop_x0 + ((FRAME_WIDTH == 1) ? 0 : (x * (side - 1)) / (FRAME_WIDTH - 1));
            dst[y * FRAME_WIDTH + x] = src[src_y * src_width + src_x];
        }
    }
}
static esp_err_t capture_grayscale_inference_frame() {
    if (!g_tflite_ready) {
        dual_printf("ERROR,TFLITE_NOT_READY\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!allocate_frame_buffers()) {
        return ESP_ERR_NO_MEM;
    }

    const pixformat_t previous_format = g_current_format;
    const framesize_t previous_size = g_current_size;
    const bool previous_streaming_mode = streaming_mode;
    streaming_mode = false;

    esp_err_t err = camera_capture_reinit(PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA);
    if (err != ESP_OK) {
        streaming_mode = previous_streaming_mode;
        dual_printf("ERROR: Failed to switch camera to grayscale inference mode: %s\n", esp_err_to_name(err));
        return err;
    }
    g_current_format = PIXFORMAT_GRAYSCALE;
    g_current_size = FRAMESIZE_QVGA;

    CameraFrame frame = {};
    err = camera_capture_frame(&frame);
    if (err == ESP_OK) {
        resize_grayscale_to_runtime_frame(frame.data, frame.width, frame.height, g_raw_frame);
        run_inference_on_grayscale_frame(g_raw_frame);
        camera_capture_release(&frame);
    } else {
        dual_printf("ERROR: Grayscale inference capture failed: %s\n", esp_err_to_name(err));
    }

    esp_err_t restore_err = camera_capture_reinit((int)previous_format, (int)previous_size);
    if (restore_err == ESP_OK) {
        g_current_format = previous_format;
        g_current_size = previous_size;
    } else {
        dual_printf("ERROR: Failed to restore camera preview mode: %s\n", esp_err_to_name(restore_err));
    }
    streaming_mode = previous_streaming_mode;
    return err;
}

static void input_output_self_test_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "Input/output self-test task started.");

    const OutputGestureAction sequence[] = {
        OutputGestureAction::kUp,
        OutputGestureAction::kDown,
        OutputGestureAction::kRight,
        OutputGestureAction::kLeft,
        OutputGestureAction::kNull,
    };
    int index = 0;

    while (true) {
        if (ENABLE_INPUT_CONTROLS) {
            const InputControlsSnapshot snapshot = input_controls_get_snapshot();
            dual_printf("SELFTEST_INPUT,pos=%ld,enc_btn=%lu,btn2=%lu,enc_level=%d,btn2_level=%d\n",
                        (long)snapshot.encoder_position,
                        (unsigned long)snapshot.encoder_button_presses,
                        (unsigned long)snapshot.button2_presses,
                        snapshot.encoder_button_level,
                        snapshot.button2_level);
        } else {
            dual_printf("SELFTEST_INPUT,disabled\n");
        }

        const OutputGestureAction action = sequence[index];
        dual_printf("SELFTEST_OUTPUT,action=%d,enabled=%d\n",
                    (int)action,
                    ENABLE_ROBOT_ARM_OUTPUT ? 1 : 0);
        output_controls_enqueue(action);
        index = (index + 1) % (int)(sizeof(sequence) / sizeof(sequence[0]));
        vTaskDelay(pdMS_TO_TICKS(IO_SELF_TEST_INTERVAL_MS));
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
    ESP_LOGI(TAG, "Reading one preloaded grayscale photo from USB FAT storage.");

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
        ESP_LOGE(TAG, "Use esp/flash_photo.py or camera_usb W command to preload latest.raw/latest.meta in /usb FAT storage.");
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
    maybe_start_input_controls();
    maybe_start_output_controls();

    if (RUNTIME_MODE == RuntimeMode::kInputOutputSelfTest) {
        xTaskCreatePinnedToCore(input_output_self_test_task,
                                "input_output_self_test_task",
                                4 * 1024,
                                NULL,
                                4,
                                NULL,
                                0);
        return;
    }

    const bool model_ready = init_tflite_micro();
    if (!model_ready && RUNTIME_MODE != RuntimeMode::kCameraUsbMsc) {
        ESP_LOGE(TAG, "Halt: TFLite Micro initialization failed.");
        return;
    }
    if (!model_ready) {
        ESP_LOGW(TAG, "TFLite Micro unavailable; continuing camera/USB test without inference.");
    }

    if (RUNTIME_MODE == RuntimeMode::kTestUartFrame) {
        xTaskCreatePinnedToCore(uart_test_inference_task,
                                "uart_test_inference_task",
                                16 * 1024,
                                NULL,
                                5,
                                &g_uart_test_task_handle,
                                1);
        return;
    }

    // Initialize USB Composite CDC + MSC interfaces
    usb_composite_init();
    camera_mutex = xSemaphoreCreateMutex();
    configASSERT(camera_mutex);

    if (RUNTIME_MODE == RuntimeMode::kCameraUsbMsc) {
        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            esp_err_t camera_err = camera_capture_reinit(CAMERA_USB_DEFAULT_PIXEL_FORMAT, CAMERA_USB_DEFAULT_FRAME_SIZE);
            if (camera_err != ESP_OK) {
                ESP_LOGE(TAG, "Camera init failed for CDC preview: %s", esp_err_to_name(camera_err));
            }
            xSemaphoreGive(camera_mutex);
        }
    }

    // Create camera streaming and command parsing tasks
    xTaskCreatePinnedToCore(camera_stream_task, "camera_stream_task", 8192, NULL, 1, &g_camera_stream_task_handle, 1);
    xTaskCreatePinnedToCore(usb_cdc_command_task, "usb_cdc_command_task", 8192, NULL, 5, &g_usb_cdc_command_task_handle, 0);

    if (RUNTIME_MODE == RuntimeMode::kPhotoFlashTest) {
        xTaskCreatePinnedToCore(photo_flash_test_task,
                                "photo_flash_test_task",
                                16 * 1024,
                                NULL,
                                5,
                                &g_photo_flash_task_handle,
                                1);
        return;
    }

    if (RUNTIME_MODE == RuntimeMode::kCameraUsbMsc) {
        return;
    }

    xTaskCreatePinnedToCore(camera_flash_task,
                            "camera_flash_task",
                            16 * 1024,
                            NULL,
                            5,
                            &g_camera_flash_task_handle,
                            1);
}






