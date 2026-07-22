#include "usb_composite.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"
#include "freertos/semphr.h"
#include "tinyusb_default_config.h"
#include "model_config.hpp"

static const char *TAG = "USB_COMPOSITE";

#define BASE_PATH USB_MSC_MOUNT_PATH

static QueueHandle_t usb_cdc_queue = NULL;
static tinyusb_msc_storage_handle_t storage_hdl = NULL;
static wl_handle_t global_wl_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t usb_cdc_write_mutex = NULL;
static bool is_cdc_connected = true;

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle) {
    ESP_LOGI(TAG, "Initializing wear levelling...");
    const esp_partition_t *data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition.");
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}

// CDC Line state callback
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

    // Initialize SPI Flash Wear-Levelling
    ESP_ERROR_CHECK(storage_init_spiflash(&global_wl_handle));

    // MSC Configuration
    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.medium.wl_handle = global_wl_handle;
    storage_cfg.fat_fs.base_path = (char *)BASE_PATH;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.config.format_if_mount_failed = true;
    storage_cfg.fat_fs.do_not_format = false;
    storage_cfg.fat_fs.format_flags = 0;
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_cfg, &storage_hdl));

    // Install TinyUSB Driver
    ESP_LOGI(TAG, "USB Composite driver installation...");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // Register CDC ACM Driver Callbacks
    tinyusb_config_cdcacm_t amc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &tinyusb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = NULL
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&amc_cfg));

    ESP_LOGI(TAG, "USB Composite initialization DONE");
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
            vTaskDelay(pdMS_TO_TICKS(10)); // Safe delay to let USB engine transmit
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
        usb_cdc_write((const uint8_t*)buffer, len);
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

esp_err_t usb_msc_mount_to_app() {
    if (!storage_hdl) {
        ESP_LOGE(TAG, "MSC storage handle is not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
    return ESP_OK;
}

esp_err_t usb_msc_mount_to_pc() {
    if (!storage_hdl) {
        ESP_LOGE(TAG, "MSC storage handle is not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
    return ESP_OK;
}

bool usb_cdc_is_connected() {
    return is_cdc_connected;
}
