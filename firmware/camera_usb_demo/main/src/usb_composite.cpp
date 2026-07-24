#include "usb_composite.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "model_config.hpp"
#include "usb_descriptors.h"

static const char *TAG = "USB_COMPOSITE";

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static QueueHandle_t usb_cdc_queue = NULL;
static SemaphoreHandle_t usb_cdc_write_mutex = NULL;
static SemaphoreHandle_t storage_owner_mutex = NULL;
static bool is_cdc_connected = true;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_storage_handle = NULL;

// UVC streaming state. tud_video_n_frame_xfer() takes a whole JPEG frame; we
// gate on this flag so we only submit one frame at a time.
static volatile bool s_uvc_tx_busy = false;

static esp_err_t init_storage_medium() {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        STORAGE_PARTITION_LABEL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "FAT storage partition '%s' not found",
                 STORAGE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(partition, &s_wl_handle);
}

static void storage_event_callback(tinyusb_msc_storage_handle_t handle,
                                   tinyusb_msc_event_t *event, void *arg) {
    (void)handle;
    (void)arg;
    const char *owner =
        event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP ? "app" : "pc";
    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
            ESP_LOGI(TAG, "Storage ownership changed: owner=%s", owner);
            break;
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        case TINYUSB_MSC_EVENT_FORMAT_FAILED:
            ESP_LOGE(TAG, "Storage transition failed: event=%d owner=%s",
                     (int)event->id, owner);
            break;
        default:
            ESP_LOGD(TAG, "Storage event=%d owner=%s", (int)event->id, owner);
            break;
    }
}

// CDC line state callback
void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event) {
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "Line state on CDC %d: DTR:%d, RTS:%d", itf, dtr, rts);
    is_cdc_connected = (dtr && rts);
}

// CDC RX callback
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event) {
    size_t rx_size = 0;
    uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    esp_err_t ret = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK && rx_size > 0) {
        usb_cdc_msg_t tx_msg;
        tx_msg.buf_len = rx_size;
        tx_msg.itf = itf;
        memcpy(tx_msg.buf, rx_buf, rx_size);
        tx_msg.buf[rx_size] = '\0';
        xQueueSend(usb_cdc_queue, &tx_msg, 0);
    }
}

void usb_composite_init() {
    usb_cdc_queue = xQueueCreate(5, sizeof(usb_cdc_msg_t));
    configASSERT(usb_cdc_queue);

    usb_cdc_write_mutex = xSemaphoreCreateMutex();
    configASSERT(usb_cdc_write_mutex);

    storage_owner_mutex = xSemaphoreCreateMutex();
    configASSERT(storage_owner_mutex);

    ESP_ERROR_CHECK(init_storage_medium());

    // Start with the FAT volume exposed to the host. Espressif's MSC
    // START_STOP_UNIT callback mounts it at /usb after a logical host eject.
    const tinyusb_msc_driver_config_t driver_cfg = {
        .user_flags = {},
        .callback = storage_event_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&driver_cfg));

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.medium.wl_handle = s_wl_handle;
    storage_cfg.fat_fs.base_path = (char *)USB_MSC_MOUNT_PATH;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.config.format_if_mount_failed = true;
    storage_cfg.fat_fs.config.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    storage_cfg.fat_fs.do_not_format = false;
    storage_cfg.fat_fs.format_flags = 0;
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(
        &storage_cfg, &s_storage_handle));

    // Install TinyUSB with our custom UVC + CDC + MSC descriptors.
    ESP_LOGI(TAG, "Installing UVC + CDC + MSC composite USB driver...");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &camusb_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = camusb_fs_config_descriptor;
    tusb_cfg.descriptor.string = camusb_string_descriptors;
    tusb_cfg.descriptor.string_count = camusb_string_descriptor_count;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // Register the CDC-ACM driver on instance 0 (interfaces 0/1 in our descriptor).
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &tinyusb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = NULL
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    ESP_LOGI(TAG, "USB composite initialized; storage owner=pc");
}

bool usb_storage_is_app_owned() {
    if (s_storage_handle == NULL) {
        return false;
    }
    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    return tinyusb_msc_get_storage_mount_point(
               s_storage_handle, &mount_point) == ESP_OK &&
           mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
}

bool usb_storage_acquire_app(TickType_t timeout) {
    if (storage_owner_mutex == NULL ||
        xSemaphoreTake(storage_owner_mutex, timeout) != pdTRUE) {
        return false;
    }
    if (!usb_storage_is_app_owned()) {
        xSemaphoreGive(storage_owner_mutex);
        return false;
    }
    return true;
}

void usb_storage_release_app() {
    if (storage_owner_mutex != NULL) {
        xSemaphoreGive(storage_owner_mutex);
    }
}

esp_err_t usb_storage_request_pc() {
    if (s_storage_handle == NULL || storage_owner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(storage_owner_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = tinyusb_msc_set_storage_mount_point(
        s_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    xSemaphoreGive(storage_owner_mutex);
    return err;
}

const char *usb_storage_owner_name() {
    return usb_storage_is_app_owned() ? "app" : "pc";
}

void usb_cdc_write(const uint8_t *buf, size_t len) {
    if (usb_cdc_write_mutex == NULL) return;
    xSemaphoreTake(usb_cdc_write_mutex, portMAX_DELAY);

    size_t written = 0;
    while (written < len) {
        size_t ret = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf + written, len - written);
        if (ret > 0) {
            written += ret;
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        } else {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            vTaskDelay(pdMS_TO_TICKS(10)); // let the USB engine drain
        }
    }

    xSemaphoreGive(usb_cdc_write_mutex);
}

int usb_cdc_printf(const char *format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len > 0) {
        usb_cdc_write((const uint8_t *)buffer, (size_t)len);
    }
    return len;
}

void usb_cdc_write_base64(const uint8_t *data, size_t length) {
    size_t i = 0;
    uint8_t a, b, c;
    uint32_t combined;
    uint8_t out_buf[256];
    size_t out_idx = 0;
    size_t bytes_since_yield = 0;

    while (i + 3 <= length) {
        a = data[i++];
        b = data[i++];
        c = data[i++];
        combined = (a << 16) | (b << 8) | c;

        out_buf[out_idx++] = b64_table[(combined >> 18) & 0x3F];
        out_buf[out_idx++] = b64_table[(combined >> 12) & 0x3F];
        out_buf[out_idx++] = b64_table[(combined >> 6) & 0x3F];
        out_buf[out_idx++] = b64_table[combined & 0x3F];

        if (out_idx >= 240) {
            usb_cdc_write(out_buf, out_idx);
            uint8_t nl = '\n';
            usb_cdc_write(&nl, 1);
            bytes_since_yield += (out_idx + 1);
            out_idx = 0;

            if (bytes_since_yield >= 2048) {
                vTaskDelay(pdMS_TO_TICKS(10)); // Safe delay yielding CPU to keep watchdog happy
                bytes_since_yield = 0;
            }
        }
    }

    if (i < length) {
        a = data[i++];
        if (i < length) {
            b = data[i++];
            combined = (a << 16) | (b << 8);
            out_buf[out_idx++] = b64_table[(combined >> 18) & 0x3F];
            out_buf[out_idx++] = b64_table[(combined >> 12) & 0x3F];
            out_buf[out_idx++] = b64_table[(combined >> 6) & 0x3F];
            out_buf[out_idx++] = '=';
        } else {
            combined = (a << 16);
            out_buf[out_idx++] = b64_table[(combined >> 18) & 0x3F];
            out_buf[out_idx++] = b64_table[(combined >> 12) & 0x3F];
            out_buf[out_idx++] = '=';
            out_buf[out_idx++] = '=';
        }
    }

    if (out_idx > 0) {
        usb_cdc_write(out_buf, out_idx);
    }
    uint8_t nl = '\n';
    usb_cdc_write(&nl, 1);
}

QueueHandle_t usb_cdc_get_queue() {
    return usb_cdc_queue;
}

bool usb_cdc_is_connected() {
    return is_cdc_connected;
}

//--------------------------------------------------------------------+
// UVC video streaming
//--------------------------------------------------------------------+
bool uvc_video_ready() {
    return tud_video_n_streaming(0, 0);
}

bool uvc_video_can_send() {
    return tud_video_n_streaming(0, 0) && !s_uvc_tx_busy;
}

bool uvc_video_submit_frame(const uint8_t *jpeg, size_t len) {
    if (!tud_video_n_streaming(0, 0)) {
        s_uvc_tx_busy = false;
        return false;
    }
    if (s_uvc_tx_busy) {
        return false;
    }
    s_uvc_tx_busy = true;
    // tud_video_n_frame_xfer returns non-zero on success (frame queued).
    if (tud_video_n_frame_xfer(0, 0, (void *)jpeg, len) == 0) {
        s_uvc_tx_busy = false;
        return false;
    }
    return true;
}

// Called by TinyUSB when the whole frame has been transmitted.
extern "C" void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx) {
    (void)ctl_idx;
    (void)stm_idx;
    s_uvc_tx_busy = false;
}

// Called by TinyUSB when the host commits a video format/frame (stream start).
extern "C" int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
                                   video_probe_and_commit_control_t const *parameters) {
    (void)ctl_idx;
    (void)stm_idx;
    (void)parameters;
    s_uvc_tx_busy = false;
    return VIDEO_ERROR_NONE;
}
