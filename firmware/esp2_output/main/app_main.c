#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
#define CLAW_CLAMP_DEG 0
#define CLAW_RELEASE_DEG 80
#define HEIGHT_COEFFICIENT 0.25

static int base_angle = BASE_INITIAL_DEG;
static int arm_angle = ARM_INITIAL_DEG;
static int pitch_angle = PITCH_INITIAL_DEG;
static int claw_angle = CLAW_INITIAL_DEG;

static void print_state(const char *prefix, int gesture);

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
static void write_all_servos(void) {
    write_servo(LEDC_CHANNEL_0, base_angle);
    write_servo(LEDC_CHANNEL_1, arm_angle);
    write_servo(LEDC_CHANNEL_2, pitch_angle);
    write_servo(LEDC_CHANNEL_3, claw_angle);
}

static int pitch_angle_calculator(int arm){
    double arm_Rad = (double)arm * M_PI / 180.0;
    double calculated_pitch = 180 - (180.0 * acos(HEIGHT_COEFFICIENT - sin(arm_Rad)) / M_PI);
    return clamp_int((int)calculated_pitch, PITCH_MIN_DEG, PITCH_MAX_DEG);
}

static void set_angles_constant_height(int base, int arm, int claw) {
    base_angle = clamp_int(base, 0, 180);
    arm_angle = clamp_int(arm, ARM_MIN_DEG, ARM_MAX_DEG);
    pitch_angle = pitch_angle_calculator(arm_angle);
    claw_angle = clamp_int(claw, CLAW_MIN_DEG, CLAW_MAX_DEG);
    write_all_servos();
}

static void set_all_angles(int base, int arm, int pitch, int claw) {
    base_angle = clamp_int(base, 0, 180);
    arm_angle = clamp_int(arm, ARM_MIN_DEG, ARM_MAX_DEG);
    pitch_angle = clamp_int(pitch, PITCH_MIN_DEG, PITCH_MAX_DEG);
    claw_angle = clamp_int(claw, CLAW_MIN_DEG, CLAW_MAX_DEG);
    write_all_servos();
}

static void servo_sweep_test(void) {
    printf("TEST_BEGIN,servo_sweep\n");
    fflush(stdout);

    set_all_angles(90, 90, 90, 30);
    print_state("TEST_STEP_CENTER", 4);
    vTaskDelay(pdMS_TO_TICKS(700));

    set_all_angles(60, 60, 45, 0);
    print_state("TEST_STEP_LOW", 4);
    vTaskDelay(pdMS_TO_TICKS(900));

    set_all_angles(120, 140, 120, 80);
    print_state("TEST_STEP_HIGH", 4);
    vTaskDelay(pdMS_TO_TICKS(900));

    set_all_angles(90, 90, 90, 30);
    print_state("TEST_STEP_CENTER", 4);
    printf("TEST_DONE,servo_sweep\n");
    fflush(stdout);
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

static const char *action_name(int action) {
    switch (action) {
        case 0: return "up";
        case 1: return "down";
        case 2: return "left";
        case 3: return "right";
        case 4: return "clamp";
        case 5: return "release";
        case 6: return "none";
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

static void print_action_state(const char *prefix, int action) {
    printf("%s,action=%d,name=%s,base=%d,arm=%d,pitch=%d,claw=%d\n",
           prefix,
           action,
           action_name(action),
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

static int parse_action(const char *command) {
    char buf[96];
    strlcpy(buf, command, sizeof(buf));
    trim_ascii(buf);
    to_lower_ascii(buf);

    if (strncmp(buf, "action,", 7) == 0) {
        memmove(buf, buf + 7, strlen(buf + 7) + 1);
        trim_ascii(buf);
    }

    if (strcmp(buf, "0") == 0 || strcmp(buf, "up") == 0) return 0;
    if (strcmp(buf, "1") == 0 || strcmp(buf, "down") == 0) return 1;
    if (strcmp(buf, "2") == 0 || strcmp(buf, "left") == 0) return 2;
    if (strcmp(buf, "3") == 0 || strcmp(buf, "right") == 0) return 3;
    if (strcmp(buf, "4") == 0 || strcmp(buf, "clamp") == 0 || strcmp(buf, "close") == 0 || strcmp(buf, "grab") == 0) return 4;
    if (strcmp(buf, "5") == 0 || strcmp(buf, "release") == 0 || strcmp(buf, "open") == 0) return 5;
    if (strcmp(buf, "6") == 0 || strcmp(buf, "none") == 0 || strcmp(buf, "null") == 0) return 6;
    return -1;
}

static void apply_gesture(int gesture) {
    switch (gesture) {
        case 0:
            arm_angle = clamp_int(arm_angle + STEP_DEG, ARM_MIN_DEG, ARM_MAX_DEG);
            pitch_angle = pitch_angle_calculator(arm_angle);
            write_servo(LEDC_CHANNEL_1, arm_angle);
            write_servo(LEDC_CHANNEL_2, pitch_angle);
            break;
        case 1:
            arm_angle = clamp_int(arm_angle - STEP_DEG, ARM_MIN_DEG, ARM_MAX_DEG);
            pitch_angle = pitch_angle_calculator(arm_angle);
            write_servo(LEDC_CHANNEL_1, arm_angle);
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

static void apply_action(int action) {
    switch (action) {
        case 0:
            pitch_angle = clamp_int(pitch_angle + STEP_DEG, PITCH_MIN_DEG, PITCH_MAX_DEG);
            write_servo(LEDC_CHANNEL_2, pitch_angle);
            break;
        case 1:
            pitch_angle = clamp_int(pitch_angle - STEP_DEG, PITCH_MIN_DEG, PITCH_MAX_DEG);
            write_servo(LEDC_CHANNEL_2, pitch_angle);
            break;
        case 2:
            base_angle = clamp_int(base_angle - STEP_DEG, 0, 180);
            write_servo(LEDC_CHANNEL_0, base_angle);
            break;
        case 3:
            base_angle = clamp_int(base_angle + STEP_DEG, 0, 180);
            write_servo(LEDC_CHANNEL_0, base_angle);
            break;
        case 4:
            claw_angle = clamp_int(CLAW_CLAMP_DEG, CLAW_MIN_DEG, CLAW_MAX_DEG);
            write_servo(LEDC_CHANNEL_3, claw_angle);
            break;
        case 5:
            claw_angle = clamp_int(CLAW_RELEASE_DEG, CLAW_MIN_DEG, CLAW_MAX_DEG);
            write_servo(LEDC_CHANNEL_3, claw_angle);
            break;
        case 6:
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

    char command_lower[128];
    strlcpy(command_lower, line, sizeof(command_lower));
    to_lower_ascii(command_lower);
    if (strcmp(command_lower, "test") == 0 || strcmp(command_lower, "sweep") == 0) {
        servo_sweep_test();
        print_state("OK_TEST", 4);
        return;
    }

    if (strncmp(command_lower, "action,", 7) == 0) {
        const int action = parse_action(line);
        if (action >= 0 && action <= 6) {
            apply_action(action);
            print_action_state("OK_ACTION", action);
            return;
        }
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




