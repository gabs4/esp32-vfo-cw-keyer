/*
 * morse.c - Text -> CW auto-sender task. See morse.h for the model.
 */
#include "morse.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <ctype.h>

/* Morse table for ASCII 0x20..0x5F (space .. '_'); '.'=dit, '-'=dah, ""=skip. */
static const char *k_morse[] = {
    /* 0x20 */ " ",        /* space -> word gap, handled specially */
    /* 0x21 ! */ "-.-.--",
    /* 0x22 " */ ".-..-.",
    /* 0x23 # */ "",
    /* 0x24 $ */ "...-..-",
    /* 0x25 % */ "",
    /* 0x26 & */ ".-...",
    /* 0x27 ' */ ".----.",
    /* 0x28 ( */ "-.--.",
    /* 0x29 ) */ "-.--.-",
    /* 0x2A * */ "",
    /* 0x2B + */ ".-.-.",
    /* 0x2C , */ "--..--",
    /* 0x2D - */ "-....-",
    /* 0x2E . */ ".-.-.-",
    /* 0x2F / */ "-..-.",
    /* 0x30 0 */ "-----",
    /* 0x31 1 */ ".----",
    /* 0x32 2 */ "..---",
    /* 0x33 3 */ "...--",
    /* 0x34 4 */ "....-",
    /* 0x35 5 */ ".....",
    /* 0x36 6 */ "-....",
    /* 0x37 7 */ "--...",
    /* 0x38 8 */ "---..",
    /* 0x39 9 */ "----.",
    /* 0x3A : */ "---...",
    /* 0x3B ; */ "-.-.-.",
    /* 0x3C < */ "",
    /* 0x3D = */ "-...-",
    /* 0x3E > */ "",
    /* 0x3F ? */ "..--..",
    /* 0x40 @ */ ".--.-.",
    /* 0x41 A */ ".-",
    /* 0x42 B */ "-...",
    /* 0x43 C */ "-.-.",
    /* 0x44 D */ "-..",
    /* 0x45 E */ ".",
    /* 0x46 F */ "..-.",
    /* 0x47 G */ "--.",
    /* 0x48 H */ "....",
    /* 0x49 I */ "..",
    /* 0x4A J */ ".---",
    /* 0x4B K */ "-.-",
    /* 0x4C L */ ".-..",
    /* 0x4D M */ "--",
    /* 0x4E N */ "-.",
    /* 0x4F O */ "---",
    /* 0x50 P */ ".--.",
    /* 0x51 Q */ "--.-",
    /* 0x52 R */ ".-.",
    /* 0x53 S */ "...",
    /* 0x54 T */ "-",
    /* 0x55 U */ "..-",
    /* 0x56 V */ "...-",
    /* 0x57 W */ ".--",
    /* 0x58 X */ "-..-",
    /* 0x59 Y */ "-.--",
    /* 0x5A Z */ "--..",
    /* 0x5B [ */ "",
    /* 0x5C \ */ "",
    /* 0x5D ] */ "",
    /* 0x5E ^ */ "",
    /* 0x5F _ */ "..--.-",
};

static const char *morse_for(char c)
{
    c = toupper((unsigned char)c);
    if (c < 0x20 || c > 0x5F) return "";
    return k_morse[c - 0x20];
}

/* --- Text queue (ring buffer) ---------------------------------------- */
#define QUEUE_LEN 512
static char             s_queue[QUEUE_LEN];
static volatile size_t  s_q_head, s_q_tail;   /* head=write, tail=read */

#define REPEAT_LEN 128
static char             s_repeat[REPEAT_LEN];
static volatile uint16_t s_repeat_gap_ms;
static int64_t          s_idle_since_us;       /* when the queue last went empty */

static morse_cfg_t      s_cfg;
static volatile uint16_t s_dit_ms = 60;        /* 20 WPM default */
static volatile bool    s_sending;
static SemaphoreHandle_t s_lock;

static bool q_empty(void) { return s_q_head == s_q_tail; }

static void q_push_str(const char *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (; *s; s++) {
        size_t next = (s_q_head + 1) % QUEUE_LEN;
        if (next == s_q_tail) break;    /* full: drop the rest */
        s_queue[s_q_head] = *s;
        s_q_head = next;
    }
    xSemaphoreGive(s_lock);
}

static int q_pop(void)
{
    int c = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_q_head != s_q_tail) {
        c = (unsigned char)s_queue[s_q_tail];
        s_q_tail = (s_q_tail + 1) % QUEUE_LEN;
    }
    xSemaphoreGive(s_lock);
    return c;
}

static void q_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_q_head = s_q_tail = 0;
    xSemaphoreGive(s_lock);
}

void morse_set_wpm(uint16_t wpm)
{
    if (wpm < 5)  wpm = 5;
    if (wpm > 60) wpm = 60;
    s_cfg.wpm = wpm;
    s_dit_ms = 1200 / wpm;
}

void morse_enqueue(const char *text)
{
    if (text && *text) q_push_str(text);
}

void morse_set_repeat(const char *text, uint16_t gap_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (text && *text) {
        strncpy(s_repeat, text, REPEAT_LEN - 1);
        s_repeat[REPEAT_LEN - 1] = '\0';
        s_repeat_gap_ms = gap_ms;
        s_idle_since_us = 0;   /* send the first rep immediately */
    } else {
        s_repeat[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

void morse_stop(void)
{
    q_clear();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_repeat[0] = '\0';
    xSemaphoreGive(s_lock);
}

bool morse_is_sending(void) { return s_sending; }

bool morse_is_repeating(void) { return s_repeat[0] != '\0'; }

/* True if a paddle is touched -> operator wants to take over. */
static bool paddle_active(void) { return input_dit() || input_dah(); }

/* Delay `ms`, bailing early if a paddle is touched. Returns false if aborted. */
static bool delay_ms_abortable(uint16_t ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        if (paddle_active()) return false;
        vTaskDelay(1);
    }
    return true;
}

/* Key one element (dit or dah) plus its trailing 1-dit gap. */
static bool send_element(bool dah)
{
    if (paddle_active()) return false;
    s_sending = true;
    if (s_cfg.on_key) s_cfg.on_key(true);
    bool ok = delay_ms_abortable(dah ? s_dit_ms * 3 : s_dit_ms);
    if (s_cfg.on_key) s_cfg.on_key(false);
    if (!ok) { s_sending = false; return false; }
    return delay_ms_abortable(s_dit_ms);   /* inter-element gap */
}

/* Send one character; returns false if aborted by the paddle. */
static bool send_char(char c)
{
    if (c == ' ') {
        /* Word gap is 7 dits; 1 already spent after the previous element. */
        return delay_ms_abortable(s_dit_ms * 6);
    }
    const char *m = morse_for(c);
    if (!*m) return true;                  /* unknown char: skip silently */
    for (; *m; m++)
        if (!send_element(*m == '-')) return false;
    /* Inter-letter gap is 3 dits; 1 already spent after the last element. */
    return delay_ms_abortable(s_dit_ms * 2);
}

static void morse_task(void *arg)
{
    for (;;) {
        int c = q_pop();

        if (c >= 0) {
            if (!send_char((char)c)) {
                /* Paddle took over: drop everything so it has the key. */
                morse_stop();
            }
            s_sending = false;
            continue;
        }

        /* Queue empty. Re-arm a repeat message once its gap has elapsed. */
        s_sending = false;
        if (s_idle_since_us == 0) s_idle_since_us = esp_timer_get_time();

        bool have_repeat;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        have_repeat = (s_repeat[0] != '\0');
        xSemaphoreGive(s_lock);

        if (have_repeat && !paddle_active()) {
            int64_t idle_ms = (esp_timer_get_time() - s_idle_since_us) / 1000;
            if (idle_ms >= s_repeat_gap_ms) {
                char buf[REPEAT_LEN];
                xSemaphoreTake(s_lock, portMAX_DELAY);
                strncpy(buf, s_repeat, sizeof(buf));
                xSemaphoreGive(s_lock);
                q_push_str(buf);
                s_idle_since_us = 0;
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void morse_start(const morse_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_lock = xSemaphoreCreateMutex();
    morse_set_wpm(cfg->wpm ? cfg->wpm : 20);
    xTaskCreatePinnedToCore(morse_task, "morse", 4096, NULL,
                            configMAX_PRIORITIES - 4, NULL, 1);
}
