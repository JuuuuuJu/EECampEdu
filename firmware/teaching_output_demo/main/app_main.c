// Output-class teaching demo: GPIO digital output + LEDC PWM brightness.
//
// Recovered and adapted from the old firmware/gpio38_led_test unit test. It keeps
// the simple line-based USB-serial command protocol so the AI PC portal (Output
// Demo tab) can drive the LED directly from the browser over Web Serial.
//
// Teaching APIs shown here (match the class slides):
//   * gpio_reset_pin        - reset a pin to a known default state
//   * gpio_set_direction    - configure the pin as an output
//   * gpio_set_level        - drive the pin HIGH/LOW (digital LED on/off, blink)
//   * LEDC (ledc_timer_config / ledc_channel_config / ledc_set_duty) - PWM brightness
//   * a repeated blink loop inside while(1)
//
// Serial commands (one per line, newline-terminated):
//   LED,1 | LED,0        digital on/off (gpio_set_level)
//   PWM,<0-255>          PWM brightness (LEDC duty)
//   BLINK | STOP         start/stop the while(1) blink
//   STATUS | HELP        report state / list commands
// Legacy aliases: on, off, toggle, blink, stop, status, help.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO          GPIO_NUM_38   // onboard LED on many ESP32-S3 dev boards
#define LED_ACTIVE_HIGH   1
#define BLINK_PERIOD_MS   500

#define LEDC_MODE         LEDC_LOW_SPEED_MODE
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_CHANNEL      LEDC_CHANNEL_0
#define LEDC_RES          LEDC_TIMER_8_BIT   // duty range 0..255
#define LEDC_FREQ_HZ      5000
#define DUTY_MAX          255

static const char *TAG = "OUTPUT_DEMO";
static bool blink_enabled = false;
static bool pwm_mode = false;     // false: digital gpio_set_level; true: LEDC PWM
static int  brightness = 0;       // 0..255 (PWM duty when pwm_mode)
static bool led_on = false;       // digital state when !pwm_mode

// --- digital output (gpio_set_level) ---
static void gpio_output_init(void) {
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_ACTIVE_HIGH ? 0 : 1);
}

static void led_digital(bool on) {
    pwm_mode = false;
    led_on = on;
    int level = on ? 1 : 0;
#if !LED_ACTIVE_HIGH
    level = !level;
#endif
    gpio_set_level(LED_GPIO, level);
    printf("LED,%s\n", on ? "ON" : "OFF");
    fflush(stdout);
}

// --- PWM output (LEDC) ---
static void ledc_output_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE, .duty_resolution = LEDC_RES,
        .timer_num = LEDC_TIMER, .freq_hz = LEDC_FREQ_HZ, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = LED_GPIO, .speed_mode = LEDC_MODE, .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER, .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&ch);
}

static void led_pwm(int value) {
    if (value < 0) value = 0;
    if (value > DUTY_MAX) value = DUTY_MAX;
    brightness = value;
    pwm_mode = true;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LED_ACTIVE_HIGH ? value : (DUTY_MAX - value));
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    printf("PWM,%d\n", brightness);
    fflush(stdout);
}

// Switching between digital and PWM: LEDC keeps control of the pin once attached,
// so re-init the matching driver when the mode changes.
static void use_digital(void) { if (pwm_mode) { gpio_output_init(); } }
static void use_pwm(void)     { if (!pwm_mode) { ledc_output_init(); } }

static void print_help(void) {
    printf("Commands: LED,1 | LED,0 | PWM,<0-255> | BLINK | STOP | STATUS | HELP\n");
    fflush(stdout);
}
static void print_status(void) {
    printf("STATUS,gpio=%d,mode=%s,on=%d,pwm=%d,blink=%d\n",
           (int)LED_GPIO, pwm_mode ? "pwm" : "digital",
           led_on ? 1 : 0, brightness, blink_enabled ? 1 : 0);
    fflush(stdout);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n] = '\0';
}

static void handle_command(char *line) {
    rstrip(line);
    // key[,value]
    char *comma = strchr(line, ',');
    int value = 0;
    if (comma) { *comma = '\0'; value = atoi(comma + 1); }

    if (strcasecmp(line, "LED") == 0) {           // LED,1 / LED,0
        blink_enabled = false; use_digital(); led_digital(value != 0);
    } else if (strcasecmp(line, "PWM") == 0) {    // PWM,<0-255>
        blink_enabled = false; use_pwm(); led_pwm(value);
    } else if (strcasecmp(line, "on") == 0) {
        blink_enabled = false; use_digital(); led_digital(true);
    } else if (strcasecmp(line, "off") == 0) {
        blink_enabled = false; use_digital(); led_digital(false);
    } else if (strcasecmp(line, "toggle") == 0) {
        blink_enabled = false; use_digital(); led_digital(!led_on);
    } else if (strcasecmp(line, "blink") == 0 || strcasecmp(line, "BLINK") == 0) {
        use_digital(); blink_enabled = true; printf("LED,BLINK\n"); fflush(stdout);
    } else if (strcasecmp(line, "stop") == 0 || strcasecmp(line, "STOP") == 0) {
        blink_enabled = false; use_digital(); led_digital(false);
    } else if (strcasecmp(line, "status") == 0 || strcasecmp(line, "STATUS") == 0) {
        print_status();
    } else if (line[0] == '\0' || strcasecmp(line, "help") == 0 || strcasecmp(line, "HELP") == 0) {
        print_help();
    } else {
        printf("ERR,unknown_command,%s\n", line);
        print_help();
    }
}

void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    gpio_output_init();
    ESP_LOGI(TAG, "Output demo ready on GPIO%d.", (int)LED_GPIO);
    printf("READY,OUTPUT_DEMO\n");
    print_help();

    char line[64];
    while (true) {
        // Repeated LED blink inside while(1) (classic teaching loop).
        if (blink_enabled) {
            led_on = !led_on;
            int level = led_on ? 1 : 0;
#if !LED_ACTIVE_HIGH
            level = !level;
#endif
            gpio_set_level(LED_GPIO, level);
            vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
        }
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
