#pragma once

#include <stddef.h>
#include <stdint.h>

#include "camera_capture.hpp"
#include "esp_err.h"

struct StoredPhotoMetadata {
    uint32_t bytes;
    uint16_t width;
    uint16_t height;
    uint8_t format;
};

esp_err_t photo_storage_init();
esp_err_t photo_storage_write_latest(const CameraFrame &frame);
esp_err_t photo_storage_write_capture(const CameraFrame &frame, const char *prefix);
esp_err_t photo_storage_read_latest(uint8_t *buffer, size_t buffer_size, StoredPhotoMetadata *metadata);
const char *photo_storage_last_error();
