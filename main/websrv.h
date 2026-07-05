/*
 * websrv.h - WiFi provisioning + HTTP/WebSocket control server.
 *
 * Boot behaviour:
 *   - No STA credentials saved in NVS (or the "force AP" flag is set):
 *     start a SoftAP "VFO-Keyer" at 192.168.4.1 serving a provisioning form.
 *     Submitting it saves SSID/password to NVS and reboots into station mode.
 *   - Credentials present: connect as a station. If no IP is obtained within
 *     a timeout, set the force-AP flag and reboot back into provisioning, so a
 *     wrong/stale password can never lock the device out.
 *
 * In station mode "/" serves the CW control page and "/ws" is the WebSocket.
 *
 * WS commands (JSON):
 *   {"cmd":"send","text":"CQ CQ DE ..."}   one-shot text
 *   {"cmd":"repeat","text":"CQ ...","gap":3}  repeat with `gap` seconds
 *   {"cmd":"stop"}                          stop sending / clear repeat
 *   {"cmd":"wpm","value":22}                set keyer + sender speed
 *   {"cmd":"freq","value":7030000}          set VFO frequency (Hz)
 *   {"cmd":"cal","measured":7029890,"target":7030000}  refine Si5351 cal
 *   {"cmd":"freqb","value":7040000}         set VFO B frequency (CLK2)
 *   {"cmd":"vfob","on":true}                VFO B output on/off
 *   {"cmd":"iqswap","on":true}              swap quadrature I/Q (sideband)
 *   {"cmd":"rfkeyed","on":true}             RF out: false=carrier, true=keyed
 *   {"cmd":"phase","value":0}               quadrature phase trim (LSBs)
 *   {"cmd":"status"}                         request a status push
 *   {"cmd":"forget"}                         erase WiFi creds and reboot to AP
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* size_t */

typedef struct {
    void (*on_send)(const char *text);
    void (*on_repeat)(const char *text, uint16_t gap_ms);
    void (*on_stop)(void);
    void (*on_wpm)(uint16_t wpm);
    void (*on_freq)(uint64_t hz);
    void (*on_cal)(uint64_t measured_hz, uint64_t target_hz);
    void (*on_freqb)(uint64_t hz);      /* VFO B frequency (CLK2) */
    void (*on_vfob)(bool on);           /* VFO B output on/off */
    void (*on_iqswap)(bool on);         /* quadrature I/Q swap */
    void (*on_rfkeyed)(bool on);        /* RF out: false=carrier, true=keyed */
    void (*on_phase)(int32_t lsb);      /* quadrature phase trim (register LSBs) */
    void (*on_iqbal)(int32_t v);        /* I/Q amplitude balance (CLK1 drive offset) */
    /* Fill `buf` with a JSON status object, e.g.
     * {"freq":7030000,"wpm":22,"sending":true}. */
    void (*get_status)(char *buf, size_t len);
} websrv_cb_t;

/* Bring up WiFi (provisioning AP or station, decided from NVS) and start the
 * server. Returns ESP_OK on server start. */
int websrv_start(const websrv_cb_t *cb);

/* Push the current status JSON to all connected WebSocket clients. */
void websrv_broadcast_status(void);

/* Erase saved WiFi credentials and reboot into provisioning AP mode. */
void websrv_forget_wifi(void);
