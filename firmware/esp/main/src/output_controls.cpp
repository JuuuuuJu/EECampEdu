#include "output_controls.hpp"

#include "driver/ledc.h"
#include "esp_log.h"
#include "model_config.hpp"

static const char *TAG = "OUTPUT_CONTROLS";

static QueueHandle_t g_output_queue = nullptr;
static int g_base_angle = ROBOT_ARM_BASE_INITIAL_DEG;
static int g_arm_angle = ROBOT_ARM_ARM_INITIAL_DEG;
static int g_pitch_angle = ROBOT_ARM_PITCH_INITIAL_DEG;
static int g_claw_angle = ROBOT_ARM_CLAW_INITIAL_DEG;

static int clamp_angle(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint32_t angle_to_duty(int angle) {
    const int pulse_us = ROBOT_ARM_SERVO_MIN_US +
                         ((ROBOT_ARM_SERVO_MAX_US - ROBOT_ARM_SERVO_MIN_US) * angle) / 180;
    return (uint32_t)((pulse_us * ((1 << ROBOT_ARM_SERVO_DUTY_BITS) - 1)) / 20000);
}

static void write_servo(ledc_channel_t channel, int angle) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, angle_to_duty(angle));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static esp_err_t configure_servo_channel(ledc_channel_t channel, int gpio, int angle) {
    ledc_channel_config_t config = {};
    config.gpio_num = gpio;
    config.speed_mode = LEDC_LOW_SPEED_MODE;
    config.channel = channel;
    config.intr_type = LEDC_INTR_DISABLE;
    config.timer_sel = LEDC_TIMER_1;
    config.duty = angle_to_duty(angle);
    config.hpoint = 0;
    return ledc_channel_config(&config);
}

esp_err_t output_controls_init() {
    if (g_output_queue == nullptr) {
        g_output_queue = xQueueCreate(8, sizeof(OutputGestureAction));
        if (g_output_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_1;
    timer.duty_resolution = (ledc_timer_bit_t)ROBOT_ARM_SERVO_DUTY_BITS;
    timer.freq_hz = ROBOT_ARM_SERVO_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_timer_config(&timer));

    ESP_ERROR_CHECK_WITHOUT_ABORT(configure_servo_channel(LEDC_CHANNEL_0, ROBOT_ARM_BASE_GPIO, g_base_angle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(configure_servo_channel(LEDC_CHANNEL_1, ROBOT_ARM_ARM_GPIO, g_arm_angle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(configure_servo_channel(LEDC_CHANNEL_2, ROBOT_ARM_PITCH_GPIO, g_pitch_angle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(configure_servo_channel(LEDC_CHANNEL_3, ROBOT_ARM_CLAW_GPIO, g_claw_angle));

    ESP_LOGI(TAG,
             "Robot arm output initialized. GPIO base=%d arm=%d pitch=%d claw=%d",
             ROBOT_ARM_BASE_GPIO,
             ROBOT_ARM_ARM_GPIO,
             ROBOT_ARM_PITCH_GPIO,
             ROBOT_ARM_CLAW_GPIO);
    return ESP_OK;
}

QueueHandle_t output_controls_get_queue() {
    return g_output_queue;
}

void output_controls_enqueue(OutputGestureAction action) {
    if (!ENABLE_ROBOT_ARM_OUTPUT || g_output_queue == nullptr) {
        return;
    }
    xQueueSend(g_output_queue, &action, 0);
}

void output_controls_task(void *pvParameters) {
    (void)pvParameters;
    esp_err_t err = output_controls_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Output controls init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    OutputGestureAction action = OutputGestureAction::kNull;
    while (true) {
        if (xQueueReceive(g_output_queue, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (action) {
            case OutputGestureAction::kUp:
                g_pitch_angle = clamp_angle(g_pitch_angle + ROBOT_ARM_STEP_DEG,
                                            ROBOT_ARM_PITCH_MIN_DEG,
                                            ROBOT_ARM_PITCH_MAX_DEG);
                write_servo(LEDC_CHANNEL_2, g_pitch_angle);
                break;
            case OutputGestureAction::kDown:
                g_pitch_angle = clamp_angle(g_pitch_angle - ROBOT_ARM_STEP_DEG,
                                            ROBOT_ARM_PITCH_MIN_DEG,
                                            ROBOT_ARM_PITCH_MAX_DEG);
                write_servo(LEDC_CHANNEL_2, g_pitch_angle);
                break;
            case OutputGestureAction::kRight:
                g_base_angle = clamp_angle(g_base_angle + ROBOT_ARM_STEP_DEG, 0, 180);
                write_servo(LEDC_CHANNEL_0, g_base_angle);
                break;
            case OutputGestureAction::kLeft:
                g_base_angle = clamp_angle(g_base_angle - ROBOT_ARM_STEP_DEG, 0, 180);
                write_servo(LEDC_CHANNEL_0, g_base_angle);
                break;
            case OutputGestureAction::kNull:
                break;
        }

        ESP_LOGI(TAG,
                 "OUTPUT_ARM,action=%d,base=%d,arm=%d,pitch=%d,claw=%d",
                 (int)action,
                 g_base_angle,
                 g_arm_angle,
                 g_pitch_angle,
                 g_claw_angle);
    }
}
