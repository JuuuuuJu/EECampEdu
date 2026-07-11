#include "input_controls.hpp"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "model_config.hpp"

static const char *TAG = "INPUT_CONTROLS";

static portMUX_TYPE g_input_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int g_prev_clk = 0;
static volatile int32_t g_encoder_position = 0;
static volatile uint32_t g_encoder_button_presses = 0;
static volatile uint32_t g_button2_presses = 0;
static volatile TickType_t g_last_encoder_button_tick = 0;
static volatile TickType_t g_last_button2_tick = 0;
static bool g_initialized = false;

static bool is_valid_gpio(int gpio) {
    return gpio >= 0;
}

static bool debounce_from_isr(volatile TickType_t *last_tick) {
    const TickType_t now = xTaskGetTickCountFromISR();
    const TickType_t delay_ticks = pdMS_TO_TICKS(INPUT_DEBOUNCE_MS);
    if ((now - *last_tick) <= delay_ticks) {
        return false;
    }
    *last_tick = now;
    return true;
}

static void IRAM_ATTR encoder_rotate_isr(void *arg) {
    (void)arg;
    const int cur_clk = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_CLK_GPIO));
    const int cur_dt = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_DT_GPIO));

    portENTER_CRITICAL_ISR(&g_input_mux);
    if (g_prev_clk != cur_clk) {
        if (cur_clk == cur_dt) {
            --g_encoder_position;
        } else {
            ++g_encoder_position;
        }
        g_prev_clk = cur_clk;
    }
    portEXIT_CRITICAL_ISR(&g_input_mux);
}

static void IRAM_ATTR encoder_button_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&g_input_mux);
    if (debounce_from_isr(&g_last_encoder_button_tick)) {
        ++g_encoder_button_presses;
    }
    portEXIT_CRITICAL_ISR(&g_input_mux);
}

static void IRAM_ATTR button2_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&g_input_mux);
    if (debounce_from_isr(&g_last_button2_tick)) {
        ++g_button2_presses;
    }
    portEXIT_CRITICAL_ISR(&g_input_mux);
}

static esp_err_t configure_input_gpio(int gpio, gpio_int_type_t interrupt_type) {
    if (!is_valid_gpio(gpio)) {
        return ESP_OK;
    }
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << gpio;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = interrupt_type;
    return gpio_config(&config);
}

esp_err_t input_controls_init() {
    if (g_initialized) {
        return ESP_OK;
    }

    esp_err_t err = configure_input_gpio(INPUT_ENCODER_CLK_GPIO, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        return err;
    }
    err = configure_input_gpio(INPUT_ENCODER_DT_GPIO, GPIO_INTR_DISABLE);
    if (err != ESP_OK) {
        return err;
    }
    err = configure_input_gpio(INPUT_ENCODER_BUTTON_GPIO, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        return err;
    }
    err = configure_input_gpio(INPUT_BUTTON2_GPIO, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    g_prev_clk = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_CLK_GPIO));

    err = gpio_isr_handler_add(static_cast<gpio_num_t>(INPUT_ENCODER_CLK_GPIO), encoder_rotate_isr, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_isr_handler_add(static_cast<gpio_num_t>(INPUT_ENCODER_BUTTON_GPIO), encoder_button_isr, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    if (is_valid_gpio(INPUT_BUTTON2_GPIO)) {
        err = gpio_isr_handler_add(static_cast<gpio_num_t>(INPUT_BUTTON2_GPIO), button2_isr, nullptr);
        if (err != ESP_OK) {
            return err;
        }
    }

    g_initialized = true;
    ESP_LOGI(TAG,
             "Input controls initialized: encoder CLK=%d DT=%d SW=%d, button2=%d",
             INPUT_ENCODER_CLK_GPIO,
             INPUT_ENCODER_DT_GPIO,
             INPUT_ENCODER_BUTTON_GPIO,
             INPUT_BUTTON2_GPIO);
    return ESP_OK;
}

InputControlsSnapshot input_controls_get_snapshot() {
    InputControlsSnapshot snapshot = {};
    portENTER_CRITICAL(&g_input_mux);
    snapshot.encoder_position = g_encoder_position;
    snapshot.encoder_button_presses = g_encoder_button_presses;
    snapshot.button2_presses = g_button2_presses;
    portEXIT_CRITICAL(&g_input_mux);

    snapshot.encoder_button_level = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_BUTTON_GPIO));
    snapshot.button2_level = is_valid_gpio(INPUT_BUTTON2_GPIO)
                                 ? gpio_get_level(static_cast<gpio_num_t>(INPUT_BUTTON2_GPIO))
                                 : -1;
    return snapshot;
}
