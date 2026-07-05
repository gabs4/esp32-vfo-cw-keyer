/*
 * morse.h - Text -> CW auto-sender.
 *
 * Runs as its own FreeRTOS task and keys via the SAME key callback the iambic
 * keyer uses, so it gates the Si5351 output + key line identically. Timing is
 * standard PARIS: dit = 1200/WPM ms, dah = 3 dits, inter-element gap = 1 dit,
 * inter-letter gap = 3 dits, inter-word gap = 7 dits.
 *
 * Two ways to feed it:
 *   - morse_enqueue()    : append text to send once (live keyboard, one-shots).
 *   - morse_set_repeat() : a message re-sent forever with a gap between reps.
 * A live paddle touch aborts whatever is being sent and clears both.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*morse_key_cb_t)(bool key_down);

typedef struct {
    uint16_t       wpm;
    morse_key_cb_t on_key;
} morse_cfg_t;

void morse_start(const morse_cfg_t *cfg);
void morse_set_wpm(uint16_t wpm);

/* Append text to the one-shot send queue (returns immediately). */
void morse_enqueue(const char *text);

/* Set a message to repeat with `gap_ms` idle between repetitions.
 * Pass NULL or "" to disable repeating. */
void morse_set_repeat(const char *text, uint16_t gap_ms);

/* Abort current sending, clear the queue and any repeat message. */
void morse_stop(void);

bool morse_is_sending(void);

/* True while a repeat message is armed (set via morse_set_repeat). */
bool morse_is_repeating(void);
