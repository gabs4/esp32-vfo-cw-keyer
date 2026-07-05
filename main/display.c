/*
 * display.c - U8g2 SSD1306 128x64 over 3-wire *hardware* SPI (MOSI, SCK, RESET, CS).
 *
 * The panel has no DC line, so each 8-bit byte is transmitted as a 9-bit word:
 * a leading D/C flag bit followed by the 8 data bits. Within one U8g2 transfer
 * the D/C level is constant, so we bit-pack the whole chunk into a 9-bits-per-
 * byte stream and send it as a single HW SPI transaction (length in bits).
 * This runs on the SPI2 peripheral at MHz clock rates - fast enough for
 * animated content (VU-meters etc.), unlike a bit-banged 3-wire driver.
 *
 * Equivalent Arduino constructor: U8G2_SSD1306_128X64_NONAME_F_3W_HW_SPI.
 */
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "u8g2.h"

/* Set to 1 to bisect with U8g2's proven software 3-wire SPI (slow, for
 * bring-up only). Set back to 0 for the fast 9-bit HW SPI path. */
#define OLED_USE_SW_SPI 0

#define OLED_SPI_HOST   SPI2_HOST
#define OLED_SPI_HZ     (8 * 1000 * 1000)    /* verified solid on this wiring */

/* Worst case: full 1024-byte framebuffer in one SEND -> 1024*9 bits packed. */
#define PACK_BUF_BYTES  ((1024 * 9 + 7) / 8)

static const char *TAG = "display";
static u8g2_t s_u8g2;
static spi_device_handle_t s_spi;
static uint8_t s_dc;                                  /* current D/C flag bit */
static int     s_bitpos;                              /* accumulated bits this transfer */
static DMA_ATTR uint8_t s_packbuf[PACK_BUF_BYTES];    /* DMA-capable tx buffer */

/* Append one MSB-first bit, zeroing each byte on first touch. */
static inline void pack_bit(int bit)
{
    int p = s_bitpos;
    if ((p & 7) == 0) s_packbuf[p >> 3] = 0;      /* clear byte at its first bit */
    if (bit) s_packbuf[p >> 3] |= (0x80 >> (p & 7));
    s_bitpos = p + 1;
}

static bool s_spi_ready;

static void spi_bus_setup(void)
{
    if (s_spi_ready) return;                       /* idempotent */
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_OLED_MOSI,
        .sclk_io_num = PIN_OLED_SCLK,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PACK_BUF_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(OLED_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = OLED_SPI_HZ,
        .spics_io_num = -1,          /* CS handled manually per U8g2 transfer */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(OLED_SPI_HOST, &devcfg, &s_spi));
    s_spi_ready = true;
    ESP_LOGI(TAG, "SPI bus ready (MOSI=%d SCLK=%d)", PIN_OLED_MOSI, PIN_OLED_SCLK);
}

#if !OLED_USE_SW_SPI
/*
 * 3-wire HW SPI byte protocol. We CANNOT send each SEND as its own sub-byte
 * transaction: the ESP32 SPI rounds a transfer up to a whole byte on the wire,
 * so a 9-bit SEND would clock 16 bits and the stray 7 bits corrupt the next
 * word while CS is still low. Instead we accumulate all 9-bit words of the
 * whole START..END transfer into one buffer and transmit once at END. The only
 * byte-rounding padding (<8 bits) then lands at the very end, forming an
 * incomplete word the SSD1306 discards when CS rises.
 */
static uint8_t byte_hw_3wire(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        spi_bus_setup();
        break;
    case U8X8_MSG_BYTE_SET_DC:
        s_dc = arg_int ? 1 : 0;
        break;
    case U8X8_MSG_BYTE_START_TRANSFER:
        s_bitpos = 0;
        gpio_set_level(PIN_OLED_CS, 0);
        break;
    case U8X8_MSG_BYTE_SEND: {
        const uint8_t *data = (const uint8_t *)arg_ptr;
        int len = arg_int;
        for (int i = 0; i < len; i++) {
            pack_bit(s_dc);                        /* D/C flag bit first */
            for (int b = 7; b >= 0; b--)           /* 8 data bits, MSB first */
                pack_bit((data[i] >> b) & 1);
        }
        break;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (s_bitpos > 0) {
            /* Round up to a whole byte so the ESP32 sends deterministic
             * full-byte, MSB-first words (no ambiguous sub-byte tail). The
             * <8 zero padding bits form an incomplete 9-bit word that the
             * SSD1306 discards when CS rises. */
            int bits = (s_bitpos + 7) & ~7;
            spi_transaction_t t = {
                .length = bits,                    /* length is in bits */
                .tx_buffer = s_packbuf,
            };
            esp_err_t e = spi_device_polling_transmit(s_spi, &t);
            if (e != ESP_OK) ESP_LOGE(TAG, "spi tx failed: %s", esp_err_to_name(e));
        }
        gpio_set_level(PIN_OLED_CS, 1);
        break;
    default:
        return 0;
    }
    return 1;
}
#endif /* !OLED_USE_SW_SPI */

/* GPIO/delay backend: owns RESET + CS (and, in SW mode, SCLK/MOSI) + delays. */
static uint8_t gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)arg_ptr;
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT: {
        uint64_t mask = (1ULL << PIN_OLED_CS) | (1ULL << PIN_OLED_RST);
#if OLED_USE_SW_SPI
        mask |= (1ULL << PIN_OLED_SCLK) | (1ULL << PIN_OLED_MOSI);
#endif
        gpio_config_t io = { .pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(PIN_OLED_CS, 1);
        break;
    }
#if OLED_USE_SW_SPI
    case U8X8_MSG_GPIO_SPI_CLOCK:
        gpio_set_level(PIN_OLED_SCLK, arg_int);
        break;
    case U8X8_MSG_GPIO_SPI_DATA:
        gpio_set_level(PIN_OLED_MOSI, arg_int);
        break;
#endif
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int ? arg_int : 1));
        break;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(10);
        break;
    case U8X8_MSG_DELAY_100NANO:
        esp_rom_delay_us(1);
        break;
    case U8X8_MSG_GPIO_RESET:
        gpio_set_level(PIN_OLED_RST, arg_int);
        break;
    case U8X8_MSG_GPIO_CS:
        gpio_set_level(PIN_OLED_CS, arg_int);
        break;
    default:
        return 0;
    }
    return 1;
}

u8g2_t *display_init(void)
{
    /* SSD1306 128x64 "noname", full framebuffer (_f_), 3-wire SPI backend. */
#if !OLED_USE_SW_SPI
    spi_bus_setup();                 /* don't rely on U8X8_MSG_BYTE_INIT delivery */
#endif
#if OLED_USE_SW_SPI
    u8g2_Setup_ssd1306_128x64_noname_f(&s_u8g2, U8G2_R2,
                                       u8x8_byte_3wire_sw_spi,
                                       gpio_and_delay);
#else
    u8g2_Setup_ssd1306_128x64_noname_f(&s_u8g2, U8G2_R2,
                                       byte_hw_3wire,
                                       gpio_and_delay);
#endif
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_SetContrast(&s_u8g2, 0x7F);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
#if OLED_USE_SW_SPI
    ESP_LOGI(TAG, "SSD1306 (3-wire SW SPI, bring-up) ready");
#else
    ESP_LOGI(TAG, "SSD1306 (3-wire HW SPI @ %d Hz) ready", OLED_SPI_HZ);
#endif
    return &s_u8g2;
}

/* Format a frequency as "MHz.kkk.hhh" into out. */
static void freq_str(uint64_t hz, char *out, size_t n)
{
    uint32_t mhz = (uint32_t)(hz / 1000000ULL);
    uint32_t khz = (uint32_t)((hz % 1000000ULL) / 1000ULL);
    uint32_t u   = (uint32_t)(hz % 1000ULL);
    snprintf(out, n, "%lu.%03lu.%03lu",
             (unsigned long)mhz, (unsigned long)khz, (unsigned long)u);
}

/* Character index in `s` of the digit that `step` (a power of ten) tunes,
 * counting digits from the right and skipping the '.' separators. -1 if none. */
static int step_digit_index(const char *s, uint32_t step)
{
    int power = 0;
    for (uint32_t t = step; t >= 10; t /= 10) power++;
    int seen = -1;
    for (int i = (int)strlen(s) - 1; i >= 0; i--) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (++seen == power) return i;
        }
    }
    return -1;
}

/* Draw a big VFO frequency at baseline y, with a small "A"/"B" tag at the far
 * right and (optionally) an underline beneath the digit selected by step_hz.
 * Uses profont17 (fixed-width, dotted zero); right-aligned so a full
 * "160.000.000" fits alongside the tag. */
#define VFO_RIGHT   112              /* right edge the digits align to (before "Hz") */

static void draw_vfo_line(u8g2_t *u8g2, int y, uint64_t hz, char tag,
                          bool tag_filled, uint32_t step_hz)
{
    char line[24];
    freq_str(hz, line, sizeof(line));

    /* A/B tag chip at the far left, sitting near the text baseline. */
    char t[2] = { tag, '\0' };
    u8g2_SetFont(u8g2, u8g2_font_5x8_tf);
    int bx = 0, by = y - 9;                  /* 9x9 chip, 1px above baseline */
    if (tag_filled) {
        u8g2_DrawBox(u8g2, bx, by, 9, 9);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, bx + 2, y - 1, t);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        u8g2_DrawFrame(u8g2, bx, by, 9, 9);
        u8g2_DrawStr(u8g2, bx + 2, y - 1, t);
    }

    /* "Hz" unit at the right, on the baseline. */
    u8g2_DrawStr(u8g2, VFO_RIGHT + 3, y, "Hz");

    /* Right-align the digits (between the tag and the "Hz"). */
    u8g2_SetFont(u8g2, u8g2_font_profont17_tf);
    int w  = u8g2_GetStrWidth(u8g2, line);
    int x0 = VFO_RIGHT - w;
    u8g2_DrawStr(u8g2, x0, y, line);

    /* Underline the active digit (tuning cursor) - only when step given. */
    if (step_hz) {
        int idx = step_digit_index(line, step_hz);
        if (idx >= 0) {
            char pre[24];
            memcpy(pre, line, idx);
            pre[idx] = '\0';
            int px = u8g2_GetStrWidth(u8g2, pre);
            char ch[2] = { line[idx], '\0' };
            int cw = u8g2_GetStrWidth(u8g2, ch);
            u8g2_DrawHLine(u8g2, x0 + px, y + 1, cw);
        }
    }
}

/* Draw `s` horizontally centred on the current font at baseline y. */
static void draw_centered(u8g2_t *u8g2, int y, const char *s)
{
    int w = u8g2_GetStrWidth(u8g2, s);
    u8g2_DrawStr(u8g2, (128 - w) / 2, y, s);
}

void display_splash(u8g2_t *u8g2, const char *name, const char *callsign,
                    const char *rev)
{
    u8g2_ClearBuffer(u8g2);

    /* Vertical antenna with a small dipole top and radiating RF arcs. */
    const int ax = 16, base = 46, top = 24;
    u8g2_DrawVLine(u8g2, ax, top, base - top);       /* mast */
    u8g2_DrawHLine(u8g2, ax - 6, base, 13);          /* ground plane */
    u8g2_DrawHLine(u8g2, ax - 7, top, 15);           /* dipole */
    for (int r = 7; r <= 19; r += 6)                 /* RF radiating up-right */
        u8g2_DrawCircle(u8g2, ax, top, r, U8G2_DRAW_UPPER_RIGHT);

    /* Callsign, large, to the right of the antenna. */
    u8g2_SetFont(u8g2, u8g2_font_profont17_tf);
    if (callsign && *callsign) {
        int w = u8g2_GetStrWidth(u8g2, callsign);
        u8g2_DrawStr(u8g2, 40 + (88 - w) / 2, 22, callsign);
    }

    /* Project / tester name, centred. */
    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    if (name && *name) draw_centered(u8g2, 44, name);

    /* Tag line + revision on the bottom row. */
    u8g2_SetFont(u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(u8g2, 2, 62, "Ham Radio");
    if (rev && *rev) {
        int w = u8g2_GetStrWidth(u8g2, rev);
        u8g2_DrawStr(u8g2, 126 - w, 62, rev);
    }

    u8g2_SendBuffer(u8g2);
}

void display_vfo(u8g2_t *u8g2, const vfo_view_t *v)
{
    char line[24];

    u8g2_ClearBuffer(u8g2);

    /* Top line: RF level (dBm) left, band label right. */
    u8g2_SetFont(u8g2, u8g2_font_5x8_tf);
    if (v->rf && *v->rf) u8g2_DrawStr(u8g2, 2, 7, v->rf);
    if (v->band && *v->band) {
        int w = u8g2_GetStrWidth(u8g2, v->band);
        u8g2_DrawStr(u8g2, 126 - w, 7, v->band);
    }

    /* Two VFOs; the tag of the SELECTED (tuning target) one is filled, and the
     * digit cursor (underline) is drawn on that VFO. */
    draw_vfo_line(u8g2, 27, v->freqA, 'A', !v->sel_b, v->sel_b ? 0 : v->step_hz);
    draw_vfo_line(u8g2, 48, v->freqB, 'B',  v->sel_b, v->sel_b ? v->step_hz : 0);

    /* Bottom status flags, compact. */
    u8g2_SetFont(u8g2, u8g2_font_5x8_tf);
    line[0] = '\0';
    int x = 2;
    if (v->quad) { u8g2_DrawStr(u8g2, x, 62, v->swap ? "IQ~" : "IQ"); x += v->swap ? 24 : 18; }
    u8g2_DrawStr(u8g2, x, 62, v->keyed ? "KEY" : "CAR"); x += 24;
    if (v->vfob_on) { u8g2_DrawStr(u8g2, x, 62, "Bon"); x += 24; }
    if (v->tx) {
        u8g2_DrawBox(u8g2, 110, 54, 18, 10);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, 112, 62, "TX");
        u8g2_SetDrawColor(u8g2, 1);
    }

    u8g2_SendBuffer(u8g2);
}
