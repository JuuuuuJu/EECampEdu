// Output-class teaching demo: drives the on-board addressable RGB LED on GPIO38.
//
// IMPORTANT: GPIO38 on this board is an *addressable* RGB LED (SK6812MINI-HS,
// WS2812-family). It is driven by a timed one-wire (NRZ) protocol at ~800 kHz,
// NOT by gpio_set_level() or LEDC PWM. A static logic level does nothing to it.
// We therefore generate the waveform with the RMT peripheral (core IDF, no
// external component needed, so builds work offline).
//
// Browser/Web Serial commands (unchanged so the portal works as-is):
//   LED,1 | LED,0        turn the LED on (white at current level) / off
//   PWM,<0-255>          brightness of the white on-color (0 = off)
//   RGB,<r>,<g>,<b>      set an explicit color (0-255 each) and turn on
//   BLINK | STOP         start/stop the blink loop
//   TEST                 cycle red -> green -> blue -> white -> off (proves RGB)
//   STATUS               report gpio, on/off, level, color, blink
//   PIN,<gpio>           move the LED data pin at runtime (board debugging)
//   ACTIVE,<1|0>         accepted but not applicable to an addressable LED
//   LEVEL,<1|0>          raw on/off (alias of LED)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DEFAULT_LED_GPIO   GPIO_NUM_38
#define BLINK_PERIOD_MS    500
#define RMT_RESOLUTION_HZ  10000000   // 10 MHz -> 0.1 us per tick

static const char *TAG = "OUTPUT_DEMO";

// ---- RMT one-wire (SK6812/WS2812) driver ----------------------------------
static rmt_channel_handle_t rmt_chan = NULL;
static rmt_encoder_handle_t rmt_bytes_enc = NULL;

// SK6812MINI-HS bit timing at 0.1 us/tick: T0H=0.3us, T0L=0.9us, T1H=0.6us,
// T1L=0.6us. WS2812-compatible; msb_first, GRB byte order.
static const rmt_symbol_word_t SK_BIT0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 };
static const rmt_symbol_word_t SK_BIT1 = { .level0 = 1, .duration0 = 6, .level1 = 0, .duration1 = 6 };

// ---- logical LED state (serialized by g_lock) -----------------------------
static gpio_num_t led_gpio = DEFAULT_LED_GPIO;
static bool led_on = false;
static int  level = 255;                 // brightness 0-255 applied to the on-color
static uint8_t color_r = 255, color_g = 255, color_b = 255;  // white by default
static bool blink_enabled = false;
static bool blink_phase = false;
static SemaphoreHandle_t g_lock = NULL;

static uint8_t scale(uint8_t c) { return (uint8_t)((c * level + 127) / 255); }

// Push one pixel to the LED. SK6812 wants Green, Red, Blue order.
static esp_err_t led_show_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!rmt_chan || !rmt_bytes_enc) return ESP_ERR_INVALID_STATE;
    const uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0, .flags = { .eot_level = 0 } };
    esp_err_t err = rmt_transmit(rmt_chan, rmt_bytes_enc, grb, sizeof(grb), &tx);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(rmt_chan, pdMS_TO_TICKS(100));
    esp_rom_delay_us(120);   // hold the line low >80us so the LED latches the frame
    return err;
}

// Render current logical state (on/off * color * level) to the LED.
static void led_render(void) {
    esp_err_t err;
    if (led_on) err = led_show_rgb(scale(color_r), scale(color_g), scale(color_b));
    else        err = led_show_rgb(0, 0, 0);
    if (err != ESP_OK) printf("ERR,led_show,%s\n", esp_err_to_name(err));
}

static esp_err_t led_driver_init(gpio_num_t gpio) {
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &rmt_chan);
    if (err != ESP_OK) return err;

    if (!rmt_bytes_enc) {
        rmt_bytes_encoder_config_t enc_cfg = {
            .bit0 = SK_BIT0,
            .bit1 = SK_BIT1,
            .flags = { .msb_first = 1 },
        };
        err = rmt_new_bytes_encoder(&enc_cfg, &rmt_bytes_enc);
        if (err != ESP_OK) return err;
    }
    err = rmt_enable(rmt_chan);
    return err;
}

// Rebuild the RMT channel on a different data GPIO (for PIN,<gpio>).
static esp_err_t led_driver_reinit(gpio_num_t gpio) {
    if (rmt_chan) {
        rmt_disable(rmt_chan);
        rmt_del_channel(rmt_chan);
        rmt_chan = NULL;
    }
    return led_driver_init(gpio);
}

// ---- command helpers (assume g_lock held) ---------------------------------
static void print_help(void) {
    printf("Commands: LED,1 | LED,0 | PWM,<0-255> | RGB,<r>,<g>,<b> | BLINK | STOP | TEST | STATUS | PIN,<gpio> | HELP\n");
    fflush(stdout);
}

static void print_status(void) {
    printf("STATUS,gpio=%d,type=addressable_sk6812,on=%d,level=%d,rgb=%d,%d,%d,blink=%d\n",
           (int)led_gpio, led_on ? 1 : 0, level,
           color_r, color_g, color_b, blink_enabled ? 1 : 0);
    fflush(stdout);
}

static void set_led(bool on) {
    blink_enabled = false;
    led_on = on;
    if (on && level == 0) level = 255;   // a bare "on" must be visible
    led_render();
    printf("LED,%s,gpio=%d,rgb=%d,%d,%d\n", on ? "ON" : "OFF",
           (int)led_gpio, on ? scale(color_r) : 0, on ? scale(color_g) : 0, on ? scale(color_b) : 0);
    fflush(stdout);
}

static void set_pwm(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    blink_enabled = false;
    level = value;
    led_on = value > 0;
    led_render();
    printf("PWM,%d,gpio=%d\n", value, (int)led_gpio);
    fflush(stdout);
}

static void set_rgb(int r, int g, int b) {
    color_r = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    color_g = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    color_b = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    blink_enabled = false;
    led_on = true;
    if (level == 0) level = 255;
    led_render();
    printf("RGB,%d,%d,%d,gpio=%d\n", color_r, color_g, color_b, (int)led_gpio);
    fflush(stdout);
}

static void diagnostic_test(void) {
    blink_enabled = false;
    const int saved_level = level; const bool saved_on = led_on;
    const uint8_t sr = color_r, sg = color_g, sb = color_b;
    level = 255;
    printf("TEST,start,gpio=%d\n", (int)led_gpio);
    fflush(stdout);
    const uint8_t seq[5][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}, {0,0,0} };
    const char *names[5] = { "red", "green", "blue", "white", "off" };
    for (int i = 0; i < 5; ++i) {
        led_show_rgb(seq[i][0], seq[i][1], seq[i][2]);
        printf("TEST,%s\n", names[i]);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    // restore prior logical state
    level = saved_level; led_on = saved_on; color_r = sr; color_g = sg; color_b = sb;
    led_render();
    printf("TEST,done\n");
    fflush(stdout);
}

static void set_pin(int gpio) {
    gpio_num_t next = (gpio_num_t)gpio;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(next)) {
        printf("ERR,bad_gpio,%d\n", gpio);
        fflush(stdout);
        return;
    }
    blink_enabled = false;
    esp_err_t err = led_driver_reinit(next);
    if (err != ESP_OK) {
        printf("ERR,rmt_init,%s\n", esp_err_to_name(err));
        fflush(stdout);
        return;
    }
    led_gpio = next;
    led_on = false;
    led_render();                        // start the new pin's LED off
    printf("PIN,%d\n", gpio);
    fflush(stdout);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void handle_command(char *line) {
    rstrip(line);
    char *comma = strchr(line, ',');
    char *args = NULL;
    int value = 0;
    if (comma) {
        args = comma + 1;
        value = atoi(args);
    }
    // Split the verb off (after capturing args) for the simple commands.
    char verb[16] = {0};
    size_t vlen = comma ? (size_t)(comma - line) : strlen(line);
    if (vlen >= sizeof(verb)) vlen = sizeof(verb) - 1;
    memcpy(verb, line, vlen);

    // Serialize with the blink loop so a command and a blink toggle can never
    // interleave two writes to the LED.
    if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY);

    if (strcasecmp(verb, "LED") == 0) {
        set_led(value != 0);
    } else if (strcasecmp(verb, "PWM") == 0) {
        set_pwm(value);
    } else if (strcasecmp(verb, "RGB") == 0) {
        int r = 0, g = 0, b = 0;
        if (args && sscanf(args, "%d,%d,%d", &r, &g, &b) == 3) set_rgb(r, g, b);
        else { printf("ERR,rgb_needs_r,g,b\n"); fflush(stdout); }
    } else if (strcasecmp(verb, "PIN") == 0) {
        set_pin(value);
    } else if (strcasecmp(verb, "ACTIVE") == 0) {
        // Polarity does not apply to an addressable LED; accept for portal compat.
        printf("ACTIVE,ignored_for_addressable_led\n");
        fflush(stdout);
    } else if (strcasecmp(verb, "LEVEL") == 0) {
        set_led(value != 0);
    } else if (strcasecmp(verb, "on") == 0) {
        set_led(true);
    } else if (strcasecmp(verb, "off") == 0) {
        set_led(false);
    } else if (strcasecmp(verb, "toggle") == 0) {
        set_led(!led_on);
    } else if (strcasecmp(verb, "BLINK") == 0) {
        if (level == 0) level = 255;
        blink_enabled = true;
        blink_phase = false;
        printf("LED,BLINK,gpio=%d\n", (int)led_gpio);
        fflush(stdout);
    } else if (strcasecmp(verb, "STOP") == 0) {
        set_led(false);
    } else if (strcasecmp(verb, "TEST") == 0) {
        diagnostic_test();
    } else if (strcasecmp(verb, "STATUS") == 0) {
        print_status();
    } else if (verb[0] == '\0' || strcasecmp(verb, "HELP") == 0) {
        print_help();
    } else {
        printf("ERR,unknown_command,%s\n", verb);
        print_help();
    }

    if (g_lock) xSemaphoreGive(g_lock);
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
    // The console is USB Serial/JTAG. printf() works without the driver, but
    // READING host input (our LED commands) does NOT until the USB-Serial-JTAG
    // VFS driver is installed and stdin is routed through it.
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_cfg));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);   // browser sends "\n"
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    g_lock = xSemaphoreCreateMutex();

    // Deterministic power-on state: addressable LED driver up, LED OFF, white
    // on-color at full level, no blink. Same every boot / replug.
    led_on = false; level = 255; blink_enabled = false;
    color_r = color_g = color_b = 255;
    esp_err_t err = led_driver_init(led_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT LED init failed: %s", esp_err_to_name(err));
    } else {
        led_render();   // ensure the LED starts OFF
    }

    ESP_LOGI(TAG, "Output demo ready on GPIO%d (addressable SK6812 RGB LED).", (int)led_gpio);
    printf("READY,OUTPUT_DEMO,gpio=%d,type=addressable_sk6812\n", (int)led_gpio);
    print_help();

    xTaskCreate(serial_command_task, "serial_command_task", 4096, NULL, 5, NULL);

    // Blink loop: toggles only while blinking, always under the shared lock so
    // it can never race a command's LED write.
    while (true) {
        bool blinking = false;
        if (g_lock && xSemaphoreTake(g_lock, portMAX_DELAY) == pdTRUE) {
            if (blink_enabled) {
                blink_phase = !blink_phase;
                if (blink_phase) led_show_rgb(scale(color_r), scale(color_g), scale(color_b));
                else             led_show_rgb(0, 0, 0);
                blinking = true;
            }
            xSemaphoreGive(g_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(blinking ? BLINK_PERIOD_MS : 50));
    }
}
