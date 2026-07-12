#include "photo_storage.hpp"
#include "esp_camera.h"
#include "esp_log.h"
#include "img_converters.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static const char *TAG = "PHOTO_STORAGE";

static constexpr const char *FILE_PATH_RAW = "/usb/latest.raw";
static constexpr const char *FILE_PATH_META = "/usb/latest.meta";
static constexpr const char *FILE_PATH_BMP = "/usb/latest.bmp";

esp_err_t photo_storage_init() {
    // Initialization is handled by usb_composite_init() in app_main.cpp
    // which mounts the FATFS partition to /usb.
    return ESP_OK;
}

esp_err_t photo_storage_write_latest(const CameraFrame &frame) {
    if (frame.data == nullptr || frame.size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Decouple memory
    uint8_t *temp_buf = (uint8_t *)malloc(frame.size);
    if (temp_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer for flash write.");
        return ESP_FAIL;
    }
    memcpy(temp_buf, frame.data, frame.size);

    // 1. Write .raw
    FILE *f_raw = fopen(FILE_PATH_RAW, "wb");
    if (f_raw == nullptr) {
        free(temp_buf);
        return ESP_FAIL;
    }
    size_t written = fwrite(temp_buf, 1, frame.size, f_raw);
    fclose(f_raw);
    free(temp_buf);
    
    if (written != frame.size) return ESP_FAIL;

    // 2. Write .meta
    StoredPhotoMetadata metadata = {};
    metadata.bytes = (uint32_t)frame.size;
    metadata.width = (uint16_t)frame.width;
    metadata.height = (uint16_t)frame.height;
    
    pixformat_t pfmt = PIXFORMAT_GRAYSCALE; // Default for BMP conversion
    switch (frame.format) {
        case CameraFrameFormat::kGrayscale: metadata.format = 0; pfmt = PIXFORMAT_GRAYSCALE; break;
        case CameraFrameFormat::kRgb565:    metadata.format = 1; pfmt = PIXFORMAT_RGB565; break;
        case CameraFrameFormat::kYuv422:    metadata.format = 2; pfmt = PIXFORMAT_YUV422; break;
        case CameraFrameFormat::kJpeg:      metadata.format = 3; break;
    }

    FILE *f_meta = fopen(FILE_PATH_META, "wb");
    if (f_meta) {
        fwrite(&metadata, 1, sizeof(metadata), f_meta);
        fclose(f_meta);
    }

    // 3. Write .bmp (using fmt2bmp instead of frame2bmp so it works without hardware handles!)
    if (frame.format != CameraFrameFormat::kJpeg) {
        uint8_t *bmp_buf = nullptr;
        size_t bmp_len = 0;
        
        if (fmt2bmp((uint8_t *)frame.data, frame.size, frame.width, frame.height, pfmt, &bmp_buf, &bmp_len)) {
            FILE *f_bmp = fopen(FILE_PATH_BMP, "wb");
            if (f_bmp) {
                fwrite(bmp_buf, 1, bmp_len, f_bmp);
                fclose(f_bmp);
            }
            free(bmp_buf);
        }
    } else {
        unlink(FILE_PATH_BMP);
    }

    ESP_LOGI(TAG, "Stored latest photo: %d bytes, %dx%d, format=%d.", 
             (int)frame.size, frame.width, frame.height, (int)metadata.format);
    return ESP_OK;
}

esp_err_t photo_storage_read_latest(uint8_t *out_buffer, size_t max_bytes, StoredPhotoMetadata *out_metadata) {
    if (out_buffer == nullptr || out_metadata == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f_meta = fopen(FILE_PATH_META, "rb");
    if (!f_meta) return ESP_FAIL;
    
    fread(out_metadata, 1, sizeof(*out_metadata), f_meta);
    fclose(f_meta);

    if (out_metadata->bytes > max_bytes) {
        ESP_LOGE(TAG, "Stored photo is %lu bytes, but max is %lu.",
                 (unsigned long)out_metadata->bytes, (unsigned long)max_bytes);
        return ESP_ERR_NO_MEM;
    }

    FILE *f_raw = fopen(FILE_PATH_RAW, "rb");
    if (!f_raw) return ESP_FAIL;
    
    fread(out_buffer, 1, out_metadata->bytes, f_raw);
    fclose(f_raw);

    ESP_LOGI(TAG, "Read latest photo: %lu bytes, %ux%u, format=%u.",
             (unsigned long)out_metadata->bytes,
             (unsigned)out_metadata->width,
             (unsigned)out_metadata->height,
             (unsigned)out_metadata->format);
    return ESP_OK;
}
