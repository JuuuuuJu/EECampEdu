// Output-class teaching demo: GPIO digital output + LEDC PWM brightness.
//
// Browser/Web Serial commands:
//   LED,1 | LED,0        logical LED on/off using gpio_set_level
//   PWM,<0-255>          LEDC PWM brightness
//   BLINK | STOP         start/stop the while(1) blink loop
//   TEST                 blink a short diagnostic pattern
//   STATUS               report gpio, polarity, level, pwm, blink state
//   PIN,<gpio>           switch teaching output pin at runtime for board debugging
//   ACTIVE,<1|0>         set LED polarity (1=active high, 0=active low)
//   LEVEL,<1|0>          raw gpio_set_level for scope/LED debugging

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEFAULT_LED_GPIO  GPIO_NUM_38
#define BLINK_PERIOD_MS   500

#define LEDC_MODE         LEDC_LOW_SPEED_MODE
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_CHANNEL      LEDC_CHANNEL_0
#define LEDC_RES          LEDC_TIMER_8_BIT
#define LEDC_FREQ_HZ      5000
#define DUTY_MAX          255

static const char *TAG = "OUTPUT_DEMO";
static gpio_num_t led_gpio = DEFAULT_LED_GPIO;
static bool led_active_high = true;
static bool blink_enabled = false;
static bool pwm_mode = false;
static int brightness = 0;
static bool led_on = false;

static int logical_to_raw(bool on) {
    return led_active_high ? (on ? 1 : 0) : (on ? 0 : 1);
}

static bool valid_output_gpio(gpio_num_t gpio) {
    return GPIO_IS_VALID_OUTPUT_GPIO(gpio);
}

static esp_err_t gpio_output_init(void) {
    if (!valid_output_gpio(led_gpio)) {
        ESP_LOGE(TAG, "GPIO%d is not a valid output GPIO", (int)led_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = gpio_reset_pin(led_gpio);
    if (err != ESP_OK) return err;
    err = gpio_set_direction(led_gpio, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) return err;
    err = gpio_set_level(led_gpio, logical_to_raw(false));
    pwm_mode = false;
    return err;
}

static esp_err_t ledc_output_init(void) {
    if (!valid_output_gpio(led_gpio)) {
        ESP_LOGE(TAG, "GPIO%d is not a valid LEDC output GPIO", (int)led_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    ledc_channel_config_t ch = {
        .gpio_num = led_gpio,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch);
    if (err == ESP_OK) pwm_mode = true;
    return err;
}

static void ensure_digital(void) {
    if (pwm_mode) {
        esp_err_t err = gpio_output_init();
        if (err != ESP_OK) printf("ERR,gpio_init,%s\n", esp_err_to_name(err));
    }
}

static void ensure_pwm(void) {
    if (!pwm_mode) {
        esp_err_t err = ledc_output_init();
        if (err != ESP_OK) printf("ERR,ledc_init,%s\n", esp_err_to_name(err));
    }
}

static void led_digital(bool on) {
    ensure_digital();
    led_on = on;
    const int raw = logical_to_raw(on);
    esp_err_t err = gpio_set_level(led_gpio, raw);
    if (err == ESP_OK) printf("LED,%s,gpio=%d,level=%d\n", on ? "ON" : "OFF", (int)led_gpio, raw);
    else printf("ERR,gpio_set_level,%s\n", esp_err_to_name(err));
    fflush(stdout);
}

static void led_raw_level(int raw) {
    ensure_digital();
    raw = raw ? 1 : 0;
    esp_err_t err = gpio_set_level(led_gpio, raw);
    led_on = led_active_high ? (raw != 0) : (raw == 0);
    if (err == ESP_OK) printf("LEVEL,%d,gpio=%d\n", raw, (int)led_gpio);
    else printf("ERR,gpio_set_level,%s\n", esp_err_to_name(err));
    fflush(stdout);
}

static void led_pwm(int value) {
    if (value < 0) value = 0;
    if (value > DUTY_MAX) value = DUTY_MAX;
    blink_enabled = false;
    ensure_pwm();
    brightness = value;
    const int duty = led_active_high ? value : (DUTY_MAX - value);
    esp_err_t err = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    if (err == ESP_OK) printf("PWM,%d,gpio=%d,duty=%d\n", brightness, (int)led_gpio, duty);
    else printf("ERR,ledc_set_duty,%s\n", esp_err_to_name(err));
    fflush(stdout);
}

static void print_help(void) {
    printf("Commands: LED,1 | LED,0 | PWM,<0-255> | BLINK | STOP | TEST | STATUS | PIN,<gpio> | ACTIVE,<1|0> | LEVEL,<1|0> | HELP\n");
    fflush(stdout);
}

static void print_status(void) {
    int level = gpio_get_level(led_gpio);
    printf("STATUS,gpio=%d,active_high=%d,level=%d,mode=%s,on=%d,pwm=%d,blink=%d\n",
           (int)led_gpio, led_active_high ? 1 : 0, level,
           pwm_mode ? "pwm" : "digital", led_on ? 1 : 0, brightness,
           blink_enabled ? 1 : 0);
    fflush(stdout);
}

static void diagnostic_test(void) {
    blink_enabled = false;
    ensure_digital();
    printf("TEST,start,gpio=%d\n", (int)led_gpio);
    fflush(stdout);
    for (int i = 0; i < 8; ++i) {
        led_digital((i % 2) == 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    led_digital(false);
    printf("TEST,done\n");
    fflush(stdout);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void set_pin(int gpio) {
    gpio_num_t next = (gpio_num_t)gpio;
    if (!valid_output_gpio(next)) {
        printf("ERR,bad_gpio,%d\n", gpio);
        fflush(stdout);
        return;
    }
    blink_enabled = false;
    led_digital(false);
    led_gpio = next;
    esp_err_t err = gpio_output_init();
    if (err == ESP_OK) printf("PIN,%d\n", gpio);
    else printf("ERR,gpio_init,%s\n", esp_err_to_name(err));
    fflush(stdout);
}

static void handle_command(char *line) {
    rstrip(line);
    char *comma = strchr(line, ',');
    int value = 0;
    if (comma) {
        *comma = '\0';
        value = atoi(comma + 1);
    }

    if (strcasecmp(line, "LED") == 0) {
        blink_enabled = false;
        led_digital(value != 0);
    } else if (strcasecmp(line, "PWM") == 0) {
        led_pwm(value);
    } else if (strcasecmp(line, "PIN") == 0) {
        set_pin(value);
    } else if (strcasecmp(line, "ACTIVE") == 0) {
        blink_enabled = false;
        led_active_high = value != 0;
        gpio_output_init();
        printf("ACTIVE,%d\n", led_active_high ? 1 : 0);
        fflush(stdout);
    } else if (strcasecmp(line, "LEVEL") == 0) {
        blink_enabled = false;
        led_raw_level(value);
    } else if (strcasecmp(line, "on") == 0) {
        blink_enabled = false;
        led_digital(true);
    } else if (strcasecmp(line, "off") == 0) {
        blink_enabled = false;
        led_digital(false);
    } else if (strcasecmp(line, "toggle") == 0) {
        blink_enabled = false;
        led_digital(!led_on);
    } else if (strcasecmp(line, "blink") == 0 || strcasecmp(line, "BLINK") == 0) {
        ensure_digital();
        blink_enabled = true;
        printf("LED,BLINK,gpio=%d\n", (int)led_gpio);
        fflush(stdout);
    } else if (strcasecmp(line, "stop") == 0 || strcasecmp(line, "STOP") == 0) {
        blink_enabled = false;
        led_digital(false);
    } else if (strcasecmp(line, "TEST") == 0) {
        diagnostic_test();
    } else if (strcasecmp(line, "status") == 0 || strcasecmp(line, "STATUS") == 0) {
        print_status();
    } else if (line[0] == '\0' || strcasecmp(line, "help") == 0 || strcasecmp(line, "HELP") == 0) {
        print_help();
    } else {
        printf("ERR,unknown_command,%s\n", line);
        print_help();
    }
}

static void serial_command_task(void *arg) {
    (void)arg;
    char line[96];
    while (true) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    esp_err_t err = gpio_output_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Output demo ready on GPIO%d active_high=%d.", (int)led_gpio, led_active_high ? 1 : 0);
    printf("READY,OUTPUT_DEMO,gpio=%d,active_high=%d\n", (int)led_gpio, led_active_high ? 1 : 0);
    print_help();

    xTaskCreate(serial_command_task, "serial_command_task", 4096, NULL, 5, NULL);

    while (true) {
        if (blink_enabled) {
            led_on = !led_on;
            gpio_set_level(led_gpio, logical_to_raw(led_on));
            vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
