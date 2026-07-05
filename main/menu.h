/*
 * menu.h - Minimal OLED settings menu (u8g2), driven by the encoder + a button.
 *
 * A menu is a flat list of items. Each item is either:
 *   - a value:  edit an int32 in [min..max] by `step`; shown numerically or,
 *               if `labels` is set, as labels[value - min] (enums).
 *   - an action: value == NULL; selecting it calls on_change(0) (e.g. "Exit").
 * on_change() is called live on every value change so the app applies it
 * immediately.
 *
 * Interaction model (wire these from your input layer):
 *   - menu_rotate(delta): navigate the list, or adjust the value while editing.
 *   - menu_click():       enter edit on a value / confirm; run an action item.
 *   - menu_back():        leave edit mode, or close the menu if not editing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "u8g2.h"

typedef struct {
    const char        *name;
    int32_t           *value;     /* NULL => action item */
    int32_t            min, max, step;
    const char *const *labels;    /* NULL => numeric; else enum labels */
    void             (*on_change)(int32_t v);
} menu_item_t;

void menu_init(u8g2_t *u8g2, menu_item_t *items, int count);

void menu_open(void);
void menu_close(void);
bool menu_is_open(void);

void menu_rotate(int delta);
void menu_click(void);
void menu_back(void);

void menu_render(void);

/* True (once) if a value changed since the last call - use to trigger a save. */
bool menu_take_changed(void);
