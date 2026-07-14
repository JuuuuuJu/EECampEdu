#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
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

static const size_t RAW_BENCH_BYTES = 64 * 1024;
static const int TIMED_BENCH_DEFAULT_SECONDS = 10 * 60;
static const int TIMED_BENCH_PROGRESS_SECONDS = 30;
static const int FRAME_BENCH_COUNT = 50;

// 1x1 JPEG base64 payload. The protocol is the same frame envelope used by the PC App.
static const char kTinyJpegBase64[] =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////"
    "2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QA"
    "FQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oA"
    "CAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/ASf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/ASf/xAAUEAEA"
    "AAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/Al//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/IV//2gAMAwEAAgADAAAAEP/EFBQRAQAA"
    "AAAAAAAAAAAAAAAAABD/2gAIAQMBAT8QH//EFBQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIBAT8QH//EFBQBAQAAAAAAAAAAAAAAAAAAABD/"
    "2gAIAQEAAT8QH//Z";

static size_t cdc_write_block(const char *text) {
    if (g_write_lock == NULL || text == NULL) {
        return 0;
    }
    xSemaphoreTake(g_write_lock, portMAX_DELAY);
    const uint8_t *buf = (const uint8_t *)text;
    const size_t len = strlen(text);
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
    return len;
}

static size_t send_test_frame(void) {
    char header[96];
    snprintf(header, sizeof(header), "---START_IMAGE:4:1:1:%u---\n", (unsigned)(sizeof(kTinyJpegBase64) - 1));
    size_t bytes = 0;
    bytes += cdc_write_block(header);
    bytes += cdc_write_block(kTinyJpegBase64);
    bytes += cdc_write_block("\n---END_IMAGE---\n");
    ESP_LOGI(TAG, "sent CDC JPEG test frame");
    return bytes;
}

static void run_raw_cdc_benchmark(size_t bytes_to_send) {
    static char chunk[256];
    memset(chunk, 'A', sizeof(chunk) - 2);
    chunk[sizeof(chunk) - 2] = '\n';
    chunk[sizeof(chunk) - 1] = '\0';

    char line[192];
    snprintf(line, sizeof(line), "USB_CDC_BENCH_BEGIN,mode=raw,bytes=%u\n", (unsigned)bytes_to_send);
    cdc_write_block(line);

    const int64_t start_us = esp_timer_get_time();
    size_t sent = 0;
    while (sent < bytes_to_send) {
        const size_t remaining = bytes_to_send - sent;
        if (remaining >= sizeof(chunk) - 1) {
            sent += cdc_write_block(chunk);
        } else {
            char tail[256];
            memset(tail, 'A', remaining);
            tail[remaining] = '\0';
            sent += cdc_write_block(tail);
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const double seconds = (double)elapsed_us / 1000000.0;
    const double kbps = seconds > 0.0 ? ((double)sent / 1024.0) / seconds : 0.0;

    snprintf(line, sizeof(line), "\nUSB_CDC_BENCH_RESULT,mode=raw,bytes=%u,elapsed_us=%lld,kBps=%.2f\n",
             (unsigned)sent,
             (long long)elapsed_us,
             kbps);
    cdc_write_block(line);
}

static void run_timed_raw_cdc_benchmark(int seconds_to_run) {
    static char chunk[256];
    memset(chunk, 'U', sizeof(chunk) - 2);
    chunk[sizeof(chunk) - 2] = '\n';
    chunk[sizeof(chunk) - 1] = '\0';

    if (seconds_to_run <= 0) {
        seconds_to_run = TIMED_BENCH_DEFAULT_SECONDS;
    }

    char line[224];
    snprintf(line,
             sizeof(line),
             "USB_CDC_BENCH_BEGIN,mode=timed_raw,duration_s=%d\n",
             seconds_to_run);
    cdc_write_block(line);

    const int64_t start_us = esp_timer_get_time();
    const int64_t duration_us = (int64_t)seconds_to_run * 1000000LL;
    int64_t next_progress_us = (int64_t)TIMED_BENCH_PROGRESS_SECONDS * 1000000LL;
    uint64_t sent = 0;

    while (true) {
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= duration_us) {
            break;
        }

        sent += cdc_write_block(chunk);

        if (elapsed_us >= next_progress_us) {
            const double seconds = (double)elapsed_us / 1000000.0;
            const double kbps = seconds > 0.0 ? ((double)sent / 1024.0) / seconds : 0.0;
            snprintf(line,
                     sizeof(line),
                     "\nUSB_CDC_BENCH_PROGRESS,mode=timed_raw,elapsed_s=%.1f,bytes=%llu,kBps=%.2f\n",
                     seconds,
                     (unsigned long long)sent,
                     kbps);
            cdc_write_block(line);
            next_progress_us += (int64_t)TIMED_BENCH_PROGRESS_SECONDS * 1000000LL;
        }
    }

    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const double seconds = (double)elapsed_us / 1000000.0;
    const double kbps = seconds > 0.0 ? ((double)sent / 1024.0) / seconds : 0.0;
    snprintf(line,
             sizeof(line),
             "\nUSB_CDC_BENCH_RESULT,mode=timed_raw,duration_s=%d,bytes=%llu,elapsed_us=%lld,kBps=%.2f\n",
             seconds_to_run,
             (unsigned long long)sent,
             (long long)elapsed_us,
             kbps);
    cdc_write_block(line);
}

static void run_frame_cdc_benchmark(int frames) {
    char line[192];
    snprintf(line, sizeof(line), "USB_CDC_BENCH_BEGIN,mode=frame,frames=%d\n", frames);
    cdc_write_block(line);

    const int64_t start_us = esp_timer_get_time();
    size_t sent = 0;
    for (int i = 0; i < frames; ++i) {
        sent += send_test_frame();
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const double seconds = (double)elapsed_us / 1000000.0;
    const double kbps = seconds > 0.0 ? ((double)sent / 1024.0) / seconds : 0.0;
    const double fps = seconds > 0.0 ? (double)frames / seconds : 0.0;

    snprintf(line, sizeof(line), "USB_CDC_BENCH_RESULT,mode=frame,frames=%d,bytes=%u,elapsed_us=%lld,kBps=%.2f,fps=%.2f\n",
             frames,
             (unsigned)sent,
             (long long)elapsed_us,
             kbps,
             fps);
    cdc_write_block(line);
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

static void normalize_command(char *cmd) {
    char *start = cmd;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }
    if (start != cmd) {
        memmove(cmd, start, strlen(start) + 1);
    }

    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t' || cmd[len - 1] == '\r' || cmd[len - 1] == '\n')) {
        cmd[--len] = '\0';
    }

    for (size_t i = 0; cmd[i] != '\0'; ++i) {
        cmd[i] = (char)toupper((unsigned char)cmd[i]);
    }
}

static bool command_is_one_of(const char *cmd, const char *a, const char *b, const char *c) {
    return strcmp(cmd, a) == 0 || strcmp(cmd, b) == 0 || strcmp(cmd, c) == 0;
}

void app_main(void) {
    usb_init();
    ESP_LOGI(TAG, "READY,USB_CDC_JPEG_TEST");
    ESP_LOGI(TAG, "APP commands: D1=start stream, D0=stop stream, C=single frame");
    ESP_LOGI(TAG, "Manual commands: S=start stream, X=stop stream, F=single frame, B=raw CDC benchmark, B<n>=raw n KiB, T=10min timed raw, T<n>=timed raw seconds, FBENCH=frame benchmark");

    bool stream = false;
    int idle_ticks = 0;
    uint8_t cmd_raw[128];
    char cmd[128];
    while (true) {
        if (xQueueReceive(g_rx_queue, cmd_raw, pdMS_TO_TICKS(100)) == pdTRUE) {
            strlcpy(cmd, (const char *)cmd_raw, sizeof(cmd));
            normalize_command(cmd);

            if (command_is_one_of(cmd, "C", "F", "CAPTURE")) {
                cdc_write_block("USB_TEST,single_frame\n");
                send_test_frame();
            } else if (command_is_one_of(cmd, "D1", "S", "STREAM_ON")) {
                stream = true;
                idle_ticks = 0;
                cdc_write_block("USB_TEST,stream=on\n");
                send_test_frame();
            } else if (command_is_one_of(cmd, "D0", "X", "STREAM_OFF")) {
                stream = false;
                cdc_write_block("USB_TEST,stream=off\n");
            } else if (strcmp(cmd, "B") == 0 || strcmp(cmd, "BENCH") == 0) {
                stream = false;
                run_raw_cdc_benchmark(RAW_BENCH_BYTES);
            } else if (cmd[0] == 'B' && isdigit((unsigned char)cmd[1])) {
                stream = false;
                const size_t kb = (size_t)atoi(cmd + 1);
                run_raw_cdc_benchmark(kb * 1024);
            } else if (strcmp(cmd, "T") == 0 || strcmp(cmd, "SOAK") == 0 || strcmp(cmd, "TIMED") == 0) {
                stream = false;
                run_timed_raw_cdc_benchmark(TIMED_BENCH_DEFAULT_SECONDS);
            } else if (cmd[0] == 'T' && isdigit((unsigned char)cmd[1])) {
                stream = false;
                run_timed_raw_cdc_benchmark(atoi(cmd + 1));
            } else if (strncmp(cmd, "SOAK", 4) == 0 && isdigit((unsigned char)cmd[4])) {
                stream = false;
                run_timed_raw_cdc_benchmark(atoi(cmd + 4));
            } else if (strcmp(cmd, "FBENCH") == 0 || strcmp(cmd, "FRAME_BENCH") == 0) {
                stream = false;
                run_frame_cdc_benchmark(FRAME_BENCH_COUNT);
            } else if (strcmp(cmd, "HELP") == 0 || strcmp(cmd, "?") == 0) {
                cdc_write_block("USB_TEST,commands=D1,D0,C,S,X,F,B,B<n>,T,T<n>,SOAK,SOAK<n>,FBENCH\n");
            } else if (cmd[0] != '\0') {
                cdc_write_block("USB_TEST,ignored_command\n");
            }
        }

        if (stream && g_connected && ++idle_ticks >= 10) {
            idle_ticks = 0;
            send_test_frame();
        }
    }
}
