#include "photo_storage.hpp"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "model_config.hpp"

static const char *TAG = "PHOTO_STORAGE";
static const uint32_t kPhotoMagic = 0x45454350; // "EECP"
static const uint32_t kPhotoVersion = 1;
static const size_t kFlashEraseSectorSize = 4096;

struct PhotoHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;
    uint16_t width;
    uint16_t height;
    uint8_t format;
    uint8_t reserved[7];
};

static const esp_partition_t *g_photo_partition = nullptr;

static size_t align_up(size_t value, size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

static uint8_t format_to_storage(CameraFrameFormat format) {
    switch (format) {
        case CameraFrameFormat::kGrayscale:
            return 0;
        case CameraFrameFormat::kRgb565:
            return 1;
        case CameraFrameFormat::kYuv422:
            return 2;
        case CameraFrameFormat::kJpeg:
            return 3;
    }
    return 255;
}

esp_err_t photo_storage_init() {
    g_photo_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        PHOTOS_PARTITION_LABEL);
    if (g_photo_partition == nullptr) {
        ESP_LOGE(TAG, "Photo partition '%s' not found.", PHOTOS_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "Photo partition: label=%s offset=0x%lx size=%lu bytes",
             g_photo_partition->label,
             (unsigned long)g_photo_partition->address,
             (unsigned long)g_photo_partition->size);
    return ESP_OK;
}

esp_err_t photo_storage_write_latest(const CameraFrame &frame) {
    if (g_photo_partition == nullptr) {
        ESP_RETURN_ON_ERROR(photo_storage_init(), TAG, "photo storage init failed");
    }
    if (frame.data == nullptr || frame.size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total_bytes = sizeof(PhotoHeader) + frame.size;
    if (total_bytes > g_photo_partition->size) {
        ESP_LOGE(TAG,
                 "Photo is too large: %u bytes, partition size=%u bytes.",
                 (unsigned)total_bytes,
                 (unsigned)g_photo_partition->size);
        return ESP_ERR_NO_MEM;
    }

    PhotoHeader header = {};
    header.magic = kPhotoMagic;
    header.version = kPhotoVersion;
    header.bytes = (uint32_t)frame.size;
    header.width = (uint16_t)frame.width;
    header.height = (uint16_t)frame.height;
    header.format = format_to_storage(frame.format);

    const size_t erase_bytes = align_up(total_bytes, kFlashEraseSectorSize);
    if (erase_bytes > g_photo_partition->size) {
        ESP_LOGE(TAG,
                 "Photo erase range is too large after sector alignment: %u bytes, partition size=%u bytes.",
                 (unsigned)erase_bytes,
                 (unsigned)g_photo_partition->size);
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(g_photo_partition, 0, erase_bytes),
                        TAG,
                        "erase latest photo range failed");
    ESP_RETURN_ON_ERROR(esp_partition_write(g_photo_partition, 0, &header, sizeof(header)),
                        TAG,
                        "write latest photo header failed");
    ESP_RETURN_ON_ERROR(esp_partition_write(g_photo_partition, sizeof(header), frame.data, frame.size),
                        TAG,
                        "write latest photo payload failed");

    ESP_LOGI(TAG,
             "Stored latest photo: %lu bytes, %dx%d, format=%u.",
             (unsigned long)frame.size,
             frame.width,
             frame.height,
             (unsigned)header.format);
    return ESP_OK;
}

esp_err_t photo_storage_read_latest(uint8_t *buffer, size_t buffer_size, StoredPhotoMetadata *metadata) {
    if (g_photo_partition == nullptr) {
        ESP_RETURN_ON_ERROR(photo_storage_init(), TAG, "photo storage init failed");
    }
    if (buffer == nullptr || metadata == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    PhotoHeader header = {};
    ESP_RETURN_ON_ERROR(esp_partition_read(g_photo_partition, 0, &header, sizeof(header)),
                        TAG,
                        "read latest photo header failed");
    if (header.magic != kPhotoMagic || header.version != kPhotoVersion) {
        ESP_LOGE(TAG, "No valid latest photo found in partition.");
        return ESP_ERR_INVALID_STATE;
    }
    if (header.bytes > buffer_size) {
        ESP_LOGE(TAG,
                 "Read buffer too small: need %lu bytes, have %u bytes.",
                 (unsigned long)header.bytes,
                 (unsigned)buffer_size);
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_partition_read(g_photo_partition, sizeof(header), buffer, header.bytes),
                        TAG,
                        "read latest photo payload failed");

    metadata->bytes = header.bytes;
    metadata->width = header.width;
    metadata->height = header.height;
    metadata->format = header.format;
    return ESP_OK;
}
