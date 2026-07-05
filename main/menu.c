/*
 * menu.c - see menu.h. Renders a scrolling list; edits the selected value.
 */
#include "menu.h"
#include <stdio.h>
#include <string.h>

static u8g2_t     *s_u8g2;
static menu_item_t *s_items;
static int          s_count;
static int          s_sel;        /* selected item */
static int          s_top;        /* first visible item (viewport) */
static bool         s_open;
static bool         s_editing;
static bool         s_changed;

#define ROWS_VISIBLE 4

void menu_init(u8g2_t *u8g2, menu_item_t *items, int count)
{
    s_u8g2 = u8g2;
    s_items = items;
    s_count = count;
}

void menu_open(void)  { s_open = true; s_editing = false; s_sel = 0; s_top = 0; }
void menu_close(void) { s_open = false; s_editing = false; }
bool menu_is_open(void) { return s_open; }
bool menu_take_changed(void) { bool c = s_changed; s_changed = false; return c; }

static void clamp_viewport(void)
{
    if (s_sel < s_top)                    s_top = s_sel;
    if (s_sel >= s_top + ROWS_VISIBLE)    s_top = s_sel - ROWS_VISIBLE + 1;
}

void menu_rotate(int delta)
{
    if (!s_open || delta == 0) return;
    menu_item_t *it = &s_items[s_sel];

    if (s_editing && it->value) {
        int32_t v = *it->value + (int32_t)delta * it->step;
        if (v < it->min) v = it->min;
        if (v > it->max) v = it->max;
        if (v != *it->value) {
            *it->value = v;
            if (it->on_change) it->on_change(v);
            s_changed = true;
        }
    } else {
        s_sel += (delta > 0) ? 1 : -1;
        if (s_sel < 0)          s_sel = 0;
        if (s_sel >= s_count)   s_sel = s_count - 1;
        clamp_viewport();
    }
}

void menu_click(void)
{
    if (!s_open) return;
    menu_item_t *it = &s_items[s_sel];
    if (!it->value) {                 /* action item (e.g. Exit) */
        if (it->on_change) it->on_change(0);
        return;
    }
    s_editing = !s_editing;           /* toggle edit / confirm */
}

void menu_back(void)
{
    if (!s_open) return;
    if (s_editing) s_editing = false;
    else           menu_close();
}

static void value_str(const menu_item_t *it, char *out, size_t n)
{
    if (!it->value) { out[0] = '\0'; return; }
    int32_t v = *it->value;
    if (it->labels) snprintf(out, n, "%s", it->labels[v - it->min]);
    else            snprintf(out, n, "%ld", (long)v);
}

void menu_render(void)
{
    if (!s_u8g2 || !s_open) return;
    u8g2_ClearBuffer(s_u8g2);

    u8g2_SetFont(s_u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(s_u8g2, 2, 10, "MENU");
    u8g2_DrawHLine(s_u8g2, 0, 13, 128);

    for (int r = 0; r < ROWS_VISIBLE; r++) {
        int i = s_top + r;
        if (i >= s_count) break;
        int y = 25 + r * 12;              /* baseline */
        bool selected = (i == s_sel);

        if (selected) {                   /* highlight row */
            u8g2_DrawBox(s_u8g2, 0, y - 10, 128, 12);
            u8g2_SetDrawColor(s_u8g2, 0);
        }

        char val[16];
        value_str(&s_items[i], val, sizeof(val));
        u8g2_DrawStr(s_u8g2, 2, y, s_items[i].name);
        if (val[0]) {
            int w = u8g2_GetStrWidth(s_u8g2, val);
            /* '[value]' while editing this row, else plain right-aligned. */
            if (selected && s_editing) {
                char eb[20];
                snprintf(eb, sizeof(eb), "[%s]", val);
                w = u8g2_GetStrWidth(s_u8g2, eb);
                u8g2_DrawStr(s_u8g2, 126 - w, y, eb);
            } else {
                u8g2_DrawStr(s_u8g2, 126 - w, y, val);
            }
        }

        if (selected) u8g2_SetDrawColor(s_u8g2, 1);
    }

    u8g2_SendBuffer(s_u8g2);
}
