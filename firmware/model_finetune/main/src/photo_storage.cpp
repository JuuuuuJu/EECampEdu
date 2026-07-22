#include "photo_storage.hpp"
#include "esp_camera.h"
#include "esp_log.h"
#include "img_converters.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "ff.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static const char *TAG = "PHOTO_STORAGE";

static constexpr const char *FILE_PATH_RAW = "/usb/latest.raw";
static constexpr const char *FILE_PATH_META = "/usb/latest.meta";
static constexpr const char *FILE_PATH_BMP = "/usb/latest.bmp";
static const char *extension_for_format(CameraFrameFormat format) {
    switch (format) {
        case CameraFrameFormat::kJpeg: return "jpg";
        case CameraFrameFormat::kGrayscale: return "gray";
        case CameraFrameFormat::kRgb565: return "rgb565";
        case CameraFrameFormat::kYuv422: return "yuv422";
    }
    return "bin";
}

static pixformat_t pixformat_for_frame(CameraFrameFormat format) {
    switch (format) {
        case CameraFrameFormat::kGrayscale: return PIXFORMAT_GRAYSCALE;
        case CameraFrameFormat::kRgb565: return PIXFORMAT_RGB565;
        case CameraFrameFormat::kYuv422: return PIXFORMAT_YUV422;
        case CameraFrameFormat::kJpeg: return PIXFORMAT_JPEG;
    }
    return PIXFORMAT_GRAYSCALE;
}

static uint64_t storage_free_bytes() {
    FATFS *fs = nullptr;
    DWORD free_clusters = 0;
    if (f_getfree("/usb", &free_clusters, &fs) != FR_OK || fs == nullptr) {
        return 0;
    }
    return (uint64_t)free_clusters * (uint64_t)fs->csize * (uint64_t)FF_MIN_SS;
}

static bool storage_has_room(size_t needed_bytes) {
    const uint64_t free_bytes = storage_free_bytes();
    const uint64_t safety_margin = 64 * 1024;
    return free_bytes > ((uint64_t)needed_bytes + safety_margin);
}

static esp_err_t next_capture_paths(const char *prefix, const char *ext, char *raw_path, size_t raw_len, char *bmp_path, size_t bmp_len) {
    const char *safe_prefix = (prefix && prefix[0]) ? prefix : "capture";
    for (unsigned i = 1; i <= 999999; ++i) {
        snprintf(raw_path, raw_len, "/usb/%s_%06u.%s", safe_prefix, i, ext);
        if (access(raw_path, F_OK) == 0) continue;
        snprintf(bmp_path, bmp_len, "/usb/%s_%06u.bmp", safe_prefix, i);
        if (access(bmp_path, F_OK) == 0) continue;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

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

esp_err_t photo_storage_write_capture(const CameraFrame &frame, const char *prefix) {
    if (frame.data == nullptr || frame.size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ext = extension_for_format(frame.format);
    char raw_path[128];
    char bmp_path[128];
    esp_err_t err = next_capture_paths(prefix, ext, raw_path, sizeof(raw_path), bmp_path, sizeof(bmp_path));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No available capture file name.");
        return err;
    }

    size_t estimated_bytes = frame.size + 4096;
    if (frame.format != CameraFrameFormat::kJpeg) {
        estimated_bytes += 1078 + (size_t)frame.width * (size_t)frame.height;
    }
    if (!storage_has_room(estimated_bytes)) {
        ESP_LOGE(TAG, "Storage full: need about %u bytes, free %llu bytes.",
                 (unsigned)estimated_bytes, (unsigned long long)storage_free_bytes());
        return ESP_ERR_NO_MEM;
    }

    FILE *f_raw = fopen(raw_path, "wb");
    if (!f_raw) {
        return ESP_FAIL;
    }
    const size_t written = fwrite(frame.data, 1, frame.size, f_raw);
    fclose(f_raw);
    if (written != frame.size) {
        unlink(raw_path);
        return ESP_FAIL;
    }

    if (frame.format != CameraFrameFormat::kJpeg) {
        uint8_t *bmp_buf = nullptr;
        size_t bmp_len = 0;
        if (fmt2bmp((uint8_t *)frame.data, frame.size, frame.width, frame.height,
                    pixformat_for_frame(frame.format), &bmp_buf, &bmp_len)) {
            if (!storage_has_room(bmp_len + 4096)) {
                free(bmp_buf);
                unlink(raw_path);
                return ESP_ERR_NO_MEM;
            }
            FILE *f_bmp = fopen(bmp_path, "wb");
            if (!f_bmp) {
                free(bmp_buf);
                unlink(raw_path);
                return ESP_FAIL;
            }
            const size_t bmp_written = fwrite(bmp_buf, 1, bmp_len, f_bmp);
            fclose(f_bmp);
            free(bmp_buf);
            if (bmp_written != bmp_len) {
                unlink(raw_path);
                unlink(bmp_path);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Stored capture files: %s and %s", raw_path, bmp_path);
        } else {
            ESP_LOGW(TAG, "Stored raw capture only; BMP conversion failed for %s", raw_path);
        }
    } else {
        ESP_LOGI(TAG, "Stored capture file: %s", raw_path);
    }
    return ESP_OK;
}
