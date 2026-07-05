/*
 * input.h - Rotary encoder (PCNT quadrature) + debounced buttons / CW paddle.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_BAND = 0,
    BTN_STEP,
    BTN_MODE,
    BTN_ENC,        /* encoder push */
    BTN_COUNT,
} btn_id_t;

void input_init(void);

/* Net encoder detents since last call (signed; +CW). Consumes the delta. */
int  input_encoder_delta(void);

/* True once per press (edge-triggered, debounced). */
bool input_button_pressed(btn_id_t id);

/* Live level of the CW paddle contacts (true = pressed/closed). */
bool input_dit(void);
bool input_dah(void);
