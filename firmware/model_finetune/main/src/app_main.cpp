#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "camera_capture.hpp"
#include "model_config.hpp"
#include "photo_storage.hpp"
#include "usb_composite.hpp"

static const char *TAG = "MODEL_FINETUNE_FW";

static SemaphoreHandle_t g_camera_mutex = nullptr;
static bool g_streaming = false;
static int g_format_code = CAMERA_USB_DEFAULT_PIXEL_FORMAT;
static int g_size_code = CAMERA_USB_DEFAULT_FRAME_SIZE;

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

static int protocol_format(const CameraFrame &frame) {
    switch (frame.format) {
        case CameraFrameFormat::kRgb565: return 0;
        case CameraFrameFormat::kYuv422: return 1;
        case CameraFrameFormat::kGrayscale: return 3;
        case CameraFrameFormat::kJpeg: return 4;
        default: return 4;
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

static void send_frame_to_pc(const CameraFrame &frame) {
    cdc_printf("---START_IMAGE:%d:%d:%d:%d---\n",
               protocol_format(frame), frame.width, frame.height, (int)frame.size);
    usb_cdc_write_base64(frame.data, frame.size);
    cdc_printf("---END_IMAGE---\n");
}

static esp_err_t apply_camera_config_locked() {
    return camera_capture_reinit((int)format_from_code(g_format_code), (int)size_from_code(g_size_code));
}

static void set_streaming(bool enabled) {
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        g_streaming = enabled;
        xSemaphoreGive(g_camera_mutex);
    }
    cdc_printf("[ModelFinetune] Streaming: %s\n", enabled ? "ENABLED" : "DISABLED");
}

static void camera_stream_task(void *pv) {
    (void)pv;
    while (true) {
        bool do_stream = false;
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            do_stream = g_streaming;
            if (do_stream) {
                CameraFrame frame = {};
                esp_err_t err = camera_capture_frame(&frame);
                if (err == ESP_OK) {
                    send_frame_to_pc(frame);
                    camera_capture_release(&frame);
                } else {
                    cdc_printf("ERROR,camera_capture,%s\n", esp_err_to_name(err));
                }
            }
            xSemaphoreGive(g_camera_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(do_stream ? CAMERA_USB_CAPTURE_INTERVAL_MS : 50));
    }
}

static esp_err_t capture_once(bool send_to_pc, bool save_to_storage) {
    esp_err_t err = ESP_OK;
    bool was_streaming = false;
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cdc_printf("ERROR,camera_busy\n");
        return ESP_ERR_TIMEOUT;
    }
    was_streaming = g_streaming;
    g_streaming = false;
    CameraFrame frame = {};
    err = camera_capture_frame(&frame);
    if (err == ESP_OK) {
        if (send_to_pc) send_frame_to_pc(frame);
        if (save_to_storage) {
            usb_msc_mount_to_app();
            vTaskDelay(pdMS_TO_TICKS(100));
            err = photo_storage_write_latest(frame);
            if (err == ESP_OK) {
                cdc_printf("[ModelFinetune] Saved latest frame to storage.\n");
            } else {
                cdc_printf("ERROR,photo_storage_write,%s\n", esp_err_to_name(err));
            }
            usb_msc_mount_to_pc();
        }
        camera_capture_release(&frame);
    } else {
        cdc_printf("ERROR,camera_capture,%s\n", esp_err_to_name(err));
    }
    g_streaming = was_streaming;
    xSemaphoreGive(g_camera_mutex);
    return err;
}

static void reconfigure_camera() {
    bool was_streaming = false;
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cdc_printf("ERROR,camera_busy\n");
        return;
    }
    was_streaming = g_streaming;
    g_streaming = false;
    esp_err_t err = apply_camera_config_locked();
    g_streaming = was_streaming;
    xSemaphoreGive(g_camera_mutex);
    if (err == ESP_OK) {
        cdc_printf("[ModelFinetune] Camera config ok: f%d s%d\n", g_format_code, g_size_code);
    } else {
        cdc_printf("ERROR,camera_config,%s\n", esp_err_to_name(err));
    }
}

static void list_storage_files() {
    usb_msc_mount_to_app();
    vTaskDelay(pdMS_TO_TICKS(100));
    cdc_printf("[ModelFinetune] Storage list is available through MSC on the PC.\n");
    usb_msc_mount_to_pc();
}

static void handle_command(char *cmd) {
    while (*cmd && isspace((unsigned char)*cmd)) ++cmd;
    size_t n = strlen(cmd);
    while (n > 0 && isspace((unsigned char)cmd[n - 1])) {
        cmd[--n] = '\0';
    }
    if (!*cmd) return;

    if (strcasecmp(cmd, "h") == 0 || strcasecmp(cmd, "help") == 0) {
        cdc_printf("Commands: d1/d0 stream, c capture-to-PC, w save latest, usb expose MSC, f0..f3 format, s0..s5 size, reboot\n");
        return;
    }
    if (strcasecmp(cmd, "reboot") == 0) {
        cdc_printf("[ModelFinetune] Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }
    if (strcasecmp(cmd, "usb") == 0) {
        set_streaming(false);
        usb_msc_mount_to_pc();
        cdc_printf("[ModelFinetune] USB MSC exposed to PC.\n");
        return;
    }
    if (strcasecmp(cmd, "format") == 0) {
        set_streaming(false);
        usb_msc_mount_to_app();
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
        case 'd': set_streaming(value != 0); break;
        case 'c': capture_once(true, false); break;
        case 'w': capture_once(false, true); break;
        case 'l': list_storage_files(); break;
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
    while (true) {
        if (xQueueReceive(q, &msg, portMAX_DELAY) == pdTRUE && msg.buf_len > 0) {
            size_t len = msg.buf_len;
            if (len > CONFIG_TINYUSB_CDC_RX_BUFSIZE) len = CONFIG_TINYUSB_CDC_RX_BUFSIZE;
            memcpy(cmd, msg.buf, len);
            cmd[len] = '\0';
            handle_command(cmd);
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Runtime: model finetune camera-only firmware.");
    g_camera_mutex = xSemaphoreCreateMutex();
    if (!g_camera_mutex) {
        ESP_LOGE(TAG, "Failed to create camera mutex.");
        return;
    }
    usb_composite_init();
    photo_storage_init();
    esp_err_t err = camera_capture_reinit((int)format_from_code(g_format_code), (int)size_from_code(g_size_code));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OV2640 init failed: %s", esp_err_to_name(err));
    }
    cdc_printf("READY,MODEL_FINETUNE_CAMERA,f%d,s%d\n", g_format_code, g_size_code);
    xTaskCreatePinnedToCore(camera_stream_task, "camera_stream_task", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(usb_command_task, "usb_command_task", 8192, NULL, 5, NULL, 0);
}
