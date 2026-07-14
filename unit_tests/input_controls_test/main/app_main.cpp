#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "INPUT_UNIT_TEST";

// PCB allocation from the input group / board design.
static constexpr int ENCODER_CLK_GPIO = 21;
static constexpr int ENCODER_DT_GPIO = 47;
static constexpr int ENCODER_SW_GPIO = 48;
static constexpr int BUTTON2_GPIO = -1;
static constexpr int DEBOUNCE_MS = 60;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int g_prev_clk = 0;
static volatile int32_t g_encoder = 0;
static volatile uint32_t g_encoder_button = 0;
static volatile uint32_t g_button2 = 0;
static volatile TickType_t g_last_sw_tick = 0;
static volatile TickType_t g_last_button2_tick = 0;

static bool valid_gpio(int gpio) {
    return gpio >= 0;
}

static bool debounce_from_isr(volatile TickType_t *last_tick) {
    const TickType_t now = xTaskGetTickCountFromISR();
    const TickType_t delay = pdMS_TO_TICKS(DEBOUNCE_MS);
    if ((now - *last_tick) <= delay) {
        return false;
    }
    *last_tick = now;
    return true;
}

static void IRAM_ATTR encoder_isr(void *) {
    const int clk = gpio_get_level((gpio_num_t)ENCODER_CLK_GPIO);
    const int dt = gpio_get_level((gpio_num_t)ENCODER_DT_GPIO);
    portENTER_CRITICAL_ISR(&g_mux);
    if (clk != g_prev_clk) {
        g_encoder += (clk == dt) ? -1 : 1;
        g_prev_clk = clk;
    }
    portEXIT_CRITICAL_ISR(&g_mux);
}

static void IRAM_ATTR encoder_button_isr(void *) {
    portENTER_CRITICAL_ISR(&g_mux);
    if (debounce_from_isr(&g_last_sw_tick)) {
        g_encoder_button++;
    }
    portEXIT_CRITICAL_ISR(&g_mux);
}

static void IRAM_ATTR button2_isr(void *) {
    portENTER_CRITICAL_ISR(&g_mux);
    if (debounce_from_isr(&g_last_button2_tick)) {
        g_button2++;
    }
    portEXIT_CRITICAL_ISR(&g_mux);
}

static esp_err_t configure_input(int gpio, gpio_int_type_t intr) {
    if (!valid_gpio(gpio)) {
        return ESP_OK;
    }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = intr;
    return gpio_config(&cfg);
}

extern "C" void app_main(void) {
    ESP_ERROR_CHECK(configure_input(ENCODER_CLK_GPIO, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(configure_input(ENCODER_DT_GPIO, GPIO_INTR_DISABLE));
    ESP_ERROR_CHECK(configure_input(ENCODER_SW_GPIO, GPIO_INTR_NEGEDGE));
    ESP_ERROR_CHECK(configure_input(BUTTON2_GPIO, GPIO_INTR_NEGEDGE));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    g_prev_clk = gpio_get_level((gpio_num_t)ENCODER_CLK_GPIO);
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)ENCODER_CLK_GPIO, encoder_isr, nullptr));
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)ENCODER_SW_GPIO, encoder_button_isr, nullptr));
    if (valid_gpio(BUTTON2_GPIO)) {
        ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)BUTTON2_GPIO, button2_isr, nullptr));
    }

    ESP_LOGI(TAG, "READY,INPUT_CONTROLS_TEST clk=%d dt=%d sw=%d button2=%d", ENCODER_CLK_GPIO, ENCODER_DT_GPIO, ENCODER_SW_GPIO, BUTTON2_GPIO);
    while (true) {
        int32_t pos;
        uint32_t sw;
        uint32_t b2;
        portENTER_CRITICAL(&g_mux);
        pos = g_encoder;
        sw = g_encoder_button;
        b2 = g_button2;
        portEXIT_CRITICAL(&g_mux);
        printf("INPUT_STATUS,encoder=%ld,encoder_button=%lu,button2=%lu,clk=%d,dt=%d,sw_level=%d\n",
               (long)pos,
               (unsigned long)sw,
               (unsigned long)b2,
               gpio_get_level((gpio_num_t)ENCODER_CLK_GPIO),
               gpio_get_level((gpio_num_t)ENCODER_DT_GPIO),
               gpio_get_level((gpio_num_t)ENCODER_SW_GPIO));
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
