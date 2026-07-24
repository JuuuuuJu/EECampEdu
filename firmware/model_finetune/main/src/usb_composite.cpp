#include "usb_composite.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "model_config.hpp"
#include "usb_descriptors.h"

static const char *TAG = "USB_COMPOSITE";

static QueueHandle_t usb_cdc_queue = NULL;
static SemaphoreHandle_t usb_cdc_write_mutex = NULL;
static bool is_cdc_connected = true;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// UVC streaming state. tud_video_n_frame_xfer() takes a whole JPEG frame; we
// gate on this flag so we only submit one frame at a time.
static volatile bool s_uvc_tx_busy = false;

// Mount the FAT storage partition to /usb so photo_storage can write there.
// Previously this happened as a side effect of MSC init; MSC is gone now, so
// the app side simply owns the filesystem permanently.
static esp_err_t mount_photo_storage() {
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        USB_MSC_MOUNT_PATH, STORAGE_PARTITION_LABEL, &mount_cfg, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FAT storage at %s: %s",
                 USB_MSC_MOUNT_PATH, esp_err_to_name(err));
    }
    return err;
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

    ESP_ERROR_CHECK(mount_photo_storage());

    // Install TinyUSB with our custom UVC + CDC composite descriptors.
    ESP_LOGI(TAG, "Installing UVC + CDC composite USB driver...");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &model_finetune_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = model_finetune_fs_config_descriptor;
    tusb_cfg.descriptor.string = model_finetune_string_descriptors;
    tusb_cfg.descriptor.string_count = model_finetune_string_descriptor_count;
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

    ESP_LOGI(TAG, "USB composite (UVC + CDC) initialization DONE");
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
