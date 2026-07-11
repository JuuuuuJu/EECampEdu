#include "camera_capture.hpp"

#include "esp_log.h"

static const char *TAG = "CAMERA_CAPTURE";

esp_err_t camera_capture_init() {
    ESP_LOGW(TAG, "Camera capture is not enabled in this build. Use TEST UART mode.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_capture_frame(CameraFrame *frame) {
    if (frame != nullptr) {
        frame->data = nullptr;
        frame->size = 0;
        frame->width = 0;
        frame->height = 0;
        frame->format = CameraFrameFormat::kGrayscale;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void camera_capture_release(CameraFrame *frame) {
    (void)frame;
}
