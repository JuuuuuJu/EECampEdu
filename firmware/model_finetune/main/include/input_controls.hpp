#pragma once

#include <stdint.h>

#include "esp_err.h"

struct InputControlsSnapshot {
    int32_t encoder_position;
    uint32_t encoder_button_presses;
    uint32_t button2_presses;
    int encoder_button_level;
    int button2_level;
    int encoder_clk_level;
    int encoder_dt_level;
};

esp_err_t input_controls_init();
InputControlsSnapshot input_controls_get_snapshot();
