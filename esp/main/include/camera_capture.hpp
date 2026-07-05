#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum class CameraFrameFormat {
    kGrayscale,
    kRgb565,
    kYuv422,
    kJpeg,
};

struct CameraFrame {
    const uint8_t *data;
    size_t size;
    int width;
    int height;
    CameraFrameFormat format;
};

esp_err_t camera_capture_init();
esp_err_t camera_capture_frame(CameraFrame *frame);
void camera_capture_release(CameraFrame *frame);
