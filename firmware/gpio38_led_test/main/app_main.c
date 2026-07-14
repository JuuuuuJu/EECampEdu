#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO GPIO_NUM_38
#define LED_ACTIVE_HIGH 1
#define BLINK_PERIOD_MS 500

static const char *TAG = "GPIO38_LED";
static bool led_on = false;
static bool blink_enabled = false;

static void led_apply(void) {
    int level = led_on ? 1 : 0;
#if !LED_ACTIVE_HIGH
    level = !level;
#endif
    gpio_set_level(LED_GPIO, level);
}

static void led_set(bool on) {
    led_on = on;
    led_apply();
    printf("LED,%s\n", led_on ? "ON" : "OFF");
    fflush(stdout);
}

static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    led_set(false);
}

static void print_help(void) {
    printf("Commands: on, off, toggle, blink, stop, status, help\n");
    fflush(stdout);
}

static void handle_command(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    if (strcmp(line, "on") == 0) {
        blink_enabled = false;
        led_set(true);
    } else if (strcmp(line, "off") == 0) {
        blink_enabled = false;
        led_set(false);
    } else if (strcmp(line, "toggle") == 0) {
        blink_enabled = false;
        led_set(!led_on);
    } else if (strcmp(line, "blink") == 0) {
        blink_enabled = true;
        printf("LED,BLINK\n");
        fflush(stdout);
    } else if (strcmp(line, "stop") == 0) {
        blink_enabled = false;
        led_set(false);
    } else if (strcmp(line, "status") == 0) {
        printf("LED_STATUS,gpio=%d,on=%d,blink=%d,active_high=%d\n", (int)LED_GPIO, led_on ? 1 : 0, blink_enabled ? 1 : 0, LED_ACTIVE_HIGH);
        fflush(stdout);
    } else if (strcmp(line, "help") == 0 || line[0] == '\0') {
        print_help();
    } else {
        printf("ERR,unknown_command,%s\n", line);
        print_help();
    }
}

void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    led_init();
    ESP_LOGI(TAG, "GPIO38 LED test ready.");
    printf("READY,GPIO38_LED_TEST\n");
    print_help();

    char line[64];
    while (true) {
        if (blink_enabled) {
            led_on = !led_on;
            led_apply();
            vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
        }

        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
