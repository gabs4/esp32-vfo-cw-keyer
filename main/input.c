/*
 * input.c - Encoder on the PCNT peripheral (hardware quadrature decode) plus
 * GPIO buttons/paddle with a simple periodic-poll debouncer.
 */
#include "input.h"
#include "bsp_pins.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define ENC_GLITCH_NS   1000
#define ENC_HIGH_LIMIT  1000
#define ENC_LOW_LIMIT  -1000
#define ENC_COUNTS_PER_DETENT 2     /* this encoder emits 2 counts/detent */

static pcnt_unit_handle_t s_pcnt;
static int s_enc_accum;

/* Debounce state per button. */
static const gpio_num_t s_btn_pin[BTN_COUNT] = {
    [BTN_BAND] = PIN_BTN_BAND,
    [BTN_STEP] = PIN_BTN_STEP,
    [BTN_MODE] = PIN_BTN_MODE,
    [BTN_ENC]  = PIN_ENC_PUSH,
};
static bool     s_btn_stable[BTN_COUNT];      /* debounced logical state (true=pressed) */
static bool     s_btn_last_raw[BTN_COUNT];
static int64_t  s_btn_change_us[BTN_COUNT];
static bool     s_btn_event[BTN_COUNT];

#define DEBOUNCE_US 15000

static void encoder_init(void)
{
    pcnt_unit_config_t unit_cfg = {
        .high_limit = ENC_HIGH_LIMIT,
        .low_limit  = ENC_LOW_LIMIT,
    };
    pcnt_new_unit(&unit_cfg, &s_pcnt);

    pcnt_glitch_filter_config_t filt = { .max_glitch_ns = ENC_GLITCH_NS };
    pcnt_unit_set_glitch_filter(s_pcnt, &filt);

    /* Channel A: edge on A, control on B -> full x4 quadrature with 2 channels. */
    pcnt_chan_config_t ch_a_cfg = {
        .edge_gpio_num  = PIN_ENC_A,
        .level_gpio_num = PIN_ENC_B,
    };
    pcnt_channel_handle_t ch_a;
    pcnt_new_channel(s_pcnt, &ch_a_cfg, &ch_a);
    pcnt_channel_set_edge_action(ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_chan_config_t ch_b_cfg = {
        .edge_gpio_num  = PIN_ENC_B,
        .level_gpio_num = PIN_ENC_A,
    };
    pcnt_channel_handle_t ch_b;
    pcnt_new_channel(s_pcnt, &ch_b_cfg, &ch_b);
    pcnt_channel_set_edge_action(ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(s_pcnt);
    pcnt_unit_clear_count(s_pcnt);
    pcnt_unit_start(s_pcnt);
}

static void buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) mask |= (1ULL << s_btn_pin[i]);
    mask |= (1ULL << PIN_KEY_DIT) | (1ULL << PIN_KEY_DAH);

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* active-low switches to GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn_stable[i] = false;
        s_btn_last_raw[i] = false;
    }
}

void input_init(void)
{
    encoder_init();
    buttons_init();
}

/* Call frequency is implicit: poll inside the accessors. */
static void buttons_poll(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < BTN_COUNT; i++) {
        bool raw = (gpio_get_level(s_btn_pin[i]) == 0);   /* active-low */
        if (raw != s_btn_last_raw[i]) {
            s_btn_last_raw[i] = raw;
            s_btn_change_us[i] = now;
        } else if ((now - s_btn_change_us[i]) > DEBOUNCE_US &&
                   raw != s_btn_stable[i]) {
            s_btn_stable[i] = raw;
            if (raw) s_btn_event[i] = true;   /* register press edge */
        }
    }
}

int input_encoder_delta(void)
{
    int count = 0;
    pcnt_unit_get_count(s_pcnt, &count);
    s_enc_accum += count;
    pcnt_unit_clear_count(s_pcnt);

    int detents = s_enc_accum / ENC_COUNTS_PER_DETENT;
    s_enc_accum -= detents * ENC_COUNTS_PER_DETENT;
    return detents;
}

bool input_button_pressed(btn_id_t id)
{
    buttons_poll();
    if (s_btn_event[id]) { s_btn_event[id] = false; return true; }
    return false;
}

bool input_dit(void) { return gpio_get_level(PIN_KEY_DIT) == 0; }
bool input_dah(void) { return gpio_get_level(PIN_KEY_DAH) == 0; }
