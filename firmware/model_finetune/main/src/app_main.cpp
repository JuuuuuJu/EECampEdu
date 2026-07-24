#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "camera_capture.hpp"
#include "input_controls.hpp"
#include "model_config.hpp"
#include "photo_storage.hpp"
#include "usb_composite.hpp"
#include "usb_descriptors.h"

static const char *TAG = "MODEL_FINETUNE_FW";

static SemaphoreHandle_t g_camera_mutex = nullptr;

// Video is delivered to the PC as a real UVC (webcam) MJPEG stream, so the
// camera runs in JPEG at the UVC-advertised size. The CDC channel carries only
// control (commands, RESULT/INPUT lines). f/s commands can still retune the
// sensor, but leaving JPEG + QVGA keeps the UVC stream valid.
static int g_format_code = 3;  // JPEG (see model_config.hpp command mapping)
static int g_size_code = 2;    // QVGA 320x240 -> matches UVC_FRAME_WIDTH/HEIGHT

// JPEG staging buffer for the UVC endpoint. Must stay valid until the frame has
// finished transmitting, so we only refill it while uvc_video_can_send() is true.
static uint8_t *g_uvc_buf = nullptr;
static size_t g_uvc_buf_cap = 0;

static pixformat_t format_from_code(int code) {
    switch (code) {
        case 0: return PIXFORMAT_GRAYSCALE;
        case 1: return PIXFORMAT_RGB565;
        case 2: return PIXFORMAT_YUV422;
        case 3: return PIXFORMAT_JPEG;
        default: return PIXFORMAT_JPEG;
    }
}

static framesize_t size_from_code(int code) {
    switch (code) {
        case 0: return FRAMESIZE_96X96;
        case 1: return FRAMESIZE_QQVGA;
        case 2: return FRAMESIZE_QVGA;
        case 3: return FRAMESIZE_VGA;
        case 4: return FRAMESIZE_SVGA;
        case 5: return FRAMESIZE_UXGA;
        default: return FRAMESIZE_VGA;
    }
}

static void cdc_printf(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    printf("%s", buffer);
    fflush(stdout);
    usb_cdc_printf("%s", buffer);
}

static esp_err_t apply_camera_config_locked() {
    return camera_capture_reinit((int)format_from_code(g_format_code), (int)size_from_code(g_size_code));
}

static esp_err_t apply_camera_config_with_fallback_locked(const char *reason) {
    struct Candidate {
        int format_code;
        int size_code;
        const char *name;
    };
    const Candidate candidates[] = {
        {g_format_code, g_size_code, "requested"},
        {3, 2, "fallback jpeg qvga"},
        {3, 1, "fallback jpeg qqvga"},
        {0, 0, "fallback grayscale 96x96"},
    };

    esp_err_t last_err = ESP_FAIL;
    int tried_format = -1;
    int tried_size = -1;
    for (const Candidate &candidate : candidates) {
        if (candidate.format_code == tried_format && candidate.size_code == tried_size) {
            continue;
        }
        tried_format = candidate.format_code;
        tried_size = candidate.size_code;
        g_format_code = candidate.format_code;
        g_size_code = candidate.size_code;
        last_err = apply_camera_config_locked();
        if (last_err == ESP_OK) {
            cdc_printf("[ModelFinetune] Camera config ok: f%d s%d (%s, %s)\n",
                       g_format_code, g_size_code, candidate.name, reason);
            return ESP_OK;
        }
        cdc_printf("WARN,camera_config_failed,f%d,s%d,%s,%s\n",
                   g_format_code, g_size_code, candidate.name, esp_err_to_name(last_err));
    }
    cdc_printf("ERROR,camera_config_all_failed,%s\n", esp_err_to_name(last_err));
    return last_err;
}

// UVC streaming task: whenever a host has the stream open and the previous
// frame has drained, grab a JPEG frame, stage it, and hand it to the UVC EP.
static void uvc_stream_task(void *pv) {
    (void)pv;
    const TickType_t frame_period = pdMS_TO_TICKS(1000 / UVC_FRAME_RATE);
    while (true) {
        if (!uvc_video_can_send()) {
            vTaskDelay(pdMS_TO_TICKS(uvc_video_ready() ? 2 : 50));
            continue;
        }
        size_t staged = 0;
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            CameraFrame frame = {};
            esp_err_t err = camera_capture_frame(&frame);
            if (err == ESP_OK) {
                if (frame.format == CameraFrameFormat::kJpeg &&
                    frame.size > 0 && frame.size <= g_uvc_buf_cap) {
                    memcpy(g_uvc_buf, frame.data, frame.size);
                    staged = frame.size;
                }
                camera_capture_release(&frame);
            } else {
                cdc_printf("ERROR,camera_capture,%s\n", esp_err_to_name(err));
            }
            xSemaphoreGive(g_camera_mutex);
        }
        if (staged > 0) {
            uvc_video_submit_frame(g_uvc_buf, staged);
        }
        vTaskDelay(frame_period);
    }
}

// Capture the current frame and save it to on-device flash storage (/usb).
// With MSC removed, the app side always owns the filesystem, so this is a plain
// write; students later pull photos over the portal (or a future CDC download).
static esp_err_t capture_and_save() {
    esp_err_t err;
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cdc_printf("ERROR,camera_busy\n");
        return ESP_ERR_TIMEOUT;
    }
    CameraFrame frame = {};
    err = camera_capture_frame(&frame);
    if (err != ESP_OK) {
        cdc_printf("WARN,camera_capture_retry,%s\n", esp_err_to_name(err));
        err = apply_camera_config_with_fallback_locked("capture retry");
        if (err == ESP_OK) {
            err = camera_capture_frame(&frame);
        }
    }
    if (err == ESP_OK) {
        err = photo_storage_write_capture(frame, "model_finetune");
        if (err == ESP_OK) {
            cdc_printf("[ModelFinetune] Saved capture frame to ESP flash.\n");
        } else {
            cdc_printf("ERROR,photo_storage_write,%s,%s\n", esp_err_to_name(err), photo_storage_last_error());
        }
        camera_capture_release(&frame);
    } else {
        cdc_printf("ERROR,camera_capture,%s\n", esp_err_to_name(err));
    }
    xSemaphoreGive(g_camera_mutex);
    return err;
}

static void reconfigure_camera() {
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cdc_printf("ERROR,camera_busy\n");
        return;
    }
    esp_err_t err = apply_camera_config_with_fallback_locked("manual config");
    xSemaphoreGive(g_camera_mutex);
    if (err != ESP_OK) {
        cdc_printf("ERROR,camera_config,%s\n", esp_err_to_name(err));
    }
}

static void handle_command(char *cmd) {
    while (*cmd && isspace((unsigned char)*cmd)) ++cmd;
    size_t n = strlen(cmd);
    while (n > 0 && isspace((unsigned char)cmd[n - 1])) {
        cmd[--n] = '\0';
    }
    if (!*cmd) return;

    if (strcasecmp(cmd, "h") == 0 || strcasecmp(cmd, "help") == 0) {
        cdc_printf("Commands: c/w capture-to-flash, input status, f0..f3 format, s0..s5 size, format erase-storage, reboot. Live video is the UVC webcam.\n");
        return;
    }
    if (strcasecmp(cmd, "input") == 0) {
        const InputControlsSnapshot snapshot = input_controls_get_snapshot();
        cdc_printf("INPUT_STATUS,encoder=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,enc_level=%d,clk=%d,dt=%d\n",
                   (long)snapshot.encoder_position,
                   (unsigned long)snapshot.encoder_button_presses,
                   (unsigned long)snapshot.button2_presses,
                   snapshot.button2_level,
                   snapshot.encoder_button_level,
                   snapshot.encoder_clk_level,
                   snapshot.encoder_dt_level);
        return;
    }
    if (strcasecmp(cmd, "reboot") == 0) {
        cdc_printf("[ModelFinetune] Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }
    if (strcasecmp(cmd, "format") == 0) {
        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
        if (part && esp_partition_erase_range(part, 0, part->size) == ESP_OK) {
            cdc_printf("[ModelFinetune] Storage erased. Rebooting to rebuild FAT.\n");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        cdc_printf("ERROR,format_failed\n");
        return;
    }

    char action = (char)tolower((unsigned char)cmd[0]);
    int value = atoi(cmd + 1);
    switch (action) {
        case 'c': capture_and_save(); break;
        case 'w': capture_and_save(); break;
        case 'e': {
            sensor_t *sensor = esp_camera_sensor_get();
            if (sensor) {
                sensor->set_exposure_ctrl(sensor, value);
                cdc_printf("[ModelFinetune] Exposure Control updated to %d.\n", value);
            }
            break;
        }
        case 'v': {
            sensor_t *sensor = esp_camera_sensor_get();
            if (sensor) {
                sensor->set_aec_value(sensor, value);
                cdc_printf("[ModelFinetune] Manual Exposure AEC value updated to %d.\n", value);
            }
            break;
        }
        case 'f':
            if (value < 0 || value > 3) { cdc_printf("ERROR,bad_format\n"); break; }
            g_format_code = value;
            reconfigure_camera();
            break;
        case 's':
            if (value < 0 || value > 5) { cdc_printf("ERROR,bad_size\n"); break; }
            g_size_code = value;
            reconfigure_camera();
            break;
        case 'd':
            // Legacy stream toggle: video is now the host-controlled UVC stream.
            cdc_printf("[ModelFinetune] Live video is the UVC webcam; open it from the app.\n");
            break;
        default:
            cdc_printf("ERROR,unknown_command,%s\n", cmd);
            break;
    }
}

static void usb_command_task(void *pv) {
    (void)pv;
    usb_cdc_msg_t msg;
    QueueHandle_t q = usb_cdc_get_queue();
    char cmd[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t cmd_len = 0;
    while (true) {
        if (xQueueReceive(q, &msg, portMAX_DELAY) == pdTRUE && msg.buf_len > 0) {
            for (size_t i = 0; i < msg.buf_len; ++i) {
                const char ch = static_cast<char>(msg.buf[i]);
                if (ch == '\r' || ch == '\n') {
                    cmd[cmd_len] = '\0';
                    if (cmd_len > 0) {
                        handle_command(cmd);
                    }
                    cmd_len = 0;
                    continue;
                }
                if (cmd_len < CONFIG_TINYUSB_CDC_RX_BUFSIZE) {
                    cmd[cmd_len++] = ch;
                } else {
                    cmd_len = 0;
                    cdc_printf("ERROR,command_too_long\n");
                }
            }
        }
    }
}

// Report rotary-encoder / button activity over CDC as INPUT_CONTROL lines, the
// same protocol main_board uses. The browser can map encoder steps to camera
// controls; this firmware itself only reports.
static void input_controls_monitor_task(void *pv) {
    (void)pv;
    InputControlsSnapshot previous = input_controls_get_snapshot();
    cdc_printf("INPUT_STATUS,encoder=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,enc_level=%d,clk=%d,dt=%d\n",
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
            cdc_printf("INPUT_CONTROL,encoder=%ld,delta=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,clk=%d,dt=%d\n",
                       (long)current.encoder_position,
                       (long)(current.encoder_position - previous.encoder_position),
                       (unsigned long)current.encoder_button_presses,
                       (unsigned long)current.button2_presses,
                       current.button2_level,
                       current.encoder_clk_level,
                       current.encoder_dt_level);
            if (shutter_pressed) {
                cdc_printf("[ModelFinetune] Physical shutter pressed.\n");
            }
            previous = current;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Runtime: model finetune camera firmware (UVC video + CDC control).");
    g_camera_mutex = xSemaphoreCreateMutex();
    if (!g_camera_mutex) {
        ESP_LOGE(TAG, "Failed to create camera mutex.");
        return;
    }

    // Staging buffer for one JPEG frame handed to the UVC endpoint. QVGA MJPEG
    // frames are a few KB; size the buffer to the descriptor's max frame size.
    g_uvc_buf_cap = UVC_FRAME_WIDTH * UVC_FRAME_HEIGHT * 2;
    g_uvc_buf = (uint8_t *)heap_caps_malloc(g_uvc_buf_cap, MALLOC_CAP_SPIRAM);
    if (!g_uvc_buf) {
        g_uvc_buf = (uint8_t *)heap_caps_malloc(g_uvc_buf_cap, MALLOC_CAP_DEFAULT);
    }
    if (!g_uvc_buf) {
        ESP_LOGE(TAG, "Failed to allocate UVC JPEG buffer (%u bytes).", (unsigned)g_uvc_buf_cap);
        return;
    }

    usb_composite_init();
    photo_storage_init();

    esp_err_t err = ESP_OK;
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        err = apply_camera_config_with_fallback_locked("boot");
        xSemaphoreGive(g_camera_mutex);
    } else {
        err = ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OV2640 init failed after fallback: %s", esp_err_to_name(err));
    }

    if (ENABLE_INPUT_CONTROLS) {
        esp_err_t in_err = input_controls_init();
        if (in_err != ESP_OK) {
            ESP_LOGW(TAG, "Input controls init failed: %s", esp_err_to_name(in_err));
        } else {
            xTaskCreatePinnedToCore(input_controls_monitor_task, "input_monitor", 4096, NULL, 3, NULL, 0);
        }
    }

    cdc_printf("READY,MODEL_FINETUNE_CAMERA,f%d,s%d,uvc=%dx%d\n",
               g_format_code, g_size_code, UVC_FRAME_WIDTH, UVC_FRAME_HEIGHT);
    xTaskCreatePinnedToCore(uvc_stream_task, "uvc_stream_task", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(usb_command_task, "usb_command_task", 8192, NULL, 5, NULL, 0);
}
