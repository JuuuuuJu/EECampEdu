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
#include "esp_camera.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "CAMERA_USB_DEMO";

static pixformat_t g_current_format = PIXFORMAT_JPEG;
static framesize_t g_current_size = FRAMESIZE_VGA;

extern int g_hmirror;
extern int g_vflip;

static bool streaming_mode = false;
static SemaphoreHandle_t camera_mutex = NULL;

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
    int py_fmt = 4; // Default to JPEG
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
            vTaskDelay(pdMS_TO_TICKS(15));
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
                memcpy(cmd_buf, msg.buf, msg.buf_len);
                cmd_buf[msg.buf_len] = '\0';
                
                while (msg.buf_len > 0 && (cmd_buf[msg.buf_len - 1] == '\n' || cmd_buf[msg.buf_len - 1] == '\r')) {
                    cmd_buf[msg.buf_len - 1] = '\0';
                    msg.buf_len--;
                }
                
                if (strlen(cmd_buf) == 0) continue;
                
                if (strcasecmp(cmd_buf, "format") == 0) {
                    bool was_streaming = streaming_mode;
                    if (was_streaming) {
                        if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                            streaming_mode = false;
                            xSemaphoreGive(camera_mutex);
                        }
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    dual_printf("[System] Formatting internal flash drive partition...\n");
                    usb_msc_mount_to_app();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl(USB_MSC_MOUNT_PATH, "storage");
                    if (err == ESP_OK) {
                        dual_printf("[System] Format successful! Rebooting board...\n");
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    } else {
                        dual_printf("ERROR: Format failed: %s\n", esp_err_to_name(err));
                    }
                    usb_msc_mount_to_pc();
                    if (was_streaming) streaming_mode = true;
                    continue;
                }
                
                if (strcasecmp(cmd_buf, "usb") == 0) {
                    bool was_streaming = streaming_mode;
                    if (was_streaming) {
                        if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                            streaming_mode = false;
                            xSemaphoreGive(camera_mutex);
                        }
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    dual_printf("[System] Exposing FAT partition as USB Mass Storage drive...\n");
                    usb_msc_mount_to_pc();
                    if (was_streaming) streaming_mode = true;
                    continue;
                }
                
                char act = cmd_buf[0];
                char act_upper = toupper((unsigned char)act);
                const char* argStr = cmd_buf + 1;
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
                            
                            vTaskDelay(pdMS_TO_TICKS(100));
                            esp_err_t err = camera_capture_reinit(fmt_val, (int)g_current_size);
                            if (err == ESP_OK) {
                                g_current_format = (pixformat_t)fmt_val;
                                dual_printf("[System] Pixel Format updated successfully.\n");
                            } else {
                                dual_printf("ERROR: Reinit format failed: %s\n", esp_err_to_name(err));
                            }
                            break;
                        }
                        case 'S': {
                            int size_val = FRAMESIZE_96X96;
                            if (val == 1) size_val = FRAMESIZE_QQVGA;
                            else if (val == 2) size_val = FRAMESIZE_QVGA;
                            else if (val == 3) size_val = FRAMESIZE_VGA;
                            else if (val == 4) size_val = FRAMESIZE_SVGA;
                            else if (val == 5) size_val = FRAMESIZE_UXGA;
                            
                            vTaskDelay(pdMS_TO_TICKS(100));
                            esp_err_t err = camera_capture_reinit((int)g_current_format, size_val);
                            if (err == ESP_OK) {
                                g_current_size = (framesize_t)size_val;
                                dual_printf("[System] Resolution updated successfully.\n");
                            } else {
                                dual_printf("ERROR: Reinit resolution failed: %s\n", esp_err_to_name(err));
                            }
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
                
                // Commands parsed outside lock (or requiring different mounts)
                switch (act_upper) {
                    case 'D': {
                        streaming_mode = (val == 1);
                        dual_printf("[System] Streaming: %s\n", streaming_mode ? "ENABLED" : "DISABLED");
                        break;
                    }
                    case 'C': // Map C/c to behaves like w/W
                    case 'W': {
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                        dual_printf("[System] Preparing flash drive...\n");
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        
                        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                            camera_fb_t *dummy = esp_camera_fb_get();
                            if (dummy) esp_camera_fb_return(dummy);
                            
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
                                    memcpy(temp_buf, fb->buf, data_len);
                                    
                                    const char *ext = "bin";
                                    if (fb->format == PIXFORMAT_JPEG) ext = "jpg";
                                    else if (fb->format == PIXFORMAT_GRAYSCALE) ext = "gray";
                                    else if (fb->format == PIXFORMAT_RGB565) ext = "rgb565";                                    const char* ts = (strlen(argStr) > 0) ? argStr : "manual";
                                    const char *capture_dir = "/usb/camera_usb";
                                    if (mkdir(capture_dir, 0775) != 0 && errno != EEXIST) {
                                        dual_printf("ERROR: Failed to create %s: errno=%d (%s)\n", capture_dir, errno, strerror(errno));
                                    }
                                    char filepath[1024];
                                    snprintf(filepath, sizeof(filepath), "%s/img_%s_fmt%d_w%d_h%d.%s",
                                                capture_dir, ts, (int)fb->format, fb->width, fb->height, ext);
                                    
                                    esp_camera_fb_return(fb);
                                    xSemaphoreGive(camera_mutex);
                                    
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
                        
                        vTaskDelay(pdMS_TO_TICKS(100));
                        usb_msc_mount_to_pc();
                        if (was_streaming) streaming_mode = true;
                        break;
                    }
                    case 'L': {
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                        
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        
                        usb_list_files();
                        
                        // CRITICAL FIX: Give serial TX time to send files list
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        
                        if (was_streaming) {
                            streaming_mode = true;
                        }
                        usb_msc_mount_to_pc();
                        break;
                    }
                    case 'R': {
                        bool was_streaming = streaming_mode;
                        if (was_streaming) {
                            if (xSemaphoreTake(camera_mutex, portMAX_DELAY) == pdTRUE) {
                                streaming_mode = false;
                                xSemaphoreGive(camera_mutex);
                            }
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                        
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        
                        char input_buf[256];
                        strncpy(input_buf, argStr, sizeof(input_buf));
                        input_buf[sizeof(input_buf)-1] = '\0';
                        int len = strlen(input_buf);
                        while (len > 0 && (input_buf[len-1] <= ' ')) { input_buf[--len] = '\0'; }
                        
                        if (len == 0) {
                            dual_printf("ERROR: Usage: r <filename> OR r <index>\n");
                            usb_msc_mount_to_pc();
                            if (was_streaming) streaming_mode = true;
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
                                usb_msc_mount_to_pc();
                                if (was_streaming) streaming_mode = true;
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
                    case 'K': {
                        usb_msc_mount_to_app();
                        vTaskDelay(pdMS_TO_TICKS(50));
                        DIR *dir = opendir("/usb");
                        if (dir) {
                            struct dirent *entry;
                            while ((entry = readdir(dir)) != NULL) {
                                if (entry->d_name[0] != '.') {
                                    char filepath[512];
                                    snprintf(filepath, sizeof(filepath), "/usb/%s", entry->d_name);
                                    unlink(filepath);
                                }
                            }
                            closedir(dir);
                        }
                        dual_printf("[System] Cleared files in storage partition.\n");
                        usb_msc_mount_to_pc();
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
    ESP_LOGI(TAG, "Starting camera_usb_demo firmware...");
    
    usb_composite_init();
    camera_mutex = xSemaphoreCreateMutex();
    configASSERT(camera_mutex);
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    esp_err_t err = photo_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Photo storage init failed: %s", esp_err_to_name(err));
    }
    
    err = camera_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
    }
    
    if (ENABLE_INPUT_CONTROLS) {
        esp_err_t input_err = input_controls_init();
        if (input_err != ESP_OK) {
            ESP_LOGW(TAG, "Input controls init failed: %s", esp_err_to_name(input_err));
        } else {
            xTaskCreatePinnedToCore(input_controls_monitor_task, "input_controls_monitor", 4096, NULL, 3, NULL, 0);
        }
    }

    xTaskCreatePinnedToCore(camera_stream_task, "camera_stream_task", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(usb_cdc_command_task, "usb_cdc_command_task", 8192, NULL, 5, NULL, 0);
}
