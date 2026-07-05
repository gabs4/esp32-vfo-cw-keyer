/*
 * bsp_pins.h - Board pin map for the ESP32-S3-DevKitC-1 (WROOM-2) VFO / CW keyer.
 *
 * IMPORTANT constraints for this module (Octal flash + Octal PSRAM):
 *   - GPIO 26..37 are consumed by the SPI flash / PSRAM -> DO NOT USE.
 *   - GPIO 0, 3, 45, 46 are strapping pins             -> avoid for I/O.
 *   - GPIO 19, 20 are the native USB D-/D+             -> avoid.
 *   - GPIO 43, 44 are the console UART (TX0/RX0)       -> reserved for logs.
 *   - GPIO 22..25 do not exist on the ESP32-S3.
 *
 * Everything below is chosen from the remaining safe pins:
 *   1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,47,48
 *
 * Change any assignment here to match your wiring; nothing else references
 * raw GPIO numbers.
 */
#pragma once

#include "driver/gpio.h"

/* ---- SSD1306 OLED, 3-wire SPI (no DC line; D/C sent as the 9th bit) ---- */
/* Wired as tested: MOSI, SCK, RESET, CS. Driven by software 3-wire SPI. */
#define PIN_OLED_SCLK   GPIO_NUM_12   /* SSD1306 "D0" / SCK  */
#define PIN_OLED_MOSI   GPIO_NUM_11   /* SSD1306 "D1" / MOSI */
#define PIN_OLED_CS     GPIO_NUM_10
#define PIN_OLED_RST    GPIO_NUM_8

/* ---- I2C (Si5351A clock generator) ---- */
#define PIN_I2C_SDA     GPIO_NUM_4
#define PIN_I2C_SCL     GPIO_NUM_5

/* ---- Rotary encoder (quadrature A/B + push) ---- */
#define PIN_ENC_A       GPIO_NUM_6
#define PIN_ENC_B       GPIO_NUM_7
#define PIN_ENC_PUSH    GPIO_NUM_15

/* ---- Control buttons (active-low, internal pull-ups) ---- */
#define PIN_BTN_BAND    GPIO_NUM_16
#define PIN_BTN_STEP    GPIO_NUM_17
#define PIN_BTN_MODE    GPIO_NUM_18

/* ---- CW paddle inputs (active-low, internal pull-ups) ---- */
#define PIN_KEY_DIT     GPIO_NUM_1
#define PIN_KEY_DAH     GPIO_NUM_2

/* ---- CW key line out (to PTT / QSK switch, drives a transistor) ---- */
#define PIN_KEY_OUT     GPIO_NUM_21
