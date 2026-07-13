#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ESP2_OUTPUT";

// Keep these pins consistent with the original output team's robotic_arm.ino.
#define BASE_SERVO_GPIO 18
#define ARM_SERVO_GPIO 19
#define PITCH_SERVO_GPIO 22
#define CLAW_SERVO_GPIO 21

#define SERVO_FREQ_HZ 50
#define SERVO_DUTY_BITS LEDC_TIMER_16_BIT
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500
#define STEP_DEG 5

#define BASE_INITIAL_DEG 90
#define ARM_INITIAL_DEG 90
#define PITCH_INITIAL_DEG 90
#define CLAW_INITIAL_DEG 30
#define ARM_MIN_DEG 45
#define ARM_MAX_DEG 180
#define PITCH_MIN_DEG 30
#define PITCH_MAX_DEG 125
#define CLAW_MIN_DEG 0
#define CLAW_MAX_DEG 90

static int base_angle = BASE_INITIAL_DEG;
static int arm_angle = ARM_INITIAL_DEG;
static int pitch_angle = PITCH_INITIAL_DEG;
static int claw_angle = CLAW_INITIAL_DEG;

static int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static uint32_t angle_to_duty(int angle) {
    const int pulse_us = SERVO_MIN_US + ((SERVO_MAX_US - SERVO_MIN_US) * angle) / 180;
    return (uint32_t)((pulse_us * ((1 << 16) - 1)) / 20000);
}

static void write_servo(ledc_channel_t channel, int angle) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, angle_to_duty(angle));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static esp_err_t configure_channel(ledc_channel_t channel, int gpio, int angle) {
    ledc_channel_config_t config = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = angle_to_duty(angle),
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    return ledc_channel_config(&config);
}

static const char *gesture_name(int gesture) {
    switch (gesture) {
        case 0: return "up";
        case 1: return "down";
        case 2: return "right";
        case 3: return "left";
        case 4: return "null";
        default: return "unknown";
    }
}

static void print_state(const char *prefix, int gesture) {
    printf("%s,gesture=%d,name=%s,base=%d,arm=%d,pitch=%d,claw=%d\n",
           prefix,
           gesture,
           gesture_name(gesture),
           base_angle,
           arm_angle,
           pitch_angle,
           claw_angle);
    fflush(stdout);
}

static void to_lower_ascii(char *text) {
    for (; *text; ++text) {
        *text = (char)tolower((unsigned char)*text);
    }
}

static void trim_ascii(char *text) {
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
}

static int parse_gesture(const char *command) {
    char buf[96];
    strlcpy(buf, command, sizeof(buf));
    trim_ascii(buf);
    to_lower_ascii(buf);

    if (strncmp(buf, "gesture,", 8) == 0) {
        return atoi(buf + 8);
    }
    if (strcmp(buf, "0") == 0 || strcmp(buf, "up") == 0) return 0;
    if (strcmp(buf, "1") == 0 || strcmp(buf, "down") == 0) return 1;
    if (strcmp(buf, "2") == 0 || strcmp(buf, "right") == 0) return 2;
    if (strcmp(buf, "3") == 0 || strcmp(buf, "left") == 0) return 3;
    if (strcmp(buf, "4") == 0 || strcmp(buf, "null") == 0 || strcmp(buf, "none") == 0) return 4;
    return -1;
}

static void apply_gesture(int gesture) {
    switch (gesture) {
        case 0:
            pitch_angle = clamp_int(pitch_angle + STEP_DEG, PITCH_MIN_DEG, PITCH_MAX_DEG);
            write_servo(LEDC_CHANNEL_2, pitch_angle);
            break;
        case 1:
            pitch_angle = clamp_int(pitch_angle - STEP_DEG, PITCH_MIN_DEG, PITCH_MAX_DEG);
            write_servo(LEDC_CHANNEL_2, pitch_angle);
            break;
        case 2:
            base_angle = clamp_int(base_angle + STEP_DEG, 0, 180);
            write_servo(LEDC_CHANNEL_0, base_angle);
            break;
        case 3:
            base_angle = clamp_int(base_angle - STEP_DEG, 0, 180);
            write_servo(LEDC_CHANNEL_0, base_angle);
            break;
        case 4:
        default:
            break;
    }
}

static bool apply_manual_servo_command(const char *command) {
    if (command == NULL || command[0] == '\0' || command[1] == '\0') {
        return false;
    }
    const char axis = command[0];
    const int value = atoi(command + 1);
    switch (axis) {
        case 'B': case 'b':
            base_angle = clamp_int(value, 0, 180);
            write_servo(LEDC_CHANNEL_0, base_angle);
            return true;
        case 'A': case 'a':
            arm_angle = clamp_int(value, ARM_MIN_DEG, ARM_MAX_DEG);
            write_servo(LEDC_CHANNEL_1, arm_angle);
            return true;
        case 'P': case 'p':
            pitch_angle = clamp_int(value, PITCH_MIN_DEG, PITCH_MAX_DEG);
            write_servo(LEDC_CHANNEL_2, pitch_angle);
            return true;
        case 'C': case 'c':
            claw_angle = clamp_int(value, CLAW_MIN_DEG, CLAW_MAX_DEG);
            write_servo(LEDC_CHANNEL_3, claw_angle);
            return true;
        default:
            return false;
    }
}

static void handle_command(char *line) {
    trim_ascii(line);
    if (line[0] == '\0') {
        return;
    }

    const int gesture = parse_gesture(line);
    if (gesture >= 0 && gesture <= 4) {
        apply_gesture(gesture);
        print_state("OK", gesture);
        return;
    }

    if (apply_manual_servo_command(line)) {
        print_state("OK_MANUAL", 4);
        return;
    }

    printf("ERR,unknown_command,%s\n", line);
    fflush(stdout);
}

static void servo_output_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_DUTY_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(configure_channel(LEDC_CHANNEL_0, BASE_SERVO_GPIO, base_angle));
    ESP_ERROR_CHECK(configure_channel(LEDC_CHANNEL_1, ARM_SERVO_GPIO, arm_angle));
    ESP_ERROR_CHECK(configure_channel(LEDC_CHANNEL_2, PITCH_SERVO_GPIO, pitch_angle));
    ESP_ERROR_CHECK(configure_channel(LEDC_CHANNEL_3, CLAW_SERVO_GPIO, claw_angle));

    ESP_LOGI(TAG,
             "Servo output initialized: base=%d arm=%d pitch=%d claw=%d",
             BASE_SERVO_GPIO,
             ARM_SERVO_GPIO,
             PITCH_SERVO_GPIO,
             CLAW_SERVO_GPIO);
}

void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    servo_output_init();

    printf("READY,ESP2_SERVO_OUTPUT\n");
    print_state("STATE", 4);

    char line[128];
    while (true) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}


