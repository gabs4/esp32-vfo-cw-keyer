/*
 * si5351.h - Minimal Si5351A driver for a VFO/signal generator.
 *
 * Supports CLK0/CLK1/CLK2 frequency setting via PLLA/PLLB using the standard
 * fractional-divider math. Intended range ~8 kHz .. 150 MHz (integer-ish),
 * plenty for HF SDR test/tuning.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef enum {
    SI5351_CLK0 = 0,
    SI5351_CLK1 = 1,
    SI5351_CLK2 = 2,
} si5351_clk_t;

typedef enum {
    SI5351_DRIVE_2MA = 0,
    SI5351_DRIVE_4MA = 1,
    SI5351_DRIVE_6MA = 2,
    SI5351_DRIVE_8MA = 3,
} si5351_drive_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    uint32_t xtal_freq;     /* reference crystal, typ. 25000000 or 27000000 */
    int32_t  correction;    /* frequency correction in parts-per-billion */
    uint8_t  drive[3];      /* per-output drive strength (si5351_drive_t) */
} si5351_t;

/* Quadrature (0/90 deg) is only achievable above this frequency: the phase
 * register is 7-bit, so the MultiSynth divider a = fVCO/fOUT must be <= 127
 * (600 MHz / 127 ~= 4.72 MHz). Below this, use a single output instead. */
#define SI5351_QUAD_MIN_HZ   4725000ULL

/* Attach to an already-created I2C master bus. addr is usually 0x60. */
esp_err_t si5351_init(si5351_t *dev, i2c_master_bus_handle_t bus,
                      uint8_t addr, uint32_t xtal_freq);

/* Set output frequency (Hz) on a clock. Chooses PLLA for CLK0/1, PLLB for CLK2. */
esp_err_t si5351_set_freq(si5351_t *dev, si5351_clk_t clk, uint64_t freq_hz);

/* Program CLK0 and CLK1 (both on PLLA) at the same frequency, 90 deg apart, for
 * a quadrature (I/Q) LO. swap_iq moves the +90 deg from CLK1 to CLK0 (selects
 * the opposite sideband). phase_trim nudges the phase offset by that many
 * register LSBs (each ~= 90/a degrees) to fine-tune I/Q balance; 0 = exactly
 * 90 deg. Caller sets drive via `drive`. Returns ESP_ERR_INVALID_ARG if
 * freq < SI5351_QUAD_MIN_HZ. */
esp_err_t si5351_set_quadrature(si5351_t *dev, uint64_t freq_hz,
                                si5351_drive_t drive, bool swap_iq,
                                int8_t phase_trim);

/* Enable/disable an output. */
esp_err_t si5351_output_enable(si5351_t *dev, si5351_clk_t clk, bool enable);

/* Output drive strength (affects RF level). */
esp_err_t si5351_set_drive(si5351_t *dev, si5351_clk_t clk, si5351_drive_t drive);

/* PPB correction applied to the reference. */
void si5351_set_correction(si5351_t *dev, int32_t ppb);
