#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

enum class OutputGestureAction : int {
    kUp = 0,
    kDown = 1,
    kRight = 2,
    kLeft = 3,
    kNull = 4,
};

esp_err_t output_controls_init();
QueueHandle_t output_controls_get_queue();
void output_controls_task(void *pvParameters);
void output_controls_enqueue(OutputGestureAction action);
