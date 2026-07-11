#include "photo_storage.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "img_converters.h"
#include "esp_camera.h"

static const char *TAG = "PHOTO_STORAGE";

#define FILE_PATH_RAW  "/usb/latest.raw"
#define FILE_PATH_META "/usb/latest.meta"
#define FILE_PATH_BMP  "/usb/latest.bmp"

esp_err_t photo_storage_init() {
    // Simply check if /usb directory can be resolved (VFS mount check)
    struct stat st;
    if (stat("/usb", &st) != 0) {
        ESP_LOGE(TAG, "FATFS mount point '/usb' not available.");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t photo_storage_write_latest(const CameraFrame &frame) {
    if (frame.data == nullptr || frame.size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Write the raw image bytes
    FILE *f_raw = fopen(FILE_PATH_RAW, "wb");
    if (f_raw == nullptr) {
        ESP_LOGE(TAG, "Failed to open '%s' for writing.", FILE_PATH_RAW);
        return ESP_FAIL;
    }
    size_t written = fwrite(frame.data, 1, frame.size, f_raw);
    fclose(f_raw);
    if (written != frame.size) {
        ESP_LOGE(TAG, "Incomplete write to '%s'.", FILE_PATH_RAW);
        return ESP_FAIL;
    }

    // 2. Write the StoredPhotoMetadata structure
    StoredPhotoMetadata metadata = {};
    metadata.bytes = (uint32_t)frame.size;
    metadata.width = (uint16_t)frame.width;
    metadata.height = (uint16_t)frame.height;
    switch (frame.format) {
        case CameraFrameFormat::kGrayscale: metadata.format = 0; break;
        case CameraFrameFormat::kRgb565:    metadata.format = 1; break;
        case CameraFrameFormat::kYuv422:    metadata.format = 2; break;
        case CameraFrameFormat::kJpeg:      metadata.format = 3; break;
    }

    FILE *f_meta = fopen(FILE_PATH_META, "wb");
    if (f_meta == nullptr) {
        ESP_LOGE(TAG, "Failed to open '%s' for writing.", FILE_PATH_META);
        return ESP_FAIL;
    }
    size_t meta_written = fwrite(&metadata, 1, sizeof(metadata), f_meta);
    fclose(f_meta);
    if (meta_written != sizeof(metadata)) {
        ESP_LOGE(TAG, "Incomplete write to '%s'.", FILE_PATH_META);
        return ESP_FAIL;
    }

    // 3. Optional: Write a BMP copy if format is raw and we have a valid camera handle
    if (frame.format != CameraFrameFormat::kJpeg && frame.handle != nullptr) {
        camera_fb_t *fb = (camera_fb_t *)frame.handle;
        uint8_t *bmp_buf = nullptr;
        size_t bmp_len = 0;
        if (frame2bmp(fb, &bmp_buf, &bmp_len)) {
            FILE *f_bmp = fopen(FILE_PATH_BMP, "wb");
            if (f_bmp) {
                fwrite(bmp_buf, 1, bmp_len, f_bmp);
                fclose(f_bmp);
            }
            free(bmp_buf);
        }
    } else {
        // If JPEG or no handle, remove any stale BMP copy
        unlink(FILE_PATH_BMP);
    }

    ESP_LOGI(TAG, "Stored latest photo: %d bytes, %dx%d, format=%d.", 
             (int)frame.size, frame.width, frame.height, (int)metadata.format);
    return ESP_OK;
}

esp_err_t photo_storage_read_latest(uint8_t *buffer, size_t buffer_size, StoredPhotoMetadata *metadata) {
    if (buffer == nullptr || metadata == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Read metadata
    FILE *f_meta = fopen(FILE_PATH_META, "rb");
    if (f_meta == nullptr) {
        ESP_LOGE(TAG, "Failed to open '%s' for reading.", FILE_PATH_META);
        return ESP_ERR_NOT_FOUND;
    }
    size_t meta_read = fread(metadata, 1, sizeof(StoredPhotoMetadata), f_meta);
    fclose(f_meta);
    if (meta_read != sizeof(StoredPhotoMetadata)) {
        ESP_LOGE(TAG, "Incomplete read of '%s'.", FILE_PATH_META);
        return ESP_FAIL;
    }

    // 2. Check buffer bounds
    if (metadata->bytes > buffer_size) {
        ESP_LOGE(TAG, "Read buffer too small: need %u, have %u.", (unsigned)metadata->bytes, (unsigned)buffer_size);
        return ESP_ERR_NO_MEM;
    }

    // 3. Read raw image payload
    FILE *f_raw = fopen(FILE_PATH_RAW, "rb");
    if (f_raw == nullptr) {
        ESP_LOGE(TAG, "Failed to open '%s' for reading.", FILE_PATH_RAW);
        return ESP_ERR_NOT_FOUND;
    }
    size_t read_bytes = fread(buffer, 1, metadata->bytes, f_raw);
    fclose(f_raw);
    if (read_bytes != metadata->bytes) {
        ESP_LOGE(TAG, "Incomplete read of '%s'.", FILE_PATH_RAW);
        return ESP_FAIL;
    }

    return ESP_OK;
}
