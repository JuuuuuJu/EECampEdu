#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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

static bool g_initialized = false;
static pixformat_t g_format = PIXFORMAT_JPEG;
static framesize_t g_size = FRAMESIZE_VGA;

static const char *format_name(pixformat_t format) {
    switch (format) {
        case PIXFORMAT_GRAYSCALE: return "GRAYSCALE";
        case PIXFORMAT_RGB565: return "RGB565";
        case PIXFORMAT_YUV422: return "YUV422";
        case PIXFORMAT_JPEG: return "JPEG";
        default: return "UNKNOWN";
    }
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
    cfg.jpeg_quality = 12;
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

static esp_err_t camera_reinit(pixformat_t format, framesize_t size) {
    if (g_initialized) {
        esp_camera_deinit();
        g_initialized = false;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    g_format = format;
    g_size = size;
    return camera_init_current();
}

static void dump_frame(camera_fb_t *fb) {
    uint32_t sum = 0;
    const size_t n = fb->len < 64 ? fb->len : 64;
    for (size_t i = 0; i < n; ++i) {
        sum += fb->buf[i];
    }
    printf("CAMERA_FRAME,width=%d,height=%d,format=%s,bytes=%u,first10=[",
           fb->width, fb->height, format_name(fb->format), (unsigned)fb->len);
    for (int i = 0; i < 10 && i < (int)fb->len; ++i) {
        printf("%s%u", i ? " " : "", fb->buf[i]);
    }
    printf("],sum64=%lu\n", (unsigned long)sum);
    fflush(stdout);
}

static void command_task(void *) {
    char line[16];
    while (true) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            if (line[0] == 'j') {
                ESP_ERROR_CHECK(camera_reinit(PIXFORMAT_JPEG, FRAMESIZE_VGA));
            } else if (line[0] == 'g') {
                ESP_ERROR_CHECK(camera_reinit(PIXFORMAT_GRAYSCALE, FRAMESIZE_96X96));
            } else if (line[0] == 'q') {
                ESP_ERROR_CHECK(camera_reinit(PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    ESP_ERROR_CHECK(camera_init_current());
    printf("READY,CAMERA_CAPTURE_TEST\n");
    printf("Commands: j=VGA JPEG, g=96x96 grayscale, q=QVGA grayscale\n");
    xTaskCreatePinnedToCore(command_task, "camera_command_task", 4096, nullptr, 4, nullptr, 0);

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == nullptr) {
            ESP_LOGE(TAG, "esp_camera_fb_get failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        dump_frame(fb);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
