/*
 * VFO / signal generator + CW keyer for the ESP32-S3-DevKitC-1 (WROOM-2).
 *
 * Hardware:
 *   - Si5351A clock generator on I2C  -> RF output (CLK0), used as the VFO.
 *   - SSD1306 128x64 OLED on 4-wire HW SPI (U8g2).
 *   - Rotary encoder (+push) for tuning, 3 buttons (Band/Step/Mode).
 *   - Iambic CW paddle -> keyer -> keys the Si5351 output + a key-line GPIO.
 *
 * This is a working skeleton meant for bench testing/tuning an SDR: it tunes
 * CLK0, shows the frequency, and keys CW. Extend the band table / UI as needed.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

#include "bsp_pins.h"
#include "si5351.h"
#include "display.h"
#include "input.h"
#include "keyer.h"
#include "morse.h"
#include "websrv.h"
#include "menu.h"

static const char *TAG = "vfo";

#define SI5351_I2C_ADDR   0x60
#define SI5351_XTAL_HZ    25000000UL   /* 25 MHz on most Adafruit/QRP-Labs breakouts */

/* WiFi is provisioned at runtime: first boot (or after "Forget WiFi") the
 * device is a SoftAP "VFO-Keyer" at 192.168.4.1 with a setup form. */

/* Tuning steps cycled by the STEP button (Hz). */
static const uint32_t k_steps[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };
#define N_STEPS   (sizeof(k_steps) / sizeof(k_steps[0]))
#define STEP_1KHZ 3      /* index of 1 kHz in k_steps (default / coarse toggle) */
#define STEP_1HZ  0      /* index of 1 Hz  (fine toggle) */

/* Simple band start table cycled by the BAND button (HF ham band edges). */
static const uint64_t k_bands[] = {
    1800000ULL, 3500000ULL, 7000000ULL, 10100000ULL,
    14000000ULL, 18068000ULL, 21000000ULL, 28000000ULL,
};
#define N_BANDS (sizeof(k_bands) / sizeof(k_bands[0]))

/* Amateur band edges, for labelling the display from the actual frequency
 * (not the last Band-button press). "--" when outside any band. */
static const struct { uint64_t lo, hi; const char *name; } k_band_ranges[] = {
    { 1800000,   2000000,   "160m" },
    { 3500000,   4000000,   "80m"  },
    { 5330000,   5410000,   "60m"  },
    { 7000000,   7300000,   "40m"  },
    { 10100000,  10150000,  "30m"  },
    { 14000000,  14350000,  "20m"  },
    { 18068000,  18168000,  "17m"  },
    { 21000000,  21450000,  "15m"  },
    { 24890000,  24990000,  "12m"  },
    { 28000000,  29700000,  "10m"  },
    { 50000000,  54000000,  "6m"   },
    { 144000000, 148000000, "2m"   },
};

static const char *band_name_for(uint64_t f)
{
    for (size_t i = 0; i < sizeof(k_band_ranges) / sizeof(k_band_ranges[0]); i++)
        if (f >= k_band_ranges[i].lo && f <= k_band_ranges[i].hi)
            return k_band_ranges[i].name;
    return "--";
}

static si5351_t s_si;
static volatile bool s_tx;

/* Shared VFO state, touched by the main loop AND the web-server task. */
static volatile uint64_t s_freq = 7030000ULL;
static volatile uint16_t s_wpm  = 20;
static volatile uint32_t s_band_idx = 2;  /* 40m */
static volatile int32_t  s_cal_ppb;       /* Si5351 reference correction (ppb) */
static uint8_t s_drive = SI5351_DRIVE_8MA;    /* CLK0/1 drive strength index */
static uint8_t s_kmode = KEYER_IAMBIC_B;      /* iambic A/B */
static volatile uint64_t s_freqB = 7040000ULL;/* VFO B on CLK2 (independent) */
static bool s_sel_b;                          /* encoder/buttons target: false=A, true=B */
static uint32_t s_bband_idx = 2;              /* band index for VFO B's Band button */
static bool s_vfob_on;                        /* CLK2 output enabled */
static bool s_quad_swap;                      /* I/Q swap (sideband select) */
static bool s_quad_active;                    /* VFO A currently in quadrature */
static bool s_rf_keyed;                       /* false=continuous carrier, true=keyed */
static int8_t s_phase_trim;                   /* quadrature phase fine-trim (reg LSBs) */
static int8_t s_iq_bal;                        /* I/Q amplitude balance: CLK1 drive offset */
static uint8_t s_contrast = 0x7F;             /* OLED contrast */
static u8g2_t *s_oled;                          /* for menu-driven contrast */
static volatile bool     s_web_dirty;     /* web changed freq/wpm/cal -> redraw+save */
static SemaphoreHandle_t s_key_lock;      /* serialises Si5351 keying (paddle vs auto) */

/* Last tuned frequency on each band, so cycling bands returns you to where you
 * left off instead of the band edge. Seeded from k_bands, then persisted. */
static uint64_t s_band_freq[N_BANDS];

/* --- Persisted VFO state (NVS) ---------------------------------------- */
#define NVS_NS   "vfo"

static void state_load(uint32_t *step_idx, uint32_t *band_idx, uint16_t *wpm)
{
    for (size_t i = 0; i < N_BANDS; i++) s_band_freq[i] = k_bands[i];

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;  /* keep defaults */
    nvs_get_u32(h, "step", step_idx);
    nvs_get_u32(h, "band", band_idx);
    nvs_get_u16(h, "wpm",  wpm);
    int32_t cal = 0;
    if (nvs_get_i32(h, "cal", &cal) == ESP_OK) s_cal_ppb = cal;
    nvs_get_u8(h, "drive", &s_drive);
    nvs_get_u8(h, "kmode", &s_kmode);
    if (s_drive > SI5351_DRIVE_8MA) s_drive = SI5351_DRIVE_8MA;
    if (s_kmode > KEYER_STRAIGHT)   s_kmode = KEYER_IAMBIC_B;
    nvs_get_u64(h, "freqb", &s_freqB);
    uint8_t u8;
    if (nvs_get_u8(h, "vfob", &u8) == ESP_OK) s_vfob_on   = u8;
    if (nvs_get_u8(h, "swap", &u8) == ESP_OK) s_quad_swap = u8;
    if (nvs_get_u8(h, "rfkey", &u8) == ESP_OK) s_rf_keyed = u8;
    if (nvs_get_u8(h, "selb", &u8) == ESP_OK) s_sel_b     = u8;
    nvs_get_u8(h, "contr", &s_contrast);
    int8_t i8 = 0;
    if (nvs_get_i8(h, "phase", &i8) == ESP_OK) s_phase_trim = i8;
    if (nvs_get_i8(h, "iqbal", &i8) == ESP_OK) s_iq_bal = i8;
    nvs_get_u32(h, "bband", &s_bband_idx);
    if (s_bband_idx >= N_BANDS) s_bband_idx = 2;
    size_t blen = sizeof(s_band_freq);
    nvs_get_blob(h, "bfreq", s_band_freq, &blen);   /* keeps seed if absent */
    nvs_close(h);
    if (*step_idx >= N_STEPS) *step_idx = STEP_1KHZ;
    if (*band_idx >= N_BANDS) *band_idx = 2;
    if (*wpm < 5 || *wpm > 60) *wpm = 20;
}

static void state_save(uint32_t step_idx, uint32_t band_idx, uint16_t wpm)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "step", step_idx);
    nvs_set_u32(h, "band", band_idx);
    nvs_set_u16(h, "wpm",  wpm);
    nvs_set_i32(h, "cal",  s_cal_ppb);
    nvs_set_u8(h, "drive", s_drive);
    nvs_set_u8(h, "kmode", s_kmode);
    nvs_set_u64(h, "freqb", s_freqB);
    nvs_set_u8(h, "vfob", s_vfob_on ? 1 : 0);
    nvs_set_u8(h, "swap", s_quad_swap ? 1 : 0);
    nvs_set_u8(h, "rfkey", s_rf_keyed ? 1 : 0);
    nvs_set_u8(h, "selb", s_sel_b ? 1 : 0);
    nvs_set_u8(h, "contr", s_contrast);
    nvs_set_i8(h, "phase", s_phase_trim);
    nvs_set_i8(h, "iqbal", s_iq_bal);
    nvs_set_u32(h, "bband", s_bband_idx);
    nvs_set_blob(h, "bfreq", s_band_freq, sizeof(s_band_freq));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "state saved: freq=%llu step=%lu band=%lu wpm=%u",
             (unsigned long long)s_band_freq[band_idx < N_BANDS ? band_idx : 0],
             (unsigned long)step_idx, (unsigned long)band_idx, wpm);
}

/* Keyer callback: gate the RF output and drive the external key line.
 * Called from BOTH the paddle keyer and the auto-sender tasks, so the Si5351
 * I2C write is serialised with a mutex. */
static void on_key(bool key_down)
{
    xSemaphoreTake(s_key_lock, portMAX_DELAY);
    s_tx = key_down;
    gpio_set_level(PIN_KEY_OUT, key_down ? 1 : 0);
    /* Only gate the RF in Keyed mode; in Carrier mode the output stays on. */
    if (s_rf_keyed) {
        si5351_output_enable(&s_si, SI5351_CLK0, key_down);
        if (s_quad_active)   /* gate the 90 deg output too, keeping the pair together */
            si5351_output_enable(&s_si, SI5351_CLK1, key_down);
    }
    xSemaphoreGive(s_key_lock);
}

/* Set the quadrature-pair drive: CLK0 = base, CLK1 = base + I/Q balance
 * (clamped 0..8mA). Coarse (only 4 hardware levels), for image nulling. */
static void apply_iq_drive(void)
{
    int q = (int)s_drive + s_iq_bal;
    if (q < 0) q = 0;
    if (q > SI5351_DRIVE_8MA) q = SI5351_DRIVE_8MA;
    si5351_set_drive(&s_si, SI5351_CLK0, (si5351_drive_t)s_drive);
    si5351_set_drive(&s_si, SI5351_CLK1, (si5351_drive_t)q);
}

/* Apply VFO A to the hardware: quadrature pair (CLK0+CLK1) when the frequency
 * supports it, otherwise a single CLK0 output. Idle output state follows the
 * RF-out mode: on for continuous carrier, off when keyed. */
static void set_vfoA(void)
{
    if (s_freq >= SI5351_QUAD_MIN_HZ &&
        si5351_set_quadrature(&s_si, s_freq, (si5351_drive_t)s_drive,
                              s_quad_swap, s_phase_trim) == ESP_OK) {
        s_quad_active = true;
        apply_iq_drive();                          /* CLK1 amplitude balance */
    } else {
        si5351_set_freq(&s_si, SI5351_CLK0, s_freq);
        s_quad_active = false;
    }
    bool on = !s_rf_keyed;                          /* carrier => on at idle */
    si5351_output_enable(&s_si, SI5351_CLK0, on);
    si5351_output_enable(&s_si, SI5351_CLK1, s_quad_active && on);
}

/* Apply VFO B (independent) on CLK2 / PLLB. */
static void set_vfoB(void)
{
    si5351_set_freq(&s_si, SI5351_CLK2, s_freqB);
    si5351_output_enable(&s_si, SI5351_CLK2, s_vfob_on);
}

/* -------- WebSocket command handlers (run in the httpd task) ----------- */
static void web_send(const char *text)   { morse_enqueue(text); }
static void web_repeat(const char *text, uint16_t gap_ms) { morse_set_repeat(text, gap_ms); }
static void web_stop(void)                { morse_stop(); }

static void web_wpm(uint16_t wpm)
{
    if (wpm < 5)  wpm = 5;
    if (wpm > 60) wpm = 60;
    s_wpm = wpm;
    keyer_set_wpm(wpm);
    morse_set_wpm(wpm);
    s_web_dirty = true;
}

static void web_freq(uint64_t hz)
{
    if (hz < 8000)        hz = 8000;
    if (hz > 160000000LL) hz = 160000000LL;
    s_freq = hz;
    if (s_band_idx < N_BANDS) s_band_freq[s_band_idx] = hz;
    set_vfoA();
    s_web_dirty = true;
}

static void web_freqb(uint64_t hz)
{
    if (hz < 8000)        hz = 8000;
    if (hz > 160000000LL) hz = 160000000LL;
    s_freqB = hz;
    set_vfoB();
    s_web_dirty = true;
}

static void web_vfob(bool on)   { s_vfob_on = on; set_vfoB(); s_web_dirty = true; }
static void web_rfkeyed(bool on) { s_rf_keyed = on; set_vfoA(); s_web_dirty = true; }

static void web_phase(int32_t lsb)
{
    if (lsb < -40) lsb = -40;
    if (lsb >  40) lsb =  40;
    s_phase_trim = (int8_t)lsb;
    set_vfoA();
    s_web_dirty = true;
}

static void web_iqbal(int32_t v)
{
    if (v < -3) v = -3;
    if (v >  3) v =  3;
    s_iq_bal = (int8_t)v;
    apply_iq_drive();
    s_web_dirty = true;
}

static void web_iqswap(bool on)
{
    s_quad_swap = on;
    set_vfoA();          /* re-apply the phase offset on the other channel */
    s_web_dirty = true;
}

/* Calibrate the reference: given the true carrier `measured` (Hz) observed
 * while the VFO was set to `target` (Hz), refine the ppb correction. Works
 * incrementally on top of any existing correction. */
static void web_cal(uint64_t measured, uint64_t target)
{
    if (measured < 1000 || target < 1000) return;
    double c0 = (double)s_cal_ppb / 1e9;
    double cnew = ((double)measured / (double)target) * (1.0 + c0) - 1.0;
    int32_t ppb = (int32_t)(cnew * 1e9 + (cnew >= 0 ? 0.5 : -0.5));
    if (ppb >  100000) ppb =  100000;    /* clamp to +/-100 ppm */
    if (ppb < -100000) ppb = -100000;
    s_cal_ppb = ppb;
    si5351_set_correction(&s_si, ppb);
    set_vfoA();                                    /* re-apply at new cal */
    set_vfoB();
    s_web_dirty = true;
    ESP_LOGI(TAG, "calibrated: measured=%llu target=%llu -> %ld ppb",
             (unsigned long long)measured, (unsigned long long)target,
             (long)ppb);
}

static void web_status(char *buf, size_t len)
{
    snprintf(buf, len,
             "{\"freq\":%llu,\"wpm\":%u,\"band\":\"%s\",\"cal\":%ld,"
             "\"freqb\":%llu,\"vfob\":%s,\"swap\":%s,\"quad\":%s,\"keyed\":%s,"
             "\"phase\":%d,\"iqbal\":%d,\"repeat\":%s,\"sending\":%s}",
             (unsigned long long)s_freq, s_wpm,
             band_name_for(s_freq),
             (long)s_cal_ppb,
             (unsigned long long)s_freqB,
             s_vfob_on ? "true" : "false",
             s_quad_swap ? "true" : "false",
             s_quad_active ? "true" : "false",
             s_rf_keyed ? "true" : "false",
             (int)s_phase_trim, (int)s_iq_bal,
             morse_is_repeating() ? "true" : "false",
             (morse_is_sending() || s_tx) ? "true" : "false");
}

/* -------- OLED settings menu -------------------------------------------- */
/* int32 mirrors the menu edits; apply_* push each change to the hardware. */
static int32_t m_wpm, m_kmode, m_drive, m_cal, m_vfob, m_swap, m_rfout, m_phase,
               m_iqbal, m_contr;

static void apply_wpm(int32_t v)
{
    s_wpm = (uint16_t)v;
    keyer_set_wpm(v);
    morse_set_wpm(v);
}
static void apply_kmode(int32_t v) { s_kmode = (uint8_t)v; keyer_set_mode(v); }
static void apply_drive(int32_t v)
{
    s_drive = (uint8_t)v;
    apply_iq_drive();                              /* CLK0=base, CLK1=base+bal */
}
static void apply_cal(int32_t v)
{
    s_cal_ppb = v;
    si5351_set_correction(&s_si, v);
    set_vfoA();                                    /* re-tune at new cal */
    set_vfoB();
}
static void apply_vfob(int32_t v) { s_vfob_on   = v; set_vfoB(); }
static void apply_swap(int32_t v) { s_quad_swap = v; set_vfoA(); }
static void apply_rfout(int32_t v) { s_rf_keyed = v; set_vfoA(); }
static void apply_phase(int32_t v) { s_phase_trim = (int8_t)v; set_vfoA(); }
static void apply_iqbal(int32_t v) { s_iq_bal = (int8_t)v; apply_iq_drive(); }
static void apply_contr(int32_t v)
{
    s_contrast = (uint8_t)v;
    if (s_oled) u8g2_SetContrast(s_oled, s_contrast);
}
static void menu_exit_action(int32_t v) { (void)v; menu_close(); }

static const char *const k_kmode_labels[] = { "Iambic A", "Iambic B", "Straight" };
static const char *const k_drive_labels[] = { "2mA", "4mA", "6mA", "8mA" };
static const char *const k_onoff_labels[] = { "Off", "On" };
static const char *const k_rfout_labels[] = { "Carrier", "Keyed" };
/* Approx. output level per drive step into 50 ohm, for the VFO top line. */
static const char *const k_drive_dbm[]    = { "-8dBm", "-3dBm", "0dBm", "+3dBm" };

static menu_item_t s_menu[] = {
    { "WPM",     &m_wpm,   5,        40,     1,  NULL,            apply_wpm   },
    { "Keyer",   &m_kmode, 0,        2,      1,  k_kmode_labels,  apply_kmode },
    { "Drive",   &m_drive, 0,        3,      1,  k_drive_labels,  apply_drive },
    { "Cal ppb", &m_cal,  -100000,   100000, 10, NULL,            apply_cal   },
    { "VFO B",   &m_vfob,  0,        1,      1,  k_onoff_labels,  apply_vfob  },
    { "I/Q swap",&m_swap,  0,        1,      1,  k_onoff_labels,  apply_swap  },
    { "Phase",   &m_phase, -40,      40,     1,  NULL,            apply_phase },
    { "IQ bal",  &m_iqbal, -3,       3,      1,  NULL,            apply_iqbal },
    { "RF out",  &m_rfout, 0,        1,      1,  k_rfout_labels,  apply_rfout },
    { "Contrast",&m_contr, 0,        255,    15, NULL,            apply_contr },
    { "Exit",    NULL,     0,        0,      0,  NULL,            menu_exit_action },
};
#define N_MENU (sizeof(s_menu) / sizeof(s_menu[0]))

/* Refresh the menu mirrors from live state before opening (web may have
 * changed WPM/cal in the meantime). */
static void menu_sync(void)
{
    m_wpm   = s_wpm;
    m_kmode = s_kmode;
    m_drive = s_drive;
    m_cal   = s_cal_ppb;
    m_vfob  = s_vfob_on;
    m_swap  = s_quad_swap;
    m_phase = s_phase_trim;
    m_iqbal = s_iq_bal;
    m_rfout = s_rf_keyed;
    m_contr = s_contrast;
}

static void i2c_bus_init(i2c_master_bus_handle_t *bus)
{
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, bus));
}

/* Probe the whole 7-bit address space and log every device that ACKs. */
static void i2c_scan(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "I2C scan (SDA=%d SCL=%d)...", PIN_I2C_SDA, PIN_I2C_SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X%s", addr,
                     addr == SI5351_I2C_ADDR ? "  <- Si5351" : "");
            found++;
        }
    }
    if (found == 0)
        ESP_LOGW(TAG, "  no I2C devices found (check wiring / pull-ups / power)");
}

/* TEMP diagnostic: toggle the 4 OLED signal pins at ~1 Hz so each can be
 * checked with a multimeter (should swing 0 <-> 3.3 V). Set to 0 to disable. */
#define OLED_PIN_TEST 0

#if OLED_PIN_TEST
static void oled_pin_test(void)
{
    const gpio_num_t pins[] = { PIN_OLED_SCLK, PIN_OLED_MOSI,
                                PIN_OLED_CS, PIN_OLED_RST };
    const char *names[] = { "SCLK/12", "MOSI/11", "CS/10", "RST/8" };
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_OLED_SCLK) | (1ULL << PIN_OLED_MOSI) |
                        (1ULL << PIN_OLED_CS)   | (1ULL << PIN_OLED_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    ESP_LOGW(TAG, "OLED PIN TEST: probe each pin, expect it to swing 0<->3.3V");
    int level = 0;
    for (;;) {
        level ^= 1;
        for (int i = 0; i < 4; i++) gpio_set_level(pins[i], level);
        ESP_LOGI(TAG, "OLED pins -> %s  [%s %s %s %s]",
                 level ? "3.3V" : "0V",
                 names[0], names[1], names[2], names[3]);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "VFO / CW keyer starting");

    /* NVS (persisted VFO state). Erase+retry if the partition is stale. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

#if OLED_PIN_TEST
    oled_pin_test();   /* never returns; comment out #define to run the app */
#endif

    /* Key-line output. */
    gpio_config_t keyout = {
        .pin_bit_mask = (1ULL << PIN_KEY_OUT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&keyout);
    gpio_set_level(PIN_KEY_OUT, 0);

    /* I2C + Si5351. */
    i2c_master_bus_handle_t bus;
    i2c_bus_init(&bus);
    i2c_scan(bus);                   /* rule out wiring/pull-up/power issues */
    ESP_ERROR_CHECK(si5351_init(&s_si, bus, SI5351_I2C_ADDR, SI5351_XTAL_HZ));

    /* Display + inputs. */
    u8g2_t *oled = display_init();
    input_init();

    /* VFO state — defaults, overridden by whatever was last saved to NVS.
     * step_idx/band_idx stay local (paddle/loop only); freq/wpm are shared
     * with the web task via s_freq/s_wpm. */
    uint32_t step_idx = STEP_1KHZ;   /* 1 kHz */
    uint32_t band_idx = s_band_idx;  /* 40m */
    uint16_t wpm  = 20;
    state_load(&step_idx, &band_idx, &wpm);
    s_band_idx = band_idx;
    s_freq = s_band_freq[band_idx];  /* last freq used on that band */
    s_wpm  = wpm;

    /* Power-on splash, then the live UI. */
    s_oled = oled;
    if (oled) {
        u8g2_SetContrast(oled, s_contrast);
        display_splash(oled, "VFO + CW Keyer", "YO4WM", "rev1.0");
        vTaskDelay(pdMS_TO_TICKS(1800));
    }

    /* Draw the UI FIRST, before any I2C, so a stalled/absent Si5351 can never
     * keep the screen from showing. */
    if (oled) {
        vfo_view_t vv = {
            .freqA = s_freq, .freqB = s_freqB, .step_hz = k_steps[step_idx],
            .rf = k_drive_dbm[s_drive],
            .band = band_name_for(s_sel_b ? s_freqB : s_freq),
            .sel_b = s_sel_b, .vfob_on = s_vfob_on, .quad = s_quad_active,
            .swap = s_quad_swap, .keyed = s_rf_keyed, .tx = false,
        };
        display_vfo(oled, &vv);
        ESP_LOGI(TAG, "display_vfo drawn");
    }

    menu_init(oled, s_menu, N_MENU);

    /* Serialise keying before either keyer task can call on_key. */
    s_key_lock = xSemaphoreCreateMutex();

    /* CW keyer (iambic paddle) + text auto-sender share the on_key callback. */
    keyer_cfg_t kc = { .wpm = s_wpm, .mode = s_kmode, .on_key = on_key };
    keyer_start(&kc);
    morse_cfg_t mc = { .wpm = s_wpm, .on_key = on_key };
    morse_start(&mc);

    ESP_LOGI(TAG, "configuring Si5351... (cal %ld ppb)", (long)s_cal_ppb);
    si5351_set_correction(&s_si, s_cal_ppb);
    si5351_set_drive(&s_si, SI5351_CLK0, (si5351_drive_t)s_drive);
    si5351_set_drive(&s_si, SI5351_CLK1, (si5351_drive_t)s_drive);
    set_vfoA();     /* VFO A: quad/single; enables output per RF-out mode */
    set_vfoB();     /* VFO B on CLK2 */
    ESP_LOGI(TAG, "Si5351 configured (quad=%d vfob=%d)", s_quad_active, s_vfob_on);

    /* WiFi + WebSocket control page. */
    websrv_cb_t wcb = {
        .on_send = web_send, .on_repeat = web_repeat, .on_stop = web_stop,
        .on_wpm = web_wpm,   .on_freq = web_freq,     .on_cal = web_cal,
        .on_freqb = web_freqb, .on_vfob = web_vfob,   .on_iqswap = web_iqswap,
        .on_rfkeyed = web_rfkeyed, .on_phase = web_phase, .on_iqbal = web_iqbal,
        .get_status = web_status,
    };
    websrv_start(&wcb);

    bool dirty = false;
    bool menu_redraw = false;
    bool save_pending = false;
    int64_t last_change_us = 0;
    for (;;) {
        int  delta      = input_encoder_delta();
        bool p_step     = input_button_pressed(BTN_STEP);
        bool p_band     = input_button_pressed(BTN_BAND);
        bool p_mode     = input_button_pressed(BTN_MODE);
        bool p_enc      = input_button_pressed(BTN_ENC);

        if (menu_is_open()) {
            /* --- MENU navigation: encoder rotates/edits, push selects, ---
             * --- Mode = back/exit. Band/Step are ignored here.        --- */
            if (delta)  { menu_rotate(delta); menu_redraw = true; }
            if (p_enc)  { menu_click();       menu_redraw = true; }
            if (p_mode) { menu_back();         menu_redraw = true; }

            if (menu_take_changed()) {         /* a setting was edited */
                save_pending = true;
                last_change_us = esp_timer_get_time();
            }
            if (!menu_is_open()) dirty = true; /* closed -> redraw the VFO */
            else if (menu_redraw) { menu_render(); menu_redraw = false; }
        } else {
            /* Encoder push opens the settings menu; Mode selects VFO A/B. */
            if (p_enc) {
                menu_sync();
                menu_open();
                menu_redraw = true;
                menu_render();
            }
            if (p_mode) { s_sel_b = !s_sel_b; dirty = true; }   /* A <-> B tune target */

            if (delta) {
                /* Tune whichever VFO is selected. */
                uint64_t base = s_sel_b ? s_freqB : s_freq;
                int64_t f = (int64_t)base + (int64_t)delta * k_steps[step_idx];
                if (f < 8000)        f = 8000;
                if (f > 160000000LL) f = 160000000LL;
                if (s_sel_b) {
                    s_freqB = (uint64_t)f;
                    set_vfoB();
                } else {
                    s_freq = (uint64_t)f;
                    s_band_freq[band_idx] = s_freq;   /* per-band memory for A */
                    set_vfoA();
                }
                dirty = true;
            }
            if (p_step) {
                step_idx = (step_idx + 1) % N_STEPS;
                dirty = true;
            }
            if (p_band) {
                if (s_sel_b) {
                    /* VFO B: hop to the next band's edge (no per-band memory). */
                    s_bband_idx = (s_bband_idx + 1) % N_BANDS;
                    s_freqB = k_bands[s_bband_idx];
                    set_vfoB();
                } else {
                    /* VFO A: current freq already saved to s_band_freq[band_idx]
                     * as we tuned; switch band and restore its last frequency. */
                    band_idx = (band_idx + 1) % N_BANDS;
                    s_band_idx = band_idx;
                    s_freq = s_band_freq[band_idx];
                    set_vfoA();
                }
                dirty = true;
            }
        }

        /* Pick up frequency/WPM changes made from the browser. */
        if (s_web_dirty) { s_web_dirty = false; if (!menu_is_open()) dirty = true; }

        static bool last_tx;
        if (!menu_is_open() && (dirty || s_tx != last_tx)) {
            if (oled) {
                vfo_view_t vv = {
                    .freqA = s_freq, .freqB = s_freqB, .step_hz = k_steps[step_idx],
                    .rf = k_drive_dbm[s_drive],
                    .band = band_name_for(s_sel_b ? s_freqB : s_freq),
                    .sel_b = s_sel_b, .vfob_on = s_vfob_on, .quad = s_quad_active,
                    .swap = s_quad_swap, .keyed = s_rf_keyed, .tx = s_tx,
                };
                display_vfo(oled, &vv);
            }
            websrv_broadcast_status();
            last_tx = s_tx;
            if (dirty) {
                /* Persistent state changed: defer the flash write until tuning
                 * settles (spares flash wear). TX-only redraws don't count. */
                save_pending = true;
                last_change_us = esp_timer_get_time();
            }
            dirty = false;
        }

        /* Commit to NVS ~2 s after the last change, and never while keying. */
        if (save_pending && !s_tx && !morse_is_sending() &&
            (esp_timer_get_time() - last_change_us) > 2000000) {
            state_save(step_idx, band_idx, s_wpm);
            save_pending = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
