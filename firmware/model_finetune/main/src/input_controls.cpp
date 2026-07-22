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
static volatile uint8_t g_prev_ab = 0;
static volatile int8_t g_quad_accum = 0;
static volatile int32_t g_encoder_position = 0;
static volatile uint32_t g_encoder_button_presses = 0;
static volatile uint32_t g_button2_presses = 0;
static volatile TickType_t g_last_encoder_button_tick = 0;
static int g_button2_idle_level = 1;
static int g_button2_last_level = 1;
static TickType_t g_button2_last_change_tick = 0;
static bool g_button2_pressed_latched = false;
static volatile TickType_t g_last_detent_tick = 0;
static bool g_initialized = false;

// Quadrature transition table indexed by (prev_ab << 2) | cur_ab.
// Valid single-bit transitions return +/-1; illegal/bounce transitions return 0.
static const int8_t kQuadratureTable[16] = {
    0,  +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
    0,  -1, +1,  0,
};

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

static uint8_t read_ab_state() {
    const int clk = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_CLK_GPIO));
    const int dt = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_DT_GPIO));
    return static_cast<uint8_t>(((clk & 1) << 1) | (dt & 1));
}

static void IRAM_ATTR encoder_rotate_isr(void *arg) {
    (void)arg;
    const uint8_t cur_ab = read_ab_state();

    portENTER_CRITICAL_ISR(&g_input_mux);
    const uint8_t index = static_cast<uint8_t>((g_prev_ab << 2) | cur_ab);
    const int8_t step = kQuadratureTable[index];
    g_prev_ab = cur_ab;

    if (step != 0) {
        g_quad_accum = static_cast<int8_t>(g_quad_accum + step);

        int8_t detent = 0;
        if (g_quad_accum >= INPUT_ENCODER_STEPS_PER_DETENT) {
            detent = 1;
            g_quad_accum = 0;
        } else if (g_quad_accum <= -INPUT_ENCODER_STEPS_PER_DETENT) {
            detent = -1;
            g_quad_accum = 0;
        }

        if (detent != 0) {
            const TickType_t now = xTaskGetTickCountFromISR();
            const TickType_t delay_ticks = pdMS_TO_TICKS(INPUT_ENCODER_DETENT_DEBOUNCE_MS);
            if ((now - g_last_detent_tick) > delay_ticks) {
                g_encoder_position = g_encoder_position + detent;
                g_last_detent_tick = now;
            }
        }
    }
    portEXIT_CRITICAL_ISR(&g_input_mux);
}

static void IRAM_ATTR encoder_button_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&g_input_mux);
    if (debounce_from_isr(&g_last_encoder_button_tick)) {
        g_encoder_button_presses = g_encoder_button_presses + 1;
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

    // Interrupt on both phases so direction uses a full quadrature state machine.
    esp_err_t err = configure_input_gpio(INPUT_ENCODER_CLK_GPIO, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        return err;
    }
    err = configure_input_gpio(INPUT_ENCODER_DT_GPIO, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        return err;
    }
    err = configure_input_gpio(INPUT_ENCODER_BUTTON_GPIO, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        return err;
    }
    err = configure_input_gpio(INPUT_BUTTON2_GPIO, GPIO_INTR_DISABLE);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    g_prev_ab = read_ab_state();
    if (is_valid_gpio(INPUT_BUTTON2_GPIO)) {
        g_button2_last_level = gpio_get_level(static_cast<gpio_num_t>(INPUT_BUTTON2_GPIO));
    }
    g_button2_last_change_tick = xTaskGetTickCount();
    g_button2_idle_level = g_button2_last_level;
    g_button2_pressed_latched = false;

    err = gpio_isr_handler_add(static_cast<gpio_num_t>(INPUT_ENCODER_CLK_GPIO), encoder_rotate_isr, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_isr_handler_add(static_cast<gpio_num_t>(INPUT_ENCODER_DT_GPIO), encoder_rotate_isr, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_isr_handler_add(static_cast<gpio_num_t>(INPUT_ENCODER_BUTTON_GPIO), encoder_button_isr, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    g_initialized = true;
    ESP_LOGI(TAG,
             "Input controls initialized: encoder CLK=%d DT=%d SW=%d, button2=%d, steps/detent=%d debounce=%dms",
             INPUT_ENCODER_CLK_GPIO,
             INPUT_ENCODER_DT_GPIO,
             INPUT_ENCODER_BUTTON_GPIO,
             INPUT_BUTTON2_GPIO,
             INPUT_ENCODER_STEPS_PER_DETENT,
             INPUT_ENCODER_DETENT_DEBOUNCE_MS);
    return ESP_OK;
}

InputControlsSnapshot input_controls_get_snapshot() {
    InputControlsSnapshot snapshot = {};
    portENTER_CRITICAL(&g_input_mux);
    snapshot.encoder_position = g_encoder_position;
    snapshot.encoder_button_presses = g_encoder_button_presses;
    snapshot.button2_presses = g_button2_presses;
    portEXIT_CRITICAL(&g_input_mux);

    snapshot.encoder_clk_level = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_CLK_GPIO));
    snapshot.encoder_dt_level = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_DT_GPIO));
    snapshot.encoder_button_level = gpio_get_level(static_cast<gpio_num_t>(INPUT_ENCODER_BUTTON_GPIO));
    snapshot.button2_level = is_valid_gpio(INPUT_BUTTON2_GPIO)
                                 ? gpio_get_level(static_cast<gpio_num_t>(INPUT_BUTTON2_GPIO))
                                 : -1;

    if (is_valid_gpio(INPUT_BUTTON2_GPIO)) {
        const TickType_t now = xTaskGetTickCount();
        if (snapshot.button2_level != g_button2_last_level) {
            g_button2_last_level = snapshot.button2_level;
            g_button2_last_change_tick = now;
        } else if ((now - g_button2_last_change_tick) > pdMS_TO_TICKS(INPUT_DEBOUNCE_MS)) {
            const bool active = snapshot.button2_level != g_button2_idle_level;
            if (active && !g_button2_pressed_latched) {
                portENTER_CRITICAL(&g_input_mux);
                g_button2_presses = g_button2_presses + 1;
                snapshot.button2_presses = g_button2_presses;
                portEXIT_CRITICAL(&g_input_mux);
                g_button2_pressed_latched = true;
            } else if (!active) {
                g_button2_pressed_latched = false;
            }
        }
    }
    return snapshot;
}
