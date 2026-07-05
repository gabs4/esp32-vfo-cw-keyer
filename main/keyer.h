/*
 * keyer.h - Iambic (mode A/B) CW keyer driven from the paddle inputs.
 *
 * Runs as its own FreeRTOS task; calls a user callback whenever the key line
 * changes so the app can key the transmitter and/or gate the Si5351 output.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KEYER_IAMBIC_A = 0,
    KEYER_IAMBIC_B = 1,
    KEYER_STRAIGHT = 2,   /* paddle (either lever) directly keys, no timing */
} keyer_mode_t;

typedef void (*keyer_key_cb_t)(bool key_down);

typedef struct {
    uint16_t       wpm;
    keyer_mode_t   mode;
    keyer_key_cb_t on_key;   /* called from the keyer task on every transition */
} keyer_cfg_t;

void keyer_start(const keyer_cfg_t *cfg);
void keyer_set_wpm(uint16_t wpm);
void keyer_set_mode(keyer_mode_t mode);
bool keyer_is_keying(void);
