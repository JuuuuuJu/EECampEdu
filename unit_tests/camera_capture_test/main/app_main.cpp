#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "CAMERA_UNIT_TEST";

// OV2640 wiring from the current PCB/layout contract.
static constexpr int PWDN_GPIO_NUM = -1;
static constexpr int RESET_GPIO_NUM = -1;
static constexpr int XCLK_GPIO_NUM = 15;
static constexpr int SIOD_GPIO_NUM = 4;
static constexpr int SIOC_GPIO_NUM = 5;
static constexpr int Y9_GPIO_NUM = 16;
static constexpr int Y8_GPIO_NUM = 17;
static constexpr int Y7_GPIO_NUM = 18;
static constexpr int Y6_GPIO_NUM = 12;
static constexpr int Y5_GPIO_NUM = 10;
static constexpr int Y4_GPIO_NUM = 8;
static constexpr int Y3_GPIO_NUM = 9;
static constexpr int Y2_GPIO_NUM = 11;
static constexpr int VSYNC_GPIO_NUM = 6;
static constexpr int HREF_GPIO_NUM = 7;
static constexpr int PCLK_GPIO_NUM = 13;

static constexpr int CONTROLLER_BAUD_RATE = 115200;
static constexpr int JPEG_QUALITY = 12;
static constexpr int STREAM_PERIOD_MS = 500;

static bool g_initialized = false;
static volatile bool g_streaming = false;
static pixformat_t g_format = PIXFORMAT_JPEG;
static framesize_t g_size = FRAMESIZE_QQVGA;
static SemaphoreHandle_t g_camera_mutex = nullptr;

static const char kB64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char *format_name(pixformat_t format) {
    switch (format) {
        case PIXFORMAT_GRAYSCALE: return "GRAYSCALE";
        case PIXFORMAT_RGB565: return "RGB565";
        case PIXFORMAT_YUV422: return "YUV422";
        case PIXFORMAT_JPEG: return "JPEG";
        default: return "UNKNOWN";
    }
}

static int controller_format_id(pixformat_t format) {
    // camera_controller.py expects: 0=RGB565, 1=YUV422, 3=GRAYSCALE, 4=JPEG.
    switch (format) {
        case PIXFORMAT_RGB565: return 0;
        case PIXFORMAT_YUV422: return 1;
        case PIXFORMAT_GRAYSCALE: return 3;
        case PIXFORMAT_JPEG: return 4;
        default: return 4;
    }
}

static pixformat_t map_format_command(int value) {
    switch (value) {
        case 0: return PIXFORMAT_RGB565;
        case 1: return PIXFORMAT_YUV422;
        case 2: return PIXFORMAT_GRAYSCALE;
        case 3: return PIXFORMAT_JPEG;
        default: return PIXFORMAT_JPEG;
    }
}

static framesize_t map_size_command(int value) {
    switch (value) {
        case 0: return FRAMESIZE_96X96;
        case 1: return FRAMESIZE_QQVGA;
        case 2: return FRAMESIZE_QVGA;
        case 3: return FRAMESIZE_VGA;
        case 4: return FRAMESIZE_SVGA;
        case 5: return FRAMESIZE_UXGA;
        default: return FRAMESIZE_VGA;
    }
}

static void write_base64(const uint8_t *data, size_t len) {
    size_t i = 0;
    int emitted = 0;
    while (i + 3 <= len) {
        const uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        putchar(kB64Table[(v >> 18) & 0x3f]);
        putchar(kB64Table[(v >> 12) & 0x3f]);
        putchar(kB64Table[(v >> 6) & 0x3f]);
        putchar(kB64Table[v & 0x3f]);
        i += 3;
        emitted += 4;
        if (emitted >= 240) {
            putchar('\n');
            fflush(stdout);
            emitted = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (i < len) {
        const uint8_t a = data[i++];
        const uint8_t b = (i < len) ? data[i++] : 0;
        const uint32_t v = ((uint32_t)a << 16) | ((uint32_t)b << 8);
        putchar(kB64Table[(v >> 18) & 0x3f]);
        putchar(kB64Table[(v >> 12) & 0x3f]);
        if (i <= len && (len % 3) == 2) {
            putchar(kB64Table[(v >> 6) & 0x3f]);
            putchar('=');
        } else {
            putchar('=');
            putchar('=');
        }
    }
    putchar('\n');
    fflush(stdout);
}

static void send_frame_to_controller(camera_fb_t *fb) {
    printf("---START_IMAGE:%d:%d:%d:%u---\n",
           controller_format_id(fb->format),
           fb->width,
           fb->height,
           (unsigned)fb->len);
    write_base64(fb->buf, fb->len);
    printf("---END_IMAGE---\n");
    fflush(stdout);
}

static void dump_frame(camera_fb_t *fb) {
    uint32_t sum = 0;
    const size_t n = fb->len < 64 ? fb->len : 64;
    for (size_t i = 0; i < n; ++i) {
        sum += fb->buf[i];
    }
    ESP_LOGI(TAG,
             "CAMERA_FRAME,width=%d,height=%d,format=%s,bytes=%u,sum64=%lu",
             fb->width,
             fb->height,
             format_name(fb->format),
             (unsigned)fb->len,
             (unsigned long)sum);
}

static esp_err_t camera_init_current(void) {
    if (g_initialized) {
        return ESP_OK;
    }

    camera_config_t cfg = {};
    cfg.pin_pwdn = PWDN_GPIO_NUM;
    cfg.pin_reset = RESET_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM;
    cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d0 = Y2_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM;
    cfg.pin_href = HREF_GPIO_NUM;
    cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.xclk_freq_hz = 10000000;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = g_format;
    cfg.frame_size = g_size;
    cfg.jpeg_quality = JPEG_QUALITY;
    cfg.fb_count = 1;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;

    ESP_LOGI(TAG, "init OV2640 format=%s size=%d", format_name(g_format), (int)g_size);
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != nullptr) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
    }
    g_initialized = true;
    return ESP_OK;
}

static esp_err_t camera_reinit_locked(pixformat_t format, framesize_t size) {
    if (g_initialized) {
        esp_camera_deinit();
        g_initialized = false;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    g_format = format;
    g_size = size;
    return camera_init_current();
}

static void capture_and_send_once(void) {
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "camera busy; skipping frame");
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        xSemaphoreGive(g_camera_mutex);
        return;
    }
    dump_frame(fb);
    send_frame_to_controller(fb);
    esp_camera_fb_return(fb);
    xSemaphoreGive(g_camera_mutex);
}

static void normalize_command(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    for (size_t i = 0; line[i] != '\0'; ++i) {
        line[i] = (char)tolower((unsigned char)line[i]);
    }
}

static void handle_command(char *line) {
    normalize_command(line);
    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, "d1") == 0) {
        g_streaming = true;
        printf("CAMERA_TEST,stream=on\n");
        fflush(stdout);
        return;
    }
    if (strcmp(line, "d0") == 0) {
        g_streaming = false;
        printf("CAMERA_TEST,stream=off\n");
        fflush(stdout);
        return;
    }
    if (line[0] == 'c') {
        capture_and_send_once();
        return;
    }
    if (line[0] == 'f' && line[1] != '\0') {
        const pixformat_t next_format = map_format_command(atoi(line + 1));
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_ERROR_CHECK(camera_reinit_locked(next_format, g_size));
            xSemaphoreGive(g_camera_mutex);
            printf("CAMERA_TEST,format=%s\n", format_name(g_format));
            fflush(stdout);
        }
        return;
    }
    if (line[0] == 's' && line[1] != '\0') {
        const framesize_t next_size = map_size_command(atoi(line + 1));
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_ERROR_CHECK(camera_reinit_locked(g_format, next_size));
            xSemaphoreGive(g_camera_mutex);
            printf("CAMERA_TEST,size=%d\n", (int)g_size);
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "j") == 0) {
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_ERROR_CHECK(camera_reinit_locked(PIXFORMAT_JPEG, FRAMESIZE_VGA));
            xSemaphoreGive(g_camera_mutex);
        }
        return;
    }
    if (strcmp(line, "g") == 0) {
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_ERROR_CHECK(camera_reinit_locked(PIXFORMAT_GRAYSCALE, FRAMESIZE_96X96));
            xSemaphoreGive(g_camera_mutex);
        }
        return;
    }

    printf("CAMERA_TEST,ignored_command=%s\n", line);
    fflush(stdout);
}

static void command_task(void *) {
    char line[96];
    while (true) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_command(line);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void stream_task(void *) {
    while (true) {
        if (g_streaming) {
            capture_and_send_once();
            vTaskDelay(pdMS_TO_TICKS(STREAM_PERIOD_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

extern "C" void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    uart_set_baudrate(UART_NUM_0, CONTROLLER_BAUD_RATE);

    g_camera_mutex = xSemaphoreCreateMutex();
    configASSERT(g_camera_mutex != nullptr);
    ESP_ERROR_CHECK(camera_init_current());

    printf("READY,CAMERA_CAPTURE_TEST\n");
    printf("Controller commands: d1=start preview, d0=stop preview, c=single frame, f3=JPEG, s1=QQVGA default, s3=VGA\n");
    fflush(stdout);

    xTaskCreatePinnedToCore(command_task, "camera_command_task", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(stream_task, "camera_stream_task", 8192, nullptr, 4, nullptr, 1);
}



