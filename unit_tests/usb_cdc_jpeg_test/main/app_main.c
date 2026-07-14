#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

static const char *TAG = "USB_CDC_UNIT_TEST";

static QueueHandle_t g_rx_queue;
static SemaphoreHandle_t g_write_lock;
static volatile bool g_connected = true;

// 1x1 JPEG base64 payload. The protocol is the same frame envelope used by the PC App.
static const char kTinyJpegBase64[] =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////"
    "2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QA"
    "FQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oA"
    "CAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/ASf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/ASf/xAAUEAEA"
    "AAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/Al//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/IV//2gAMAwEAAgADAAAAEP/EFBQRAQAA"
    "AAAAAAAAAAAAAAAAABD/2gAIAQMBAT8QH//EFBQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIBAT8QH//EFBQBAQAAAAAAAAAAAAAAAAAAABD/"
    "2gAIAQEAAT8QH//Z";

static void cdc_write_block(const char *text) {
    if (g_write_lock == NULL || text == NULL) {
        return;
    }
    xSemaphoreTake(g_write_lock, portMAX_DELAY);
    const uint8_t *buf = (const uint8_t *)text;
    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf + written, len - written);
        if (n > 0) {
            written += n;
        }
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    xSemaphoreGive(g_write_lock);
}

static void send_test_frame(void) {
    char header[96];
    snprintf(header, sizeof(header), "---START_IMAGE:4:1:1:%u---\n", (unsigned)(sizeof(kTinyJpegBase64) - 1));
    cdc_write_block(header);
    cdc_write_block(kTinyJpegBase64);
    cdc_write_block("\n---END_IMAGE---\n");
    ESP_LOGI(TAG, "sent CDC JPEG test frame");
}

void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event) {
    (void)itf;
    g_connected = event->line_state_changed_data.dtr && event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC line state: DTR=%d RTS=%d", event->line_state_changed_data.dtr, event->line_state_changed_data.rts);
}

void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event) {
    (void)event;
    uint8_t rx[128];
    size_t rx_size = 0;
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, rx, sizeof(rx) - 1, &rx_size) == ESP_OK && rx_size > 0) {
        rx[rx_size] = 0;
        xQueueSend(g_rx_queue, rx, 0);
    }
}

static void usb_init(void) {
    g_rx_queue = xQueueCreate(8, 128);
    g_write_lock = xSemaphoreCreateMutex();
    configASSERT(g_rx_queue);
    configASSERT(g_write_lock);

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &tinyusb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));
}

void app_main(void) {
    usb_init();
    ESP_LOGI(TAG, "READY,USB_CDC_JPEG_TEST");
    ESP_LOGI(TAG, "Commands over CDC: f=send one frame, s=start periodic stream, x=stop stream");

    bool stream = false;
    int idle_ticks = 0;
    uint8_t cmd[128];
    while (true) {
        if (xQueueReceive(g_rx_queue, cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (strchr((const char *)cmd, 'f') != NULL) {
                send_test_frame();
            } else if (strchr((const char *)cmd, 's') != NULL) {
                stream = true;
                cdc_write_block("USB_TEST,stream=on\n");
            } else if (strchr((const char *)cmd, 'x') != NULL) {
                stream = false;
                cdc_write_block("USB_TEST,stream=off\n");
            } else {
                cdc_write_block("USB_TEST,commands=f,s,x\n");
            }
        }

        if (stream && g_connected && ++idle_ticks >= 10) {
            idle_ticks = 0;
            send_test_frame();
        }
    }
}
