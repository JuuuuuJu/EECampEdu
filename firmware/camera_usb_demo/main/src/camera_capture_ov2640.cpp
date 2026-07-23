#include "camera_capture.hpp"

#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CAMERA_CAPTURE";

// GPIO wiring copied from the provided OV2640 Arduino reference.
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

static constexpr int kXclkFrequencyHz = 20000000;
static constexpr int kJpegQuality = 12;

int g_hmirror = 0;
int g_vflip = 0;

static bool g_camera_initialized = false;
static pixformat_t g_current_format = PIXFORMAT_JPEG;
static framesize_t g_current_size = FRAMESIZE_VGA;

static CameraFrameFormat map_frame_format(pixformat_t format) {
    switch (format) {
        case PIXFORMAT_GRAYSCALE:
            return CameraFrameFormat::kGrayscale;
        case PIXFORMAT_RGB565:
            return CameraFrameFormat::kRgb565;
        case PIXFORMAT_YUV422:
            return CameraFrameFormat::kYuv422;
        case PIXFORMAT_JPEG:
            return CameraFrameFormat::kJpeg;
        default:
            return CameraFrameFormat::kGrayscale;
    }
}

static const char *format_name(pixformat_t format) {
    switch (format) {
        case PIXFORMAT_GRAYSCALE:
            return "GRAYSCALE";
        case PIXFORMAT_RGB565:
            return "RGB565";
        case PIXFORMAT_YUV422:
            return "YUV422";
        case PIXFORMAT_JPEG:
            return "JPEG";
        default:
            return "UNKNOWN";
    }
}

esp_err_t camera_capture_init() {
    if (g_camera_initialized) {
        return ESP_OK;
    }

    camera_config_t config = {};
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;

    config.xclk_freq_hz = kXclkFrequencyHz;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = g_current_format;
    config.frame_size = g_current_size;
    config.jpeg_quality = kJpegQuality;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;

    ESP_LOGI(TAG,
             "Initializing OV2640: format=%s frame_size=%d xclk=%d Hz fb_count=%d location=PSRAM",
             format_name(g_current_format),
             (int)g_current_size,
             kXclkFrequencyHz,
             config.fb_count
    );

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_init failed with PSRAM fb: %s", esp_err_to_name(err));
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(150));

        if (g_current_size <= FRAMESIZE_QVGA) {
            config.fb_location = CAMERA_FB_IN_DRAM;
            config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
            ESP_LOGW(TAG, "Retrying OV2640 init with DRAM fb for small frame_size=%d", (int)g_current_size);
            err = esp_camera_init(&config);
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_camera_init failed after fallback: %s", esp_err_to_name(err));
            esp_camera_deinit();
            return err;
        }
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != nullptr) {
        sensor->set_brightness(sensor, 0);
        sensor->set_contrast(sensor, 0);
        sensor->set_saturation(sensor, 0);
        sensor->set_special_effect(sensor, 0);
        sensor->set_whitebal(sensor, 1);
        sensor->set_awb_gain(sensor, 1);
        sensor->set_exposure_ctrl(sensor, 1);
        sensor->set_gain_ctrl(sensor, 1);
        sensor->set_hmirror(sensor, g_hmirror);
        sensor->set_vflip(sensor, g_vflip);
    }

    g_camera_initialized = true;
    ESP_LOGI(TAG, "OV2640 camera initialized.");
    return ESP_OK;
}

esp_err_t camera_capture_reinit(int format_val, int framesize_val) {
    pixformat_t format = (pixformat_t)format_val;
    framesize_t size = (framesize_t)framesize_val;
    const pixformat_t previous_format = g_current_format;
    const framesize_t previous_size = g_current_size;

    // if (size >= FRAMESIZE_VGA && format != PIXFORMAT_JPEG) {
    //     ESP_LOGE(TAG, "ERROR: Raw formats at VGA and higher resolutions are disabled to prevent DRAM overflow crashes.");
    //     return ESP_ERR_INVALID_ARG;
    // }

    if (g_camera_initialized && g_current_format == format && g_current_size == size) {
        return ESP_OK;
    }

    if (g_camera_initialized) {
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor != nullptr) {
            bool runtime_ok = true;
            if (g_current_format != format && sensor->set_pixformat(sensor, format) != 0) {
                runtime_ok = false;
            }
            if (runtime_ok && g_current_size != size && sensor->set_framesize(sensor, size) != 0) {
                runtime_ok = false;
            }
            if (runtime_ok) {
                g_current_format = format;
                g_current_size = size;
                vTaskDelay(pdMS_TO_TICKS(120));
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Runtime camera reconfigure failed; retrying full init.");
        }

        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(300));
        g_camera_initialized = false;
    }

    g_current_format = format;
    g_current_size = size;

    esp_err_t err = camera_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera reinit failed for format=%s size=%d: %s", format_name(format), (int)size, esp_err_to_name(err));
        esp_camera_deinit();
        g_camera_initialized = false;
        g_current_format = previous_format;
        g_current_size = previous_size;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return err;
}
esp_err_t camera_capture_frame(CameraFrame *frame) {
    if (frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(frame, 0, sizeof(*frame));

    if (!g_camera_initialized) {
        esp_err_t err = camera_capture_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGE(TAG, "esp_camera_fb_get returned null.");
        return ESP_FAIL;
    }

    frame->data = fb->buf;
    frame->size = fb->len;
    frame->width = fb->width;
    frame->height = fb->height;
    frame->format = map_frame_format(fb->format);
    frame->handle = fb;

    return ESP_OK;
}

void camera_capture_release(CameraFrame *frame) {
    if (frame == nullptr || frame->handle == nullptr) {
        return;
    }

    camera_fb_t *fb = static_cast<camera_fb_t *>(frame->handle);
    esp_camera_fb_return(fb);
    memset(frame, 0, sizeof(*frame));
}


