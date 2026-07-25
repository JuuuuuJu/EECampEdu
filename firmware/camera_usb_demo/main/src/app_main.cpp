#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "camera_capture.hpp"
#include "input_controls.hpp"
#include "model_config.hpp"
#include "photo_storage.hpp"
#include "usb_composite.hpp"
#include "usb_descriptors.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "CAMERA_USB_DEMO";

struct CameraMode {
    framesize_t size;
    uint16_t width;
    uint16_t height;
};

// Must stay in the same order as the UVC frame descriptors (frame index = mode + 1).
static constexpr CameraMode kCameraModes[UVC_MODE_COUNT] = {
    { FRAMESIZE_QQVGA, 160, 120 }, { FRAMESIZE_QVGA, 320, 240 },
    { FRAMESIZE_VGA, 640, 480 }, { FRAMESIZE_SVGA, 800, 600 },
    { FRAMESIZE_UXGA, 1600, 1200 },
};

static pixformat_t g_current_format = PIXFORMAT_JPEG;
static framesize_t g_current_size = kCameraModes[UVC_DEFAULT_MODE].size;
static volatile uint8_t g_current_mode = UVC_DEFAULT_MODE;
static volatile uint8_t g_requested_fps = UVC_DEFAULT_FPS;

extern int g_hmirror;
extern int g_vflip;

static SemaphoreHandle_t camera_mutex = NULL;

// JPEG staging buffer for the UVC endpoint. Must stay valid until the frame has
// finished transmitting, so we only refill it while uvc_video_can_send() is true.
static uint8_t *g_uvc_buf = nullptr;
static size_t g_uvc_buf_cap = 0;
// Keep status lines out of a base64 CDC transfer: any injected text corrupts it.
static volatile bool g_cdc_file_transfer = false;

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

class StorageAppGuard {
public:
    explicit StorageAppGuard(TickType_t timeout = pdMS_TO_TICKS(2000))
        : held_(usb_storage_acquire_app(timeout)) {}
    ~StorageAppGuard() {
        if (held_) {
            usb_storage_release_app();
        }
    }
    explicit operator bool() const { return held_; }

private:
    bool held_;
};

static void report_storage_not_owned() {
    dual_printf("ERROR,storage_owned_by_pc,eject_usb_drive_first\n");
}

static void usb_list_files_recursive(const char *directory, int *count,
                                     size_t *total_size) {
    DIR *dir = opendir(directory);
    if (!dir) {
        dual_printf("ERROR: Failed to open %s.\n", directory);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(filepath, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            usb_list_files_recursive(filepath, count, total_size);
        } else {
            usb_cdc_printf("FILE,path=%s,size=%d\n", filepath, (int)st.st_size);
            *total_size += st.st_size;
            (*count)++;
        }
    }
    closedir(dir);
}

static void usb_clear_files_recursive(const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(filepath, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            usb_clear_files_recursive(filepath);
            rmdir(filepath);
        } else {
            unlink(filepath);
        }
    }
    closedir(dir);
}

static void usb_list_files() {
    StorageAppGuard storage;
    if (!storage) {
        report_storage_not_owned();
        return;
    }

    int count = 0;
    size_t total_size = 0;
    usb_list_files_recursive("/usb", &count, &total_size);
    usb_cdc_printf("FILE_LIST_END,count=%d,total=%d\n", count, (int)total_size);
}

// UVC streaming task. The requested frame rate is a pacing target, not a
// promise: USB and JPEG pressure can drop frames, which is reported once/second.
static void uvc_stream_task(void *pv) {
    (void)pv;
    int64_t metric_start_us = esp_timer_get_time();
    unsigned delivered = 0, dropped = 0;
    while (true) {
        // CDC file download gets priority over the isochronous preview stream.
        if (g_cdc_file_transfer) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const uint8_t requested_fps = g_requested_fps ? g_requested_fps : 1;
        const TickType_t frame_period = pdMS_TO_TICKS((1000 + requested_fps - 1) / requested_fps);
        if (!uvc_video_can_send()) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - metric_start_us >= 1000000 && !g_cdc_file_transfer) {
                const CameraMode &mode = kCameraModes[g_current_mode];
                dual_printf("UVC,mode=%u,width=%u,height=%u,requested_fps=%u,delivered_fps=%u,dropped=%u\n",
                            (unsigned)g_current_mode, (unsigned)mode.width, (unsigned)mode.height,
                            (unsigned)g_requested_fps, delivered, dropped);
                metric_start_us = now_us;
                delivered = 0;
                dropped = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(uvc_video_ready() ? 2 : 50));
            continue;
        }
        size_t staged = 0;
        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            CameraFrame frame = {};
            esp_err_t err = camera_capture_frame(&frame);
            if (err == ESP_OK) {
                if (frame.format == CameraFrameFormat::kJpeg && frame.size > 0 && frame.size <= g_uvc_buf_cap) {
                    memcpy(g_uvc_buf, frame.data, frame.size);
                    staged = frame.size;
                } else {
                    dropped++;
                }
                camera_capture_release(&frame);
            } else {
                dropped++;
            }
            xSemaphoreGive(camera_mutex);
        } else {
            dropped++;
        }
        if (staged > 0 && uvc_video_submit_frame(g_uvc_buf, staged)) {
            delivered++;
        } else if (staged > 0) {
            dropped++;
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - metric_start_us >= 1000000 && !g_cdc_file_transfer) {
            const CameraMode &mode = kCameraModes[g_current_mode];
            dual_printf("UVC,mode=%u,width=%u,height=%u,requested_fps=%u,delivered_fps=%u,dropped=%u\n",
                        (unsigned)g_current_mode, (unsigned)mode.width, (unsigned)mode.height,
                        (unsigned)g_requested_fps, delivered, dropped);
            metric_start_us = now_us;
            delivered = 0;
            dropped = 0;
        }
        vTaskDelay(frame_period);
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
                memcpy(cmd_buf, msg.buf, msg.buf_len);
                cmd_buf[msg.buf_len] = '\0';

                while (msg.buf_len > 0 && (cmd_buf[msg.buf_len - 1] == '\n' || cmd_buf[msg.buf_len - 1] == '\r')) {
                    cmd_buf[msg.buf_len - 1] = '\0';
                    msg.buf_len--;
                }

                if (strlen(cmd_buf) == 0) continue;

                if (strcasecmp(cmd_buf, "format") == 0) {
                    StorageAppGuard storage;
                    if (!storage) {
                        report_storage_not_owned();
                        continue;
                    }
                    dual_printf("[System] Erasing storage partition...\n");
                    const esp_partition_t *part = esp_partition_find_first(
                        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
                    if (part && esp_partition_erase_range(part, 0, part->size) == ESP_OK) {
                        dual_printf("[System] Storage erased. Rebooting to rebuild FAT.\n");
                        vTaskDelay(pdMS_TO_TICKS(300));
                        esp_restart();
                    }
                    dual_printf("ERROR: format_failed\n");
                    continue;
                }

                if (strcasecmp(cmd_buf, "storage status") == 0) {
                    dual_printf("STORAGE,owner=%s,capture=%s\n",
                                usb_storage_owner_name(),
                                usb_storage_is_app_owned() ? "ready" : "blocked");
                    continue;
                }

                if (strcasecmp(cmd_buf, "usb") == 0 ||
                    strcasecmp(cmd_buf, "storage pc") == 0) {
                    if (!usb_storage_is_app_owned()) {
                        dual_printf("STORAGE,owner=pc,capture=blocked\n");
                        continue;
                    }
                    dual_printf("[System] Flushing captures and transferring storage to the PC...\n");
                    esp_err_t storage_err = usb_storage_request_pc();
                    if (storage_err == ESP_OK) {
                        dual_printf("STORAGE,owner=pc,capture=blocked\n");
                    } else {
                        dual_printf("ERROR: Failed to transfer storage to PC: %s\n",
                                    esp_err_to_name(storage_err));
                    }
                    continue;
                }

                if (strcasecmp(cmd_buf, "storage app") == 0) {
                    dual_printf("ERROR,use_pc_eject_to_transfer_storage_to_app\n");
                    continue;
                }

                char act = cmd_buf[0];
                char act_upper = toupper((unsigned char)act);
                const char* argStr = cmd_buf + 1;
                while (*argStr && isspace((unsigned char)*argStr)) {
                    ++argStr;
                }
                int val = atoi(argStr);

                bool lock_acquired = false;
                if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    lock_acquired = true;
                }

                if (lock_acquired) {
                    switch (act_upper) {
                        case 'F': {
                            int fmt_val = PIXFORMAT_JPEG;
                            if (val == 0) fmt_val = PIXFORMAT_GRAYSCALE;
                            else if (val == 1) fmt_val = PIXFORMAT_RGB565;
                            else if (val == 2) fmt_val = PIXFORMAT_YUV422;

                            esp_err_t err = camera_capture_reinit(fmt_val, (int)g_current_size);
                            if (err == ESP_OK) {
                                g_current_format = (pixformat_t)fmt_val;
                                dual_printf("[System] Pixel Format updated successfully.\n");
                                if (fmt_val != PIXFORMAT_JPEG) {
                                    dual_printf("[System] Note: the UVC live preview only carries JPEG; it will freeze until you switch back to f3.\n");
                                }
                            } else {
                                dual_printf("ERROR: Reinit format failed: %s\n", esp_err_to_name(err));
                            }
                            break;
                        }
                        case 'S': {
                            const int mode_index = (val >= 0 && val < UVC_MODE_COUNT) ? val : UVC_DEFAULT_MODE;
                            const CameraMode &mode = kCameraModes[mode_index];
                            esp_err_t err = camera_capture_reinit((int)g_current_format, (int)mode.size);
                            if (err == ESP_OK) {
                                g_current_size = mode.size;
                                g_current_mode = mode_index;
                                dual_printf("UVC,settings=resolution,mode=%d,width=%u,height=%u\n",
                                            mode_index, (unsigned)mode.width, (unsigned)mode.height);
                            } else {
                                dual_printf("ERROR: Reinit resolution failed: %s\n", esp_err_to_name(err));
                            }
                            break;
                        }
                        case 'N': {
                            g_requested_fps = (uint8_t)((val < 1) ? 1 : (val > UVC_MAX_FPS) ? UVC_MAX_FPS : val);
                            const CameraMode &mode = kCameraModes[g_current_mode];
                            dual_printf("UVC,settings=fps,mode=%u,width=%u,height=%u,requested_fps=%u\n",
                                        (unsigned)g_current_mode, (unsigned)mode.width, (unsigned)mode.height,
                                        (unsigned)g_requested_fps);
                            break;
                        }
                        case 'E': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_exposure_ctrl(s, val);
                                dual_printf("[System] Exposure Control updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'G': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_gain_ctrl(s, val);
                                dual_printf("[System] Gain Control updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'V': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_aec_value(s, val);
                                dual_printf("[System] Manual Exposure AEC value updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'A': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_agc_gain(s, val);
                                dual_printf("[System] Manual Gain AGC value updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'B': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_brightness(s, val);
                                dual_printf("[System] Brightness updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'T': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_contrast(s, val);
                                dual_printf("[System] Contrast updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'X': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_saturation(s, val);
                                dual_printf("[System] Saturation updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'M': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_hmirror(s, val);
                                g_hmirror = val;
                                dual_printf("[System] Horizontal Mirror updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'P': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_vflip(s, val);
                                g_vflip = val;
                                dual_printf("[System] Vertical Flip updated to %d.\n", val);
                            }
                            break;
                        }
                        case 'Y': {
                            sensor_t *s = esp_camera_sensor_get();
                            if (s) {
                                s->set_whitebal(s, val);
                                dual_printf("[System] Auto White Balance updated to %d.\n", val);
                            }
                            break;
                        }
                    }
                    xSemaphoreGive(camera_mutex);
                }

                // Commands parsed outside the camera lock (pure filesystem ops).
                switch (act_upper) {
                    case 'D': {
                        dual_printf("[System] Live video is the UVC webcam; open it from the app.\n");
                        break;
                    }
                    case 'C': // Map C/c to behave like w/W
                    case 'W': {
                        StorageAppGuard storage;
                        if (!storage) {
                            report_storage_not_owned();
                            break;
                        }
                        const char *ts = (strlen(argStr) > 0) ? argStr : "manual";
                        const char *capture_dir = "/usb";
                        uint8_t *jpg_buf = nullptr;
                        size_t jpg_len = 0;
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                            dual_printf("ERROR: Camera busy, could not acquire mutex.\n");
                            break;
                        }
                        const pixformat_t restore_format = g_current_format;
                        const framesize_t restore_size = g_current_size;
                        esp_err_t err = camera_capture_reinit(PIXFORMAT_GRAYSCALE, FRAMESIZE_96X96);
                        camera_fb_t *fb = nullptr;
                        if (err == ESP_OK) {
                            // Discard one frame after a sensor reconfiguration.
                            camera_fb_t *stale = esp_camera_fb_get();
                            if (stale) esp_camera_fb_return(stale);
                            fb = esp_camera_fb_get();
                        }
                        if (fb && frame2jpg(fb, 85, &jpg_buf, &jpg_len)) {
                            esp_camera_fb_return(fb);
                            fb = nullptr;
                        } else {
                            if (fb) esp_camera_fb_return(fb);
                            jpg_buf = nullptr;
                            jpg_len = 0;
                        }
                        esp_err_t restore_err = camera_capture_reinit(restore_format, restore_size);
                        xSemaphoreGive(camera_mutex);
                        if (!jpg_buf || jpg_len == 0 || restore_err != ESP_OK) {
                            if (jpg_buf) free(jpg_buf);
                            dual_printf("ERROR: 96x96 grayscale capture failed; live mode was restored=%s.\n",
                                        esp_err_to_name(restore_err));
                            break;
                        }
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "%s/img_%.128s_gray_96x96.jpg", capture_dir, ts);
                        FILE *f = fopen(filepath, "wb");
                        if (!f) {
                            dual_printf("ERROR: Failed to open %s for writing.\n", filepath);
                        } else {
                            size_t written = fwrite(jpg_buf, 1, jpg_len, f);
                            fclose(f);
                            if (written == jpg_len) {
                                dual_printf("[System] Image saved to flash! File: %s (%d bytes, 96x96 grayscale JPEG)\n", filepath, (int)written);
                            } else {
                                dual_printf("ERROR: Incomplete file write. Only wrote %d of %d bytes\n", (int)written, (int)jpg_len);
                            }
                        }
                        free(jpg_buf);
                        break;
                    }
                    case 'L': {
                        usb_list_files();
                        break;
                    }
                    case 'R': {
                        StorageAppGuard storage;
                        if (!storage) {
                            report_storage_not_owned();
                            break;
                        }
                        char input_buf[256];
                        strncpy(input_buf, argStr, sizeof(input_buf));
                        input_buf[sizeof(input_buf)-1] = '\0';
                        int len = strlen(input_buf);
                        while (len > 0 && (input_buf[len-1] <= ' ')) { input_buf[--len] = '\0'; }

                        if (len == 0) {
                            dual_printf("ERROR: Usage: r <filename> OR r <index>\n");
                            break;
                        }

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
                                break;
                            }
                        } else {
                            if (input_buf[0] == '/') snprintf(target_path, sizeof(target_path), "%s", input_buf);
                            else snprintf(target_path, sizeof(target_path), "/usb/%s", input_buf);
                        }

                        FILE *f = fopen(target_path, "rb");
                        if (!f) {
                            dual_printf("ERROR: Could not open %s\n", target_path);
                        } else {
                            fseek(f, 0, SEEK_END);
                            size_t fsize = ftell(f);
                            fseek(f, 0, SEEK_SET);

                            uint8_t *file_buf = (uint8_t *)malloc(fsize);
                            if (file_buf) {
                                size_t read_bytes = fread(file_buf, 1, fsize, f);
                                if (read_bytes == fsize) {
                                    g_cdc_file_transfer = true;
                                    dual_printf("---START_FILE:4:640:480:%d:%s---\n", (int)fsize, target_path);
                                    usb_cdc_write_base64(file_buf, fsize);
                                    dual_printf("---END_FILE---\n");
                                    g_cdc_file_transfer = false;
                                } else {
                                    dual_printf("ERROR: Incomplete file read: %d of %d\n", (int)read_bytes, (int)fsize);
                                }
                                free(file_buf);
                            } else {
                                dual_printf("ERROR: Out of memory reading file %s\n", target_path);
                            }
                            fclose(f);
                        }
                        break;
                    }
                    case 'K': {
                        StorageAppGuard storage;
                        if (!storage) {
                            report_storage_not_owned();
                            break;
                        }
                        usb_clear_files_recursive("/usb");
                        dual_printf("[System] Cleared files in storage partition.\n");
                        break;
                    }
                    case 'Q':
                    case 'I': {
                        dual_printf("ERROR: Inference is not supported in this firmware.\n");
                        break;
                    }
                }
            }
        }
    }
}


static void input_controls_monitor_task(void *pvParameters) {
    (void)pvParameters;
    InputControlsSnapshot previous = input_controls_get_snapshot();
    dual_printf("INPUT_STATUS,encoder=%ld,encoder_button=%lu,button2=%lu,button2_level=%d,enc_level=%d,clk=%d,dt=%d\n",
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
extern "C" void app_main() {
    ESP_LOGI(TAG, "Starting camera_usb_demo firmware (UVC + CDC + MSC)...");

    usb_composite_init();
    camera_mutex = xSemaphoreCreateMutex();
    configASSERT(camera_mutex);

    // The descriptor advertises UXGA, so reserve a PSRAM staging buffer large
    // enough for its worst-case JPEG frame before a host starts streaming.
    g_uvc_buf_cap = UVC_MAX_FRAME_BYTES;
    g_uvc_buf = (uint8_t *)heap_caps_malloc(g_uvc_buf_cap, MALLOC_CAP_SPIRAM);
    if (!g_uvc_buf) {
        g_uvc_buf = (uint8_t *)heap_caps_malloc(g_uvc_buf_cap, MALLOC_CAP_DEFAULT);
    }
    if (!g_uvc_buf) {
        ESP_LOGE(TAG, "Failed to allocate UVC JPEG buffer (%u bytes).", (unsigned)g_uvc_buf_cap);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t err = photo_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Photo storage init failed: %s", esp_err_to_name(err));
    }

    err = camera_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
    } else {
        // Force the UVC-advertised geometry (JPEG + QVGA) at boot, regardless
        // of camera_capture_ov2640.cpp's own internal default (JPEG + VGA).
        err = camera_capture_reinit((int)g_current_format, (int)g_current_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Initial camera_capture_reinit to default UVC mode failed: %s", esp_err_to_name(err));
        }
    }

    if (ENABLE_INPUT_CONTROLS) {
        esp_err_t input_err = input_controls_init();
        if (input_err != ESP_OK) {
            ESP_LOGW(TAG, "Input controls init failed: %s", esp_err_to_name(input_err));
        } else {
            xTaskCreatePinnedToCore(input_controls_monitor_task, "input_controls_monitor", 4096, NULL, 3, NULL, 0);
        }
    }

    xTaskCreatePinnedToCore(uvc_stream_task, "uvc_stream_task", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(usb_cdc_command_task, "usb_cdc_command_task", 8192, NULL, 5, NULL, 0);
}
