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
#include "input_controls.hpp"
#include "model_config.hpp"
#include "photo_storage.hpp"
#include "usb_composite.hpp"
#include "esp_camera.h"
#include "img_converters.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "TFLM_GESTURE";

static uint8_t *g_raw_frame = nullptr;
static uint8_t *g_model_frame = nullptr;
static uint8_t *g_tensor_arena = nullptr;

// Forward declarations for functions defined further down in app_main.cpp
static bool allocate_frame_buffers();
static void resize_grayscale_to_runtime_frame(const uint8_t *src, int src_width, int src_height, uint8_t *dst);
static bool run_inference_on_grayscale_frame(const uint8_t *g_raw_frame);
static void dual_printf(const char *format, ...);


static bool streaming_mode = false;
static SemaphoreHandle_t camera_mutex = NULL;


/**
 * @brief High-Fidelity JPEG to Grayscale Decoder
 * Prevents banding and shadow-crushing by using bit-expansion and rounded integer math.
 */
static bool decode_jpeg_to_high_fidelity_grayscale(const uint8_t *jpg_buf, size_t jpg_size, int w, int h, uint8_t *out_gray_buf) {
    uint8_t *rgb_buf = (uint8_t*)malloc(w * h * 2);
    if (!rgb_buf) return false;

    if (!jpg2rgb565(jpg_buf, jpg_size, rgb_buf, JPG_SCALE_NONE)) {
        free(rgb_buf);
        return false;
    }

    for (int i = 0; i < (w * h); i++) {
        // FIX: Read as Little-Endian!
        uint16_t p = rgb_buf[i * 2] | (rgb_buf[i * 2 + 1] << 8);

        uint8_t r5 = (p >> 11) & 0x1F;
        uint8_t g6 = (p >> 5) & 0x3F;
        uint8_t b5 = (p & 0x1F);

        uint16_t r8 = (r5 << 3) | (r5 >> 2);
        uint16_t g8 = (g6 << 2) | (g6 >> 4);
        uint16_t b8 = (b5 << 3) | (b5 >> 2);

        out_gray_buf[i] = (uint8_t)((r8 * 77 + g8 * 150 + b8 * 29 + 128) >> 8);
    }

    free(rgb_buf);
    return true;
}
static pixformat_t g_current_format = PIXFORMAT_JPEG;
static framesize_t g_current_size = FRAMESIZE_VGA;

extern int g_hmirror;
extern int g_vflip;

static TaskHandle_t g_input_controls_task_handle = nullptr;
[[maybe_unused]] static TaskHandle_t g_uart_test_task_handle = nullptr;
[[maybe_unused]] static TaskHandle_t g_camera_stream_task_handle = nullptr;
[[maybe_unused]] static TaskHandle_t g_usb_cdc_command_task_handle = nullptr;
[[maybe_unused]] static TaskHandle_t g_photo_flash_task_handle = nullptr;
static TaskHandle_t g_camera_flash_task_handle = nullptr;

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
    // Map the internal CameraFrameFormat explicitly to what the Python script expects:
    // Python expects: 0=RGB565, 1=YUV422, 3=GRAYSCALE, 4=JPEG
    int py_fmt = 4; // Default to JPEG to be safe
    if (frame.format == CameraFrameFormat::kGrayscale) py_fmt = 3;
    else if (frame.format == CameraFrameFormat::kRgb565) py_fmt = 0;
    else if (frame.format == CameraFrameFormat::kYuv422) py_fmt = 1;

    usb_cdc_printf("---START_IMAGE:%d:%d:%d:%d---\n", py_fmt, frame.width, frame.height, (int)frame.size);
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
    usb_cdc_printf("Total: %d files, listed used space: %d bytes\n", count, (int)total_size);
    usb_cdc_printf("-----------------------------\n\n");
}

static inline void bmp_put_u32le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static inline void bmp_put_u16le(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

// Persist the model-input photo: nearest-neighbour downscale a w x h grayscale
// image to 96x96 (the model input size) and save it as an 8-bit grayscale BMP on
// the board's flash (/usb). Listable via 'l', downloadable via 'r', viewable over
// MSC. Assumes the FAT volume is already mounted to the app.
static bool save_photo96_bmp(const uint8_t *full_gray, int w, int h, const char *ts) {
    if (!full_gray || w <= 0 || h <= 0) return false;
    const int S = 96;                         // 96 is 4-byte aligned -> no row padding
    static uint8_t small_img[96 * 96];
    for (int y = 0; y < S; ++y) {
        int sy = (int)((int64_t)y * h / S);
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < S; ++x) {
            int sx = (int)((int64_t)x * w / S);
            if (sx >= w) sx = w - 1;
            small_img[y * S + x] = full_gray[sy * w + sx];
        }
    }
    const uint32_t pixels = (uint32_t)S * S;
    const uint32_t offbits = 14 + 40 + 256 * 4;
    const uint32_t filesize = offbits + pixels;

    uint8_t hdr[14 + 40];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    bmp_put_u32le(hdr + 2, filesize);
    bmp_put_u32le(hdr + 10, offbits);
    bmp_put_u32le(hdr + 14, 40);              // info header size
    bmp_put_u32le(hdr + 18, S);               // width
    bmp_put_u32le(hdr + 22, S);               // height (bottom-up)
    bmp_put_u16le(hdr + 26, 1);               // planes
    bmp_put_u16le(hdr + 28, 8);               // bits per pixel
    bmp_put_u32le(hdr + 34, pixels);          // image size
    bmp_put_u32le(hdr + 46, 256);             // colours used
    bmp_put_u32le(hdr + 50, 256);             // important colours
    (void)ts;
    const char *photo_dir = "/usb/main_inference";
    if (mkdir(photo_dir, 0775) != 0 && errno != EEXIST) {
        dual_printf("ERROR: could not create %s for 96x96 photos: errno=%d (%s)\n", photo_dir, errno, strerror(errno));
        return false;
    }
    char path[64];
    bool found_path = false;
    for (unsigned i = 1; i <= 99999; ++i) {
        snprintf(path, sizeof(path), "%s/P96%05u.BMP", photo_dir, i);
        if (access(path, F_OK) != 0) {
            found_path = true;
            break;
        }
    }
    if (!found_path) {
        dual_printf("ERROR: no available 96x96 photo filename\n");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        dual_printf("ERROR: could not open %s for 96x96 photo: errno=%d (%s)\n", path, errno, strerror(errno));
        return false;
    }
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        dual_printf("ERROR: failed writing BMP header to %s\n", path);
        fclose(f);
        unlink(path);
        return false;
    }
    uint8_t pal[4];
    for (int i = 0; i < 256; ++i) { pal[0] = pal[1] = pal[2] = (uint8_t)i; pal[3] = 0; fwrite(pal, 1, 4, f); }
    for (int y = S - 1; y >= 0; --y) fwrite(&small_img[y * S], 1, S, f);
    fclose(f);
    dual_printf("[System] 96x96 photo saved to flash! File: %s\n", path);
    return true;
}

static void camera_stream_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "Camera stream task started.");

    while (true) {
        if (streaming_mode) {
            if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                CameraFrame frame = {};
                esp_err_t err = camera_capture_frame(&frame);
                if (err == ESP_OK) {
                    send_frame_to_pc(frame);
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


static bool run_quick_live_inference() {
    bool was_streaming = streaming_mode;
    if (was_streaming) {
        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            streaming_mode = false;
            xSemaphoreGive(camera_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    bool ok = false;
    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        dual_printf("ERROR,camera_busy\n");
        if (was_streaming) streaming_mode = true;
        return false;
    }

    if (was_streaming) {
        camera_fb_t *dummy = esp_camera_fb_get();
        if (dummy) esp_camera_fb_return(dummy);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        dual_printf("ERROR,camera_capture_failed\n");
        xSemaphoreGive(camera_mutex);
        if (was_streaming) streaming_mode = true;
        return false;
    }

    const size_t data_len = fb->len;
    uint8_t *temp_buf = (uint8_t *)malloc(data_len);
    const int fmt = fb->format;
    const int w = fb->width;
    const int h = fb->height;
    if (temp_buf != nullptr) {
        memcpy(temp_buf, fb->buf, data_len);
    }
    esp_camera_fb_return(fb);
    xSemaphoreGive(camera_mutex);

    if (temp_buf == nullptr) {
        dual_printf("ERROR,capture_alloc_failed\n");
        if (was_streaming) streaming_mode = true;
        return false;
    }

    if (allocate_frame_buffers()) {
        uint8_t *full_gray_buf = (uint8_t *)malloc((size_t)w * (size_t)h);
        if (full_gray_buf != nullptr) {
            bool decoded = false;
            if (fmt == PIXFORMAT_JPEG) {
                decoded = decode_jpeg_to_high_fidelity_grayscale(temp_buf, data_len, w, h, full_gray_buf);
            } else if (fmt == PIXFORMAT_GRAYSCALE) {
                const size_t pixels = (size_t)w * (size_t)h;
                if (data_len >= pixels) {
                    memcpy(full_gray_buf, temp_buf, pixels);
                    decoded = true;
                }
            }

            if (decoded) {
                resize_grayscale_to_runtime_frame(full_gray_buf, w, h, g_raw_frame);
                ok = run_inference_on_grayscale_frame(g_raw_frame);
            } else {
                dual_printf("ERROR,quick_inference_decode_failed\n");
            }
            free(full_gray_buf);
        } else {
            dual_printf("ERROR,quick_inference_gray_alloc_failed\n");
        }
    } else {
        dual_printf("ERROR,frame_buffer_alloc_failed\n");
    }

    free(temp_buf);
    if (was_streaming) {
        streaming_mode = true;
    }
    return ok;
}

static void usb_cdc_command_task(void *pvParameters) {
    (void)pvParameters;
    usb_cdc_msg_t msg;
    QueueHandle_t q = usb_cdc_get_queue();
    char cmd_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t cmd_len = 0;

    while (true) {
        if (xQueueReceive(q, &msg, portMAX_DELAY)) {
            if (msg.buf_len > 0) {
                bool command_ready = false;
                for (size_t i = 0; i < msg.buf_len; ++i) {
                    const char ch = static_cast<char>(msg.buf[i]);
                    if (ch == '\r' || ch == '\n') {
                        cmd_buf[cmd_len] = '\0';
                        command_ready = (cmd_len > 0);
                        cmd_len = 0;
                        break;
                    }
                    if (cmd_len < CONFIG_TINYUSB_CDC_RX_BUFSIZE) {
                        cmd_buf[cmd_len++] = ch;
                    } else {
                        cmd_len = 0;
                        dual_printf("ERROR,command_too_long\n");
                    }
                }

                if (!command_ready) continue;

                // Parse command
                if (strcasecmp(cmd_buf, "input") == 0) {
                    const InputControlsSnapshot snapshot = input_controls_get_snapshot();
                    dual_printf("INPUT_STATUS,encoder=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,enc_level=%d,clk=%d,dt=%d\n",
                                (long)snapshot.encoder_position,
                                (unsigned long)snapshot.encoder_button_presses,
                                (unsigned long)snapshot.button2_presses,
                                snapshot.button2_level,
                                snapshot.encoder_button_level,
                                snapshot.encoder_clk_level,
                                snapshot.encoder_dt_level);
                }
                else if (strcasecmp(cmd_buf, "format") == 0) {
                    // 1. Pause the camera stream FIRST to prevent Base64 text collisions!
                    bool was_streaming = streaming_mode;
                    if (was_streaming) {
                        if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                            streaming_mode = false;
                            xSemaphoreGive(camera_mutex);
                        }
                        vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera stream task exit its loop
                    }

                    // NOW it is safe to print to the serial port
                    dual_printf("[System] Formatting storage partition (4MB)... please wait (~10-20 seconds)...\n");

                    // 2. Shut down the camera hardware temporarily
                    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        esp_camera_deinit();
                    }

                    // 3. Take back the USB drive and WAIT for the transition!
                    usb_msc_mount_to_app();
                    vTaskDelay(pdMS_TO_TICKS(500));

                    // 4. Forcefully erase the raw flash partition
                    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
                    if (part != nullptr) {
                        esp_err_t err = esp_partition_erase_range(part, 0, part->size);
                        if (err == ESP_OK) {
                            dual_printf("[System] FATFS partition erased successfully! Rebooting device to rebuild FAT table...\n");
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else {
                            dual_printf("ERROR: Partition erase failed: %s\n", esp_err_to_name(err));
                        }
                    } else {
                        dual_printf("ERROR: Could not find FAT partition in partition table!\n");
                    }

                    // 5. Recover state if format failed
                    camera_capture_init();
                    xSemaphoreGive(camera_mutex); // Give mutex back

                    if (was_streaming) {
                        streaming_mode = true;
                    }
                    continue;
                }

               if (strcasecmp(cmd_buf, "usb") == 0) {
                    // 1. Pause the stream safely FIRST to avoid text collision in the JPEG!
                    bool was_streaming = streaming_mode;
                    if (was_streaming) {
                        if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                            streaming_mode = false;
                            xSemaphoreGive(camera_mutex);
                        }
                        vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera stream task exit its loop
                    }

                    // 2. NOW it is safe to print text to the serial port
                    dual_printf("[System] Streaming: DISABLED (USB Mode)\n");
                    dual_printf("[System] Exposing storage partition to host PC as USB drive...\n");
                    usb_msc_mount_to_pc();
                    continue;
                }

                char action = cmd_buf[0];
                const char *argStr = cmd_buf + 1;
                while (*argStr == ' ' || *argStr == '\t') {
                    argStr++;
                }
                int val = atoi(argStr);

                bool lock_acquired = false;
                char act_upper = (action >= 'a' && action <= 'z') ? (action - 'a' + 'A') : action;
                if (act_upper == 'E' || act_upper == 'G' || act_upper == 'V' ||
                    act_upper == 'A' || act_upper == 'B' || act_upper == 'T' ||
                    act_upper == 'X' || act_upper == 'M' || act_upper == 'P' ||
                    act_upper == 'Y') {
                    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        lock_acquired = true;
                    } else {
                        dual_printf("ERROR: Camera busy, settings update delayed.\n");
                        continue;
                    }
                }

                switch (action) {
                    case 'j':
                    case 'J':
                    case 'c':
                    case 'C': {
                        const bool save_capture = (action == 'c' || action == 'C');

                        // 1. Pause stream to prevent serial collision
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }

                        // 2. Prepare USB drive only when this command really stores a photo.
                        if (save_capture) {
                            usb_msc_mount_to_app();
                            vTaskDelay(pdMS_TO_TICKS(500));
                        }

                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {

                            // Flush one stale buffer
                            camera_fb_t *dummy = esp_camera_fb_get();
                            if (dummy) esp_camera_fb_return(dummy);

                            // Capture the real, clean frame
                            camera_fb_t *fb = esp_camera_fb_get();
                            if (!fb) {
                                dual_printf("ERROR: Failed to acquire camera frame buffer.\n");
                                xSemaphoreGive(camera_mutex);
                            } else {
                                size_t data_len = fb->len;
                                uint8_t *temp_buf = (uint8_t*)malloc(data_len);

                                if (!temp_buf) {
                                    dual_printf("ERROR: Failed to allocate %d bytes for capture.\n", (int)data_len);
                                    esp_camera_fb_return(fb);
                                    xSemaphoreGive(camera_mutex);
                                } else {
                                    // Copy data safely out of camera DMA space
                                    memcpy(temp_buf, fb->buf, data_len);

                                    int fmt = fb->format;
                                    int w = fb->width;
                                    int h = fb->height;

                                    // CRITICAL FIX: Release hardware lock BEFORE doing heavy file I/O or Inference!
                                    esp_camera_fb_return(fb);
                                    xSemaphoreGive(camera_mutex);

                                    // --- PART A: Save Original Image (Like 'w' command) ---
                                    // Use strict FAT 8.3 names and no overwrite so captures accumulate safely.
                                    const char* ts = (strlen(argStr) > 0) ? argStr : "manual";
                                    (void)ts;

                                    CameraFrame original_frame = {};
                                    original_frame.data = temp_buf;
                                    original_frame.size = data_len;
                                    original_frame.width = w;
                                    original_frame.height = h;
                                    if (fmt == PIXFORMAT_JPEG) original_frame.format = CameraFrameFormat::kJpeg;
                                    else if (fmt == PIXFORMAT_GRAYSCALE) original_frame.format = CameraFrameFormat::kGrayscale;
                                    else if (fmt == PIXFORMAT_RGB565) original_frame.format = CameraFrameFormat::kRgb565;
                                    else if (fmt == PIXFORMAT_YUV422) original_frame.format = CameraFrameFormat::kYuv422;
                                    else original_frame.format = CameraFrameFormat::kJpeg;
                                    original_frame.handle = nullptr;

                                    if (save_capture) {
                                        esp_err_t store_err = photo_storage_write_capture(original_frame, "main");
                                        if (store_err == ESP_OK) {
                                            dual_printf("[System] Image saved to ESP flash storage.\n");
                                        } else {
                                            dual_printf("ERROR,photo_storage_write,%s,%s\n", esp_err_to_name(store_err), photo_storage_last_error());
                                        }
                                    }
                                    // --- PART B: Prepare & Run Inference (Like 'i' command) ---
                                    if (allocate_frame_buffers()) {
                                        bool success = false;
                                        uint8_t *full_gray_buf = (uint8_t*)malloc(w * h);

                                        if (full_gray_buf) {
                                            if (fmt == PIXFORMAT_JPEG) {
                                                // Convert JPEG directly to Full Grayscale
                                                success = decode_jpeg_to_high_fidelity_grayscale(temp_buf, data_len, w, h, full_gray_buf);
                                            } else if (fmt == PIXFORMAT_GRAYSCALE) {
                                                memcpy(full_gray_buf, temp_buf, data_len);
                                                success = true;
                                            }

                                            if (success) {
                                                // Downscale full grayscale to 96x96 TFLite Input Tensor
                                                resize_grayscale_to_runtime_frame(full_gray_buf, w, h, g_raw_frame);

                                                // Construct virtual frame and save Ghost Files using our unified wrapper!
                                                CameraFrame inf_frame = {};
                                                inf_frame.data = g_raw_frame;
                                                inf_frame.size = FRAME_WIDTH * FRAME_HEIGHT;
                                                inf_frame.width = FRAME_WIDTH;
                                                inf_frame.height = FRAME_HEIGHT;
                                                inf_frame.format = CameraFrameFormat::kGrayscale;
                                                inf_frame.handle = nullptr;

                                                if (save_capture) {
                                                    // Also persist a 96x96 model-input photo (viewable/downloadable).
                                                    save_photo96_bmp(full_gray_buf, w, h, ts);
                                                }

                                                dual_printf("[System] Inference started on live capture.\n");
                                                run_inference_on_grayscale_frame(g_raw_frame);
                                            } else {
                                                dual_printf("ERROR: Failed to prepare capture for inference.\n");
                                            }
                                            free(full_gray_buf);
                                        }
                                    }
                                    free(temp_buf);
                                }
                            }
                        } else {
                            dual_printf("ERROR: Camera busy, could not acquire mutex.\n");
                        }

                        // 3. Keep MSC mounted on the ESP app side during live inference.
                        // Re-exposing it to Windows after every prediction makes the removable drive pop up repeatedly.
                        // Use the explicit usb command when the user wants to browse stored photos on the PC.
                        vTaskDelay(pdMS_TO_TICKS(100));

                        // 4. Resume stream
                        if (was_streaming) {
                            streaming_mode = true;
                        }
                        break;
                    }
                    case 'q':
                    case 'Q': {
                        run_quick_live_inference();
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
                        // 1. Pause streaming to free up DMA resources
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera stream task exit
                        }

                        // 2. Take control of the USB drive from the PC
                        dual_printf("[System] Preparing flash drive...\n");
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500)); // CRITICAL: Wait for VFS to transition!

                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {

                            // Flush one stale buffer left behind by the DMA stream
                            camera_fb_t *dummy = esp_camera_fb_get();
                            if (dummy) esp_camera_fb_return(dummy);

                            // Capture the real, clean frame
                            camera_fb_t *fb = esp_camera_fb_get();
                            if (!fb) {
                                dual_printf("ERROR: Failed to acquire camera frame buffer.\n");
                                xSemaphoreGive(camera_mutex);
                            } else {
                                size_t data_len = fb->len;
                                uint8_t *temp_buf = (uint8_t*)malloc(data_len);

                                if (!temp_buf) {
                                    dual_printf("ERROR: Failed to allocate %d bytes for flash write.\n", (int)data_len);
                                    esp_camera_fb_return(fb);
                                    xSemaphoreGive(camera_mutex);
                                } else {
                                    // Copy data safely out of camera DMA space
                                    memcpy(temp_buf, fb->buf, data_len);

                                    // Determine correct file extension
                                    const char *ext = "bin";
                                    if (fb->format == PIXFORMAT_JPEG) ext = "jpg";
                                    else if (fb->format == PIXFORMAT_GRAYSCALE) ext = "gray";
                                    else if (fb->format == PIXFORMAT_RGB565) ext = "rgb565";

                                    // argStr contains the timestamp from Python
                                    const char* ts = (strlen(argStr) > 0) ? argStr : "manual";

                                    char filepath[1024];
                                    snprintf(filepath, sizeof(filepath), "/usb/img_%s_fmt%d_w%d_h%d.%s",
                                                ts, (int)fb->format, fb->width, fb->height, ext);

                                    // CRITICAL FIX: Release camera hardware lock BEFORE writing to slow flash!
                                    esp_camera_fb_return(fb);
                                    xSemaphoreGive(camera_mutex);

                                    // 3. Write to Flash
                                    FILE *f = fopen(filepath, "wb");
                                    if (!f) {
                                        dual_printf("ERROR: Failed to open %s for writing.\n", filepath);
                                    } else {
                                        size_t written = fwrite(temp_buf, 1, data_len, f);
                                        fclose(f);
                                        if (written == data_len) {
                                            dual_printf("[System] Image saved to flash! File: %s (%d bytes)\n", filepath, (int)written);
                                        } else {
                                            dual_printf("ERROR: Incomplete file write. Only wrote %d of %d bytes.\n", (int)written, (int)data_len);
                                        }
                                    }
                                    free(temp_buf);
                                }
                            }
                        } else {
                            dual_printf("ERROR: Camera busy, could not acquire mutex.\n");
                        }

                        // 4. Give the USB drive back to the PC so you can see the file immediately!
                        vTaskDelay(pdMS_TO_TICKS(100)); // Let the file system settle
                        usb_msc_mount_to_pc();

                        // 5. Resume stream if it was running
                        if (was_streaming) {
                            streaming_mode = true;
                            dual_printf("[System] Streaming: RESUMED\n");
                        }
                        break;
                    }
                    case 'l':
                    case 'L': {
                        // 1. Pause the stream safely FIRST to avoid text collision in the image data!
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera stream task finish sending the current frame
                        }

                        // 2. NOW it is safe to interact with the file system and print to the serial port
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500)); // Give VFS time to transition locks

                        usb_list_files();

                        // CRITICAL FIX: Give the serial TX queue time to fully transmit the list to the PC
                        // before triggering a USB re-enumeration/mount change.
                        vTaskDelay(pdMS_TO_TICKS(1000));

                        // 3. Resume the stream if it was running
                        if (was_streaming) {
                            streaming_mode = true;
                        }
                        usb_msc_mount_to_pc();
                        break;
                    }
                    case 'r':
                    case 'R': {
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera stream task finish sending the current frame
                        }

                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500)); // Give VFS time to transition locks

                        char input_buf[256];
                        strncpy(input_buf, argStr, sizeof(input_buf));
                        input_buf[sizeof(input_buf)-1] = '\0';

                        // Strip trailing spaces/newlines
                        int len = strlen(input_buf);
                        while (len > 0 && (input_buf[len-1] <= ' ')) { input_buf[--len] = '\0'; }

                        if (len == 0) {
                            dual_printf("ERROR: Usage: r <filename> OR r <index>\n");
                            usb_msc_mount_to_pc();
                            if (was_streaming) streaming_mode = true;
                            break;
                        }

                        // 1. Resolve to full path
                        char target_path[512] = {0};
                        bool is_index = true;
                        for(int j=0; j<len; j++) { if(!isdigit(input_buf[j])) is_index = false; }

                        if (is_index) {
                            int target_idx = atoi(input_buf);
                            DIR *dir = opendir("/usb");
                            if (dir) {
                                struct dirent *entry;
                                int count = 0;
                                while ((entry = readdir(dir)) != NULL) {
                                    if (entry->d_name[0] != '.') {
                                        if (count == target_idx) {
                                            snprintf(target_path, sizeof(target_path), "/usb/%s", entry->d_name);
                                            break;
                                        }
                                        count++;
                                    }
                                }
                                closedir(dir);
                            }
                            if (strlen(target_path) == 0) {
                                dual_printf("ERROR: Index %d not found.\n", target_idx);
                                usb_msc_mount_to_pc();
                                if (was_streaming) streaming_mode = true;
                                break;
                            }
                        } else {
                            if (input_buf[0] == '/') snprintf(target_path, sizeof(target_path), "%s", input_buf);
                            else snprintf(target_path, sizeof(target_path), "/usb/%s", input_buf);
                        }

                        // 2. Open file
                        FILE *f = fopen(target_path, "rb");
                        if (!f) {
                            dual_printf("ERROR: Could not open %s\n", target_path);
                        } else {
                            // Get file size
                            fseek(f, 0, SEEK_END);
                            size_t fsize = ftell(f);
                            fseek(f, 0, SEEK_SET);

                            // Read content
                            uint8_t *file_buf = (uint8_t *)malloc(fsize);
                            if (file_buf) {
                                size_t read_bytes = fread(file_buf, 1, fsize, f);
                                if (read_bytes == fsize) {
                                    // Retrieve file metadata (use default resolution for formatting output)
                                    dual_printf("---START_FILE:4:640:480:%d:%s---\n", (int)fsize, target_path);
                                    usb_cdc_write_base64(file_buf, fsize);
                                    dual_printf("---END_FILE---\n");
                                } else {
                                    dual_printf("ERROR: Incomplete file read: %d of %d\n", (int)read_bytes, (int)fsize);
                                }
                                free(file_buf);
                            } else {
                                dual_printf("ERROR: Out of memory reading file %s\n", target_path);
                            }
                            fclose(f);
                        }

                        usb_msc_mount_to_pc();
                        if (was_streaming) streaming_mode = true;
                        break;
                    }
                    case 'k':
                    case 'K': {
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(50));
                        unlink("/usb/LATEST.RAW");
                        unlink("/usb/LATEST.MET");
                        unlink("/usb/LATEST.BMP");
                        dual_printf("[System] Cleared files in storage partition.\n");
                        usb_msc_mount_to_pc();
                        break;
                    }
                    case 'i':
                    case 'I': {
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera stream task finish sending the current frame
                        }

                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500)); // Give VFS time to transition locks

                        char input_buf[256];
                        strncpy(input_buf, argStr, sizeof(input_buf));
                        input_buf[sizeof(input_buf)-1] = '\0';

                        // Strip trailing spaces/newlines
                        int len = strlen(input_buf);
                        while (len > 0 && (input_buf[len-1] <= ' ')) { input_buf[--len] = '\0'; }

                        if (len == 0) {
                            dual_printf("ERROR: Usage: i <filename> OR i <index>\n");
                            break;
                        }

                        // 1. Resolve to full path
                        char target_path[512] = {0};
                        bool is_index = true;
                        for(int j=0; j<len; j++) { if(!isdigit(input_buf[j])) is_index = false; }

                        if (is_index) {
                            int target_idx = atoi(input_buf);
                            bool was_streaming = streaming_mode;
                            if (was_streaming) {
                                if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) { streaming_mode = false; xSemaphoreGive(camera_mutex); }
                            }
                            usb_msc_mount_to_app();
                            vTaskDelay(pdMS_TO_TICKS(500));

                            DIR *dir = opendir("/usb");
                            if (dir) {
                                struct dirent *entry;
                                int count = 0;
                                while ((entry = readdir(dir)) != NULL) {
                                    if (entry->d_name[0] != '.') {
                                        if (count == target_idx) {
                                            snprintf(target_path, sizeof(target_path), "/usb/%s", entry->d_name);
                                            break;
                                        }
                                        count++;
                                    }
                                }
                                closedir(dir);
                            }
                            if (strlen(target_path) == 0) {
                                dual_printf("ERROR: Index %d not found.\n", target_idx);
                                if (was_streaming) streaming_mode = true;
                                break;
                            }
                        } else {
                            if (input_buf[0] == '/') snprintf(target_path, sizeof(target_path), "%s", input_buf);
                            else snprintf(target_path, sizeof(target_path), "/usb/%s", input_buf);
                        }

                        // 2. Perform Inference

                        FILE *f = fopen(target_path, "rb");
                        if (!f) { dual_printf("ERROR: Could not open %s\n", target_path); if (was_streaming) streaming_mode = true; break; }

                        fseek(f, 0, SEEK_END); size_t fsize = ftell(f); fseek(f, 0, SEEK_SET);
                        uint8_t *file_buf = (uint8_t*)malloc(fsize);
                        if (!file_buf) { fclose(f); if (was_streaming) streaming_mode = true; break; }
                        fread(file_buf, 1, fsize, f); fclose(f);

                        // Parse w/h
                        int w = 640, h = 480;
                        char *w_ptr = strstr(target_path, "_w");
                        char *h_ptr = strstr(target_path, "_h");
                        if (w_ptr && h_ptr) {
                            w = atoi(w_ptr + 2);
                            h = atoi(h_ptr + 2);
                        } else {
                            // Default fallback sizes for latest raw and bmp files
                            if (strstr(target_path, "LATEST.RAW") != nullptr || strstr(target_path, "LATEST.BMP") != nullptr) {
                                w = 160;
                                h = 160;
                            }
                        }

                        if (allocate_frame_buffers()) {
                            bool success = false;

                            // Only allocate the final full-size grayscale buffer
                            uint8_t *full_gray_buf = (uint8_t*)malloc(w * h);

                            if (full_gray_buf) {
                                char lower_path[512];
                                strncpy(lower_path, target_path, sizeof(lower_path));
                                lower_path[sizeof(lower_path)-1] = '\0';
                                for (int k = 0; lower_path[k]; k++) {
                                    lower_path[k] = tolower((unsigned char)lower_path[k]);
                                }

                                if (strstr(lower_path, ".jpg") != nullptr || strstr(lower_path, ".jpeg") != nullptr) {
                                    // Decode JPEG using software decoder
                                    success = decode_jpeg_to_high_fidelity_grayscale(file_buf, fsize, w, h, full_gray_buf);
                                } else if (strstr(lower_path, ".raw") != nullptr || strstr(lower_path, ".gray") != nullptr) {
                                    // Direct load of raw grayscale image data
                                    if (fsize >= (size_t)(w * h)) {
                                        memcpy(full_gray_buf, file_buf, w * h);
                                        success = true;
                                    }
                                } else if (strstr(lower_path, ".bmp") != nullptr) {
                                    // Direct load of 8-bit grayscale BMP image data, skipping the 1078-byte BMP header/palette
                                    if (fsize >= (size_t)(1078 + w * h)) {
                                        memcpy(full_gray_buf, file_buf + 1078, w * h);
                                        success = true;
                                    }
                                }

                                if (success) {
                                    // Use the official resize function to downscale to 96x96
                                    resize_grayscale_to_runtime_frame(full_gray_buf, w, h, g_raw_frame);
                                }
                                free(full_gray_buf); // Free it immediately when done
                            }

                            if (success) {
                                dual_printf("[System] Image decoded and resized to %dx%d Grayscale.\n", FRAME_WIDTH, FRAME_HEIGHT);

                                // Construct a CameraFrame to utilize the storage wrapper
                                CameraFrame inf_frame = {};
                                inf_frame.data = g_raw_frame;
                                inf_frame.size = FRAME_WIDTH * FRAME_HEIGHT;
                                inf_frame.width = FRAME_WIDTH;
                                inf_frame.height = FRAME_HEIGHT;
                                inf_frame.format = CameraFrameFormat::kGrayscale;
                                inf_frame.handle = nullptr; // Not a hardware frame

                                // Automatically saves .raw, .meta, and .bmp!
                                photo_storage_write_latest(inf_frame);

                                dual_printf("[System] Inference started on %s.\n", target_path);
                                run_inference_on_grayscale_frame(g_raw_frame);
                            } else {
                                dual_printf("ERROR: High-fidelity decode failed.\n");
                            }
                        }
                        free(file_buf);
                        if (was_streaming) streaming_mode = true;

                        usb_msc_mount_to_pc();
                        break;
                    }
                    case 'f':
                    case 'F': {
                        int format_val = PIXFORMAT_GRAYSCALE;
                        if (val == 1) format_val = PIXFORMAT_RGB565;
                        else if (val == 2) format_val = PIXFORMAT_YUV422;
                        else if (val == 3) format_val = PIXFORMAT_JPEG;

                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                            vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera hardware breathe before re-init
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
                            vTaskDelay(pdMS_TO_TICKS(100)); // Let the camera hardware breathe before re-init
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
                            g_hmirror = val;
                            dual_printf("[System] Horizontal Mirror updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'p':
                    case 'P': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_vflip(s, val);
                            g_vflip = val;
                            dual_printf("[System] Vertical Flip updated to %d.\n", val);
                        }
                        break;
                    }
                    case 'y':
                    case 'Y': {
                        sensor_t *s = esp_camera_sensor_get();
                        if (s) {
                            s->set_whitebal(s, val);
                            dual_printf("[System] Auto White Balance updated to %d.\n", val);
                        }
                        break;
                    }
                }
                if (lock_acquired) {
                    xSemaphoreGive(camera_mutex);
                }
            }
        }
    }
}
static constexpr int kExpectedTfliteSchemaVersion = 3;

static constexpr int kMaxClassCount = 8;
static constexpr const char *kClassNames[kMaxClassCount] = {
    "up",
    "ok",
    "thumb",
    "palm",
    "rock",
    "stone",
    "class6",
    "class7",
};


static int g_tensor_arena_size = 0;
static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;
static int g_output_class_count = 0;
static const void *g_mapped_model = nullptr;
static esp_partition_mmap_handle_t g_model_mmap_handle = 0;

static void input_controls_monitor_task(void *pvParameters) {
    (void)pvParameters;
    InputControlsSnapshot previous = input_controls_get_snapshot();
    ESP_LOGI(TAG,
             "INPUT_STATUS,encoder=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,enc_level=%d,clk=%d,dt=%d",
             (long)previous.encoder_position,
             (unsigned long)previous.encoder_button_presses,
             (unsigned long)previous.button2_presses,
             previous.button2_level,
             previous.encoder_button_level,
             previous.encoder_clk_level,
             previous.encoder_dt_level);
    while (true) {
        const InputControlsSnapshot current = input_controls_get_snapshot();
        if (current.encoder_position != previous.encoder_position ||
            current.encoder_button_presses != previous.encoder_button_presses ||
            current.button2_presses != previous.button2_presses ||
            current.button2_level != previous.button2_level) {
            const bool shutter_pressed = current.button2_presses != previous.button2_presses;
            dual_printf("INPUT_CONTROL,encoder=%ld,delta=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,clk=%d,dt=%d\n",
                        (long)current.encoder_position,
                        (long)(current.encoder_position - previous.encoder_position),
                        (unsigned long)current.encoder_button_presses,
                        (unsigned long)current.button2_presses,
                        current.button2_level,
                        current.encoder_clk_level,
                        current.encoder_dt_level);
            if (shutter_pressed) {
                usb_cdc_msg_t msg = {};
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "cbtn%lu\n", (unsigned long)current.button2_presses);
                msg.buf_len = strlen(cmd);
                memcpy(msg.buf, cmd, msg.buf_len);
                msg.buf[msg.buf_len] = '\0';
                msg.itf = 0;
                QueueHandle_t q = usb_cdc_get_queue();
                if (!q || xQueueSend(q, &msg, 0) != pdTRUE) {
                    dual_printf("WARN,physical_shutter_queue_full\n");
                } else {
                    dual_printf("[System] Physical shutter queued: capture photo to ESP flash.\n");
                }
            }
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
    ESP_LOGI(TAG, "Robot arm output is controlled by control board through the PC serial bridge.");
}

static const char *runtime_mode_name() {
    switch (RUNTIME_MODE) {
        case RuntimeMode::kInputOutputSelfTest:
            return "INPUT_OUTPUT_SELF_TEST";
        case RuntimeMode::kTestUartFrame:
            return "TEST_MODE_UART_FRAME";
        case RuntimeMode::kPhotoFlashTest:
            return "PHOTO_FLASH_TEST_MODE";
        case RuntimeMode::kCameraFlash:
            return "CAMERA_FLASH_MODE";
        case RuntimeMode::kCameraUsbMsc:
            return "USB_INTERACTIVE_MODE";
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
    if (output_elements <= 0) {
        ESP_LOGE(TAG, "Output tensor has no elements.");
        return false;
    }

    const int class_count = output_elements < kMaxClassCount ? output_elements : kMaxClassCount;
    int best_score = INT32_MIN;
    int best_index = 0;
    for (int i = 0; i < class_count; ++i) {
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
    g_output_class_count = class_count;
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
        resolver.AddMul() != kTfLiteOk ||
        resolver.AddPad() != kTfLiteOk ||
        resolver.AddConcatenation() != kTfLiteOk ||
        resolver.AddDequantize() != kTfLiteOk ||
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

    const int output_elements = tensor_element_count(g_output);
    g_output_class_count = output_elements < kMaxClassCount ? output_elements : kMaxClassCount;
    if (g_output_class_count <= 0) {
        ESP_LOGE(TAG, "Invalid output tensor element count: %d", output_elements);
        return false;
    }
    printf("CLASS_MAP");
    for (int i = 0; i < g_output_class_count; ++i) {
        printf(",%d=%s", i, kClassNames[i]);
    }
    printf("\n");
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

    int scores[kMaxClassCount] = {0};
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
    for (int i = 0; i < g_output_class_count; ++i) {
        dual_printf(",%d", scores[i]);
    }
    dual_printf("\n");
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

static void input_output_self_test_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "Input/output self-test task started.");

    const int sequence[] = {0, 1, 2, 3, 4};
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

        const int action = sequence[index];
        dual_printf("SELFTEST_OUTPUT,action=%d,route=PC_TO_CONTROL_BOARD_REQUIRED\n", action);
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

    // Give the USB Composite task 500ms to mount the FATFS drive
    vTaskDelay(pdMS_TO_TICKS(500));

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

    if (!init_tflite_micro()) {
        ESP_LOGE(TAG, "Halt: TFLite Micro initialization failed.");
        return;
    }

    if (RUNTIME_MODE == RuntimeMode::kTestUartFrame) {
        xTaskCreatePinnedToCore(uart_test_inference_task, "uart_test_inference_task", 16 * 1024, NULL, 5, NULL, 1);
        return;
    }

    if (RUNTIME_MODE == RuntimeMode::kPhotoFlashTest) {
        xTaskCreatePinnedToCore(photo_flash_test_task, "photo_flash_test_task", 16 * 1024, NULL, 5, NULL, 1);
        return;
    }

    // --- BOTH of the remaining modes need the USB drive! ---
    if (RUNTIME_MODE == RuntimeMode::kCameraFlash || RUNTIME_MODE == RuntimeMode::kCameraUsbMsc) {
        usb_composite_init();
        camera_mutex = xSemaphoreCreateMutex();
        configASSERT(camera_mutex);
    }

    if (RUNTIME_MODE == RuntimeMode::kCameraFlash) {
        xTaskCreatePinnedToCore(camera_flash_task, "camera_flash_task", 16 * 1024, NULL, 5, NULL, 1);
        return; // Added missing return!
    }

    if (RUNTIME_MODE == RuntimeMode::kCameraUsbMsc) {
        // 1. Give the USB Composite task 500ms to mount the FATFS drive
        vTaskDelay(pdMS_TO_TICKS(500));

        // 2. Initialize the file system and camera hardware
        esp_err_t err = photo_storage_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Photo storage init failed: %s", esp_err_to_name(err));
        }

        err = camera_capture_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        }

        // 3. Create camera streaming and command parsing tasks
        xTaskCreatePinnedToCore(camera_stream_task, "camera_stream_task", 8192, NULL, 1, NULL, 1);
        xTaskCreatePinnedToCore(usb_cdc_command_task, "usb_cdc_command_task", 8192, NULL, 5, NULL, 0);
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
