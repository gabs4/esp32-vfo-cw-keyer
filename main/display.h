/*
 * display.h - U8g2 SSD1306 128x64 display over 4-wire HW SPI (nixy4/u8g2 port).
 *
 * Exposes the raw u8g2_t so the UI code can use the full U8g2 drawing API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "u8g2.h"

/* Initialise the SPI bus + panel. Returns pointer to the ready u8g2 instance. */
u8g2_t *display_init(void);

/* Draw the power-on splash (antenna art, callsign, name, revision). */
void display_splash(u8g2_t *u8g2, const char *name, const char *callsign,
                    const char *rev);

/* Everything the VFO screen needs to render in one shot. */
typedef struct {
    uint64_t    freqA;      /* VFO A frequency, Hz */
    uint64_t    freqB;      /* VFO B frequency, Hz */
    uint32_t    step_hz;    /* tuning step -> underlines that digit of the sel VFO */
    const char *rf;         /* top-left RF level, e.g. "+3dBm" */
    const char *band;       /* top-right band label, e.g. "40m" */
    bool        sel_b;      /* true => VFO B is the tuning target (else A) */
    bool        vfob_on;    /* VFO B output enabled */
    bool        quad;       /* VFO A in quadrature */
    bool        swap;       /* I/Q swapped */
    bool        keyed;      /* RF out keyed (vs continuous carrier) */
    bool        tx;         /* currently keying */
} vfo_view_t;

/* Render the dual-VFO screen. */
void display_vfo(u8g2_t *u8g2, const vfo_view_t *v);
