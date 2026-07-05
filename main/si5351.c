/*
 * si5351.c - Minimal Si5351A driver implementation.
 *
 * Frequency algorithm follows the Silicon Labs AN619 fractional model:
 *   fVCO = fXTAL * (a + b/c)              (PLL, must stay 600..900 MHz)
 *   fOUT = fVCO / (a2 + b2/c2) / rdiv     (MultiSynth divider)
 * We keep the MultiSynth an even integer where possible and let the PLL take
 * the fraction, which is the usual "one PLL per output" VFO approach.
 */
#include "si5351.h"
#include <string.h>
#include <math.h>

/* --- register map (subset) --- */
#define SI_REG_OUTPUT_ENABLE     3
#define SI_REG_CLK0_CTRL        16
#define SI_REG_PLLA_BASE        26
#define SI_REG_PLLB_BASE        34
#define SI_REG_MS0_BASE         42
#define SI_REG_MS1_BASE         50
#define SI_REG_MS2_BASE         58
#define SI_REG_CLK0_PHOFF      165   /* CLK0..2 phase offset: 165,166,167 */
#define SI_REG_PLL_RESET       177
#define SI_REG_XTAL_LOAD       183

#define SI_CLK_CTRL_POWERUP    0x00
#define SI_CLK_CTRL_INT_MODE   0x40   /* MSx integer mode */
#define SI_CLK_CTRL_PLLB_SEL   0x20
#define SI_CLK_CTRL_MS_SRC     0x0C   /* clock source = MultiSynth */

#define SI_PLL_RESET_A         0x20
#define SI_PLL_RESET_B         0x80

static esp_err_t si_write(si5351_t *d, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(d->dev, buf, sizeof(buf), 100);
}

static esp_err_t si_write_n(si5351_t *d, uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[9];
    if (n > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], data, n);
    return i2c_master_transmit(d->dev, buf, n + 1, 100);
}

/* Pack the 8-register block for a PLL or MultiSynth given (a + b/c) and rdiv. */
static void si_pack_params(uint32_t a, uint32_t b, uint32_t c,
                           uint8_t rdiv, bool divby4, uint8_t out[8])
{
    uint32_t p1, p2, p3;
    if (b == 0) {
        p1 = 128 * a - 512;
        p2 = 0;
        p3 = c;
    } else {
        uint32_t floorv = (uint32_t)((128ULL * b) / c);
        p1 = 128 * a + floorv - 512;
        p2 = 128 * b - c * floorv;
        p3 = c;
    }
    out[0] = (p3 >> 8) & 0xFF;
    out[1] = p3 & 0xFF;
    out[2] = ((p1 >> 16) & 0x03) | (rdiv << 4) | (divby4 ? 0x0C : 0x00);
    out[3] = (p1 >> 8) & 0xFF;
    out[4] = p1 & 0xFF;
    out[5] = ((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F);
    out[6] = (p2 >> 8) & 0xFF;
    out[7] = p2 & 0xFF;
}

esp_err_t si5351_init(si5351_t *dev, i2c_master_bus_handle_t bus,
                      uint8_t addr, uint32_t xtal_freq)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev->dev);
    if (err != ESP_OK) return err;

    dev->xtal_freq  = xtal_freq;
    dev->correction = 0;
    dev->drive[0] = dev->drive[1] = dev->drive[2] = SI5351_DRIVE_8MA;

    /* Disable all outputs, then power down all output drivers. */
    si_write(dev, SI_REG_OUTPUT_ENABLE, 0xFF);
    for (int r = 16; r <= 23; r++) si_write(dev, r, 0x80);

    /* Internal 10 pF crystal load (typical for breakouts). */
    si_write(dev, SI_REG_XTAL_LOAD, 0xD2);
    return ESP_OK;
}

void si5351_set_correction(si5351_t *dev, int32_t ppb)
{
    dev->correction = ppb;
}

static uint32_t si_corrected_xtal(si5351_t *dev)
{
    /* xtal * (1 + ppb/1e9) */
    return (uint32_t)((double)dev->xtal_freq *
                      (1.0 + (double)dev->correction / 1e9) + 0.5);
}

esp_err_t si5351_set_freq(si5351_t *dev, si5351_clk_t clk, uint64_t freq_hz)
{
    const uint32_t fxtal = si_corrected_xtal(dev);

    /* R divider stages for very low frequencies (<1 MHz keeps MS in range). */
    uint8_t rdiv = 0;      /* register field: 0..7 -> divide by 2^rdiv */
    uint64_t f = freq_hz;
    while (f < 500000ULL && rdiv < 7) { f <<= 1; rdiv++; }

    /* Choose MultiSynth integer divider so fVCO lands in 600..900 MHz. */
    uint32_t ms = 900000000UL / (uint32_t)f;
    if (ms < 6)   ms = 6;
    if (ms > 1800) ms = 1800;
    if (ms % 2)   ms++;                 /* even integer -> lowest jitter */

    uint64_t fvco = f * ms;
    if (fvco < 600000000ULL) {          /* nudge divider down to raise fVCO */
        ms -= 2;
        if (ms < 6) ms = 6;
        fvco = f * ms;
    }

    /* PLL feedback: fVCO = fxtal * (a + b/c) */
    uint32_t a = (uint32_t)(fvco / fxtal);
    uint64_t rem = fvco - (uint64_t)a * fxtal;
    uint32_t c = 1048575;               /* max denominator */
    uint32_t b = (uint32_t)((rem * c) / fxtal);

    uint8_t pll_base = (clk == SI5351_CLK2) ? SI_REG_PLLB_BASE : SI_REG_PLLA_BASE;
    uint8_t ms_base  = (clk == SI5351_CLK0) ? SI_REG_MS0_BASE
                     : (clk == SI5351_CLK1) ? SI_REG_MS1_BASE
                                            : SI_REG_MS2_BASE;

    uint8_t buf[8];
    si_pack_params(a, b, c, 0, false, buf);
    esp_err_t err = si_write_n(dev, pll_base, buf, 8);
    if (err != ESP_OK) return err;

    si_pack_params(ms, 0, 1, rdiv, false, buf);   /* MS integer */
    err = si_write_n(dev, ms_base, buf, 8);
    if (err != ESP_OK) return err;

    /* Route clock: MS source, integer mode, PLL select, powered up. */
    uint8_t ctrl = SI_CLK_CTRL_POWERUP | SI_CLK_CTRL_INT_MODE | SI_CLK_CTRL_MS_SRC;
    if (clk == SI5351_CLK2) ctrl |= SI_CLK_CTRL_PLLB_SEL;
    ctrl |= (dev->drive[clk] & 0x03);
    /* Single-output tuning has no phase offset on this clock. */
    si_write(dev, SI_REG_CLK0_PHOFF + clk, 0);
    si_write(dev, SI_REG_CLK0_CTRL + clk, ctrl);

    /* Reset the PLL that changed so the phase is defined. */
    si_write(dev, SI_REG_PLL_RESET,
             (clk == SI5351_CLK2) ? SI_PLL_RESET_B : SI_PLL_RESET_A);
    return ESP_OK;
}

esp_err_t si5351_output_enable(si5351_t *dev, si5351_clk_t clk, bool enable)
{
    static uint8_t oe = 0xFF;   /* mirror; bit=1 means disabled */
    if (enable) oe &= ~(1 << clk);
    else        oe |=  (1 << clk);
    return si_write(dev, SI_REG_OUTPUT_ENABLE, oe);
}

esp_err_t si5351_set_drive(si5351_t *dev, si5351_clk_t clk, si5351_drive_t drive)
{
    dev->drive[clk] = (uint8_t)drive;
    uint8_t ctrl = SI_CLK_CTRL_POWERUP | SI_CLK_CTRL_INT_MODE | SI_CLK_CTRL_MS_SRC;
    if (clk == SI5351_CLK2) ctrl |= SI_CLK_CTRL_PLLB_SEL;
    ctrl |= (drive & 0x03);
    return si_write(dev, SI_REG_CLK0_CTRL + clk, ctrl);
}

esp_err_t si5351_set_quadrature(si5351_t *dev, uint64_t freq_hz,
                                si5351_drive_t drive, bool swap_iq,
                                int8_t phase_trim)
{
    if (freq_hz < SI5351_QUAD_MIN_HZ) return ESP_ERR_INVALID_ARG;
    const uint32_t fxtal = si_corrected_xtal(dev);

    /* Integer MultiSynth divider a, with fVCO = a*fout in 600..900 MHz and
     * a <= 127 (phase register limit). Prefer even a for lowest jitter. */
    uint32_t a = (uint32_t)(750000000ULL / freq_hz);
    if (a & 1) a++;                                   /* make even */
    while ((uint64_t)a * freq_hz > 900000000ULL) a -= 2;
    while ((uint64_t)a * freq_hz < 600000000ULL) a += 2;
    if (a < 6 || a > 127) return ESP_ERR_INVALID_ARG;

    uint64_t fvco = (uint64_t)a * freq_hz;

    /* PLLA feedback: fVCO = fxtal * (n + b/c) */
    uint32_t n = (uint32_t)(fvco / fxtal);
    uint64_t rem = fvco - (uint64_t)n * fxtal;
    uint32_t c = 1048575;
    uint32_t b = (uint32_t)((rem * c) / fxtal);

    uint8_t buf[8];
    si_pack_params(n, b, c, 0, false, buf);
    esp_err_t err = si_write_n(dev, SI_REG_PLLA_BASE, buf, 8);
    if (err != ESP_OK) return err;

    /* Both MultiSynths = same integer divider a. */
    si_pack_params(a, 0, 1, 0, false, buf);
    si_write_n(dev, SI_REG_MS0_BASE, buf, 8);
    si_write_n(dev, SI_REG_MS1_BASE, buf, 8);

    dev->drive[0] = dev->drive[1] = (uint8_t)drive;
    uint8_t ctrl = SI_CLK_CTRL_POWERUP | SI_CLK_CTRL_INT_MODE |
                   SI_CLK_CTRL_MS_SRC | (drive & 0x03);   /* PLLA (bit5=0) */

    /* Phase offset in units of Tvco/4; offset = a gives exactly 90 deg. The
     * quadrature output gets +a (+trim); swap_iq moves it to the other channel. */
    int32_t ph = (int32_t)a + phase_trim;
    if (ph < 0)   ph = 0;
    if (ph > 127) ph = 127;
    si_write(dev, SI_REG_CLK0_PHOFF + 0, swap_iq ? (uint8_t)ph : 0);
    si_write(dev, SI_REG_CLK0_PHOFF + 1, swap_iq ? 0 : (uint8_t)ph);
    si_write(dev, SI_REG_CLK0_CTRL + 0, ctrl);
    si_write(dev, SI_REG_CLK0_CTRL + 1, ctrl);

    /* Reset PLLA so the two outputs start with the defined phase relationship. */
    return si_write(dev, SI_REG_PLL_RESET, SI_PLL_RESET_A);
}
