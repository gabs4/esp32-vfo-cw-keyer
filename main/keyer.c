/*
 * keyer.c - Iambic keyer state machine.
 *
 * Dit length (ms) = 1200 / WPM (PARIS timing). A dah is 3 dits; the inter-
 * element gap is 1 dit. Paddle memory gives iambic behaviour; mode B adds one
 * extra alternating element after both paddles release.
 */
#include "keyer.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

typedef enum { EL_NONE, EL_DIT, EL_DAH } element_t;

static keyer_cfg_t s_cfg;
static volatile uint16_t s_dit_ms = 60;   /* 20 WPM default */
static volatile bool s_keying;

void keyer_set_wpm(uint16_t wpm)
{
    if (wpm < 5)  wpm = 5;
    if (wpm > 60) wpm = 60;
    s_cfg.wpm = wpm;
    s_dit_ms = 1200 / wpm;
}

void keyer_set_mode(keyer_mode_t mode) { s_cfg.mode = mode; }

bool keyer_is_keying(void) { return s_keying; }

/* Hold the key line for `ms`, but keep sampling paddles for iambic memory. */
static void key_for(uint16_t ms, bool *dit_mem, bool *dah_mem)
{
    s_keying = true;
    if (s_cfg.on_key) s_cfg.on_key(true);

    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        if (input_dit()) *dit_mem = true;
        if (input_dah()) *dah_mem = true;
        vTaskDelay(1);
    }

    s_keying = false;
    if (s_cfg.on_key) s_cfg.on_key(false);

    /* Inter-element space (1 dit), also sampled for memory. */
    end = esp_timer_get_time() + (int64_t)s_dit_ms * 1000;
    while (esp_timer_get_time() < end) {
        if (input_dit()) *dit_mem = true;
        if (input_dah()) *dah_mem = true;
        vTaskDelay(1);
    }
}

static void keyer_task(void *arg)
{
    element_t last = EL_NONE;

    for (;;) {
        /* Straight key: either lever keys the line directly, no CW timing. */
        if (s_cfg.mode == KEYER_STRAIGHT) {
            bool down = input_dit() || input_dah();
            if (down != s_keying) {
                s_keying = down;
                if (s_cfg.on_key) s_cfg.on_key(down);
            }
            vTaskDelay(1);
            continue;
        }

        bool dit = input_dit();
        bool dah = input_dah();
        bool dit_mem = false, dah_mem = false;

        element_t next = EL_NONE;
        if (dit && dah) {
            /* Both down: alternate, opposite of the last element sent. */
            next = (last == EL_DIT) ? EL_DAH : EL_DIT;
        } else if (dit) {
            next = EL_DIT;
        } else if (dah) {
            next = EL_DAH;
        }

        if (next == EL_DIT) {
            key_for(s_dit_ms, &dit_mem, &dah_mem);
            last = EL_DIT;
        } else if (next == EL_DAH) {
            key_for(s_dit_ms * 3, &dit_mem, &dah_mem);
            last = EL_DAH;
        } else {
            last = EL_NONE;
            vTaskDelay(1);
            continue;
        }

        /* Mode B: if both paddles were released during the element, squeeze in
         * one more opposite element. Mode A stops immediately. */
        if (s_cfg.mode == KEYER_IAMBIC_B && !input_dit() && !input_dah() &&
            dit_mem && dah_mem) {
            if (last == EL_DIT) { key_for(s_dit_ms * 3, &dit_mem, &dah_mem); last = EL_DAH; }
            else                { key_for(s_dit_ms,     &dit_mem, &dah_mem); last = EL_DIT; }
        }
    }
}

void keyer_start(const keyer_cfg_t *cfg)
{
    s_cfg = *cfg;
    keyer_set_wpm(cfg->wpm ? cfg->wpm : 20);
    xTaskCreatePinnedToCore(keyer_task, "keyer", 3072, NULL,
                            configMAX_PRIORITIES - 3, NULL, 1);
}
