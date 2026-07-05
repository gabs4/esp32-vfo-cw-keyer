/*
 * websrv.c - WiFi provisioning + HTTP/WebSocket control server. See websrv.h.
 */
#include "websrv.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "mdns.h"
#include <string.h>

#define MDNS_HOST   "vfo-keyer"     /* -> http://vfo-keyer.local/ */

static const char *TAG = "websrv";

#define WIFI_NVS_NS   "wifi"
#define AP_SSID       "VFO-Keyer"       /* open AP for provisioning */
#define STA_TIMEOUT_US (15 * 1000000)   /* give up joining after 15 s */

static websrv_cb_t    s_cb;
static httpd_handle_t s_httpd;
static bool           s_ap_mode;        /* true = provisioning AP */
static volatile bool  s_got_ip;
static int            s_retries;

/* ------------------------------------------------------ NVS credentials */
static bool creds_load(char *ssid, size_t sl, char *pass, size_t pl, bool *force_ap)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t fa = 0;
    nvs_get_u8(h, "forceap", &fa);
    *force_ap = fa;
    esp_err_t e = nvs_get_str(h, "ssid", ssid, &sl);
    if (e == ESP_OK) nvs_get_str(h, "pass", pass, &pl);
    nvs_close(h);
    return (e == ESP_OK && ssid[0] != '\0');
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_set_u8(h, "forceap", 0);
    nvs_commit(h);
    nvs_close(h);
}

static void set_force_ap(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "forceap", v);
    nvs_commit(h);
    nvs_close(h);
}

void websrv_forget_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "WiFi credentials erased, rebooting to provisioning AP");
    esp_restart();
}

/* --------------------------------------------------------- form parsing */
/* In-place URL-decode (%XX and '+'). */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            int hi = s[1], lo = s[2];
            hi = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
            *o++ = (char)((hi << 4) | lo);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* Extract value of `key` from an application/x-www-form-urlencoded body. */
static bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n >= outlen) n = outlen - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* ------------------------------------------------------ provisioning UI */
static const char k_prov_page[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>CW Keyer WiFi Setup</title><style>"
"body{font-family:system-ui,sans-serif;background:#0b1020;color:#e6e9f0;margin:0}"
".w{max-width:420px;margin:0 auto;padding:24px}"
"input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;border-radius:8px;"
"border:1px solid #33406b;background:#161c33;color:#e6e9f0;font-size:16px}"
"button{width:100%;padding:12px;border:0;border-radius:8px;background:#2b6cff;color:#fff;font-size:16px}"
"</style></head><body><div class=w><h1>CW Keyer WiFi Setup</h1>"
"<p>Enter your network so the keyer can join it. It will reboot and connect.</p>"
"<form method=POST action=/save>"
"<label>Network (SSID)</label><input name=ssid required>"
"<label>Password</label><input name=pass type=password>"
"<button type=submit>Save &amp; Reboot</button></form></div></body></html>";

static esp_err_t prov_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, k_prov_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t prov_save(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len
                                                       : (int)sizeof(body) - 1;
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) return ESP_FAIL;
    body[r] = '\0';

    char ssid[33] = "", pass[65] = "";
    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    form_field(body, "pass", pass, sizeof(pass));
    creds_save(ssid, pass);
    ESP_LOGI(TAG, "saved SSID \"%s\", rebooting into station mode", ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<meta charset=utf-8><body style='font-family:sans-serif;background:#0b1020;color:#e6e9f0'>"
        "<h2>Saved. Rebooting and connecting...</h2>"
        "<p>Reconnect to your normal WiFi; the keyer will appear on it.</p></body>");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------ control UI */
static const char k_page[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>CW Keyer</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#0b1020;color:#e6e9f0}"
".wrap{max-width:640px;margin:0 auto;padding:16px}"
"h1{font-size:20px} .card{background:#161c33;border-radius:12px;padding:14px;margin:12px 0}"
"button{background:#2b6cff;color:#fff;border:0;border-radius:8px;padding:10px 14px;font-size:15px;cursor:pointer;margin:3px}"
"button.sec{background:#33406b} button.warn{background:#c0392b}"
"button.active{background:#2ecc71;color:#08210f;box-shadow:0 0 0 2px #2ecc71}"
"input,textarea{width:100%;box-sizing:border-box;background:#0b1020;color:#e6e9f0;border:1px solid #33406b;border-radius:8px;padding:8px;font-size:15px}"
"textarea{height:64px;resize:vertical} .row{display:flex;gap:8px;align-items:center}"
".stat{font-variant-numeric:tabular-nums} .mem{display:flex;gap:6px;margin:4px 0}"
".mem button{flex:1;text-align:left} .dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#555;margin-right:6px}"
".dot.on{background:#2ecc71}</style></head><body><div class=wrap>"
"<h1>ESP32 CW Keyer</h1>"
"<div class=card><div class=stat><span id=dot class=dot></span>"
"<b id=band>--</b> &nbsp; <b id=freq>--</b> Hz &nbsp; | &nbsp; <b id=wpm>--</b> WPM &nbsp; | &nbsp; <span id=send>idle</span></div></div>"
"<div class=card><label>Speed (WPM): <b id=wv>20</b></label>"
"<input type=range id=wpmr min=5 max=40 value=20></div>"
"<div class=card><label>VFO A (Hz) &mdash; quadrature LO <b id=qstat></b></label>"
"<div class=row><input id=freqi type=number value=7030000><button onclick=setFreq()>Set</button></div>"
"<label style='font-size:13px'><input type=checkbox id=swap style='width:auto;display:inline' onchange=setSwap()> I/Q swap (sideband)</label><br>"
"<label style='font-size:13px'><input type=checkbox id=keyed style='width:auto;display:inline' onchange=setKeyed()> Keyed (RF off unless keying; unchecked = continuous carrier)</label>"
"<label style='font-size:13px'>Phase trim: <b id=phv>0</b> (0 = 90&deg;)</label>"
"<input type=range id=phr min=-40 max=40 value=0>"
"<label style='font-size:13px'>I/Q amplitude bal (coarse): <b id=iqv>0</b></label>"
"<input type=range id=iqr min=-3 max=3 value=0></div>"
"<div class=card><label>VFO B (Hz) &mdash; CLK2 independent</label>"
"<div class=row><input id=freqbi type=number value=7040000><button onclick=setFreqB()>Set</button></div>"
"<label style='font-size:13px'><input type=checkbox id=vfob style='width:auto;display:inline' onchange=setVfob()> VFO B output on</label></div>"
"<div class=card><label>Message</label><textarea id=msg placeholder='CQ CQ DE ...'></textarea>"
"<div><button onclick=sendOnce()>Send once</button>"
"<button id=repbtn class=sec onclick=sendRep()>Repeat</button>"
"gap <input id=gap type=number value=3 style='width:60px;display:inline'>s"
"<button class=warn onclick=stop()>Stop</button></div></div>"
"<div class=card><label><input type=checkbox id=live style='width:auto;display:inline'> Live keyboard "
"(type below, sent as you go)</label><input id=livei placeholder='type here...'></div>"
"<div class=card><b>Calibration</b> (cal <b id=cal>0</b> ppb)"
"<p style='font-size:13px;opacity:.8'>Set the VFO, measure the true carrier on a reference rx, enter it below.</p>"
"<label>Set (target) Hz</label><input id=caltgt type=number value=7030000>"
"<label>Measured Hz</label><input id=calmeas type=number placeholder=7029890>"
"<button class=sec onclick=calib()>Calibrate</button></div>"
"<div class=card><b>Memories</b><div id=mems></div>"
"<div class=row><input id=mname placeholder=name style='flex:1'>"
"<button class=sec onclick=saveMem()>Save current message</button></div></div>"
"<div class=card><button class=warn onclick=forget()>Forget WiFi</button></div>"
"</div><script>"
"let ws,rt;"
"function conn(){ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=()=>send({cmd:'status'});"
"ws.onmessage=e=>{try{let s=JSON.parse(e.data);"
"document.getElementById('freq').textContent=s.freq;"
"document.getElementById('band').textContent=s.band||'';"
"document.getElementById('wpm').textContent=s.wpm;"
"if(s.cal!==undefined)document.getElementById('cal').textContent=s.cal;"
"document.getElementById('qstat').textContent=s.quad?'(active)':'(off <4.7MHz)';"
"if(s.swap!==undefined)document.getElementById('swap').checked=s.swap;"
"if(s.vfob!==undefined)document.getElementById('vfob').checked=s.vfob;"
"if(s.keyed!==undefined)document.getElementById('keyed').checked=s.keyed;"
"if(s.phase!==undefined&&document.activeElement!=phr){phr.value=s.phase;document.getElementById('phv').textContent=s.phase;}"
"if(s.iqbal!==undefined&&document.activeElement!=iqr){iqr.value=s.iqbal;document.getElementById('iqv').textContent=s.iqbal;}"
"document.getElementById('send').textContent=s.sending?'SENDING':'idle';"
"document.getElementById('dot').className='dot'+(s.sending?' on':'');"
"if(s.repeat!==undefined)document.getElementById('repbtn').className=s.repeat?'active':'sec';}catch(x){}};"
"ws.onclose=()=>{clearTimeout(rt);rt=setTimeout(conn,1500);};}"
"function send(o){if(ws&&ws.readyState==1)ws.send(JSON.stringify(o));}"
"function sendOnce(){send({cmd:'send',text:document.getElementById('msg').value});}"
"function sendRep(){send({cmd:'repeat',text:document.getElementById('msg').value,gap:+document.getElementById('gap').value});}"
"function stop(){send({cmd:'stop'});}"
"function setFreq(){send({cmd:'freq',value:+document.getElementById('freqi').value});}"
"function setFreqB(){send({cmd:'freqb',value:+document.getElementById('freqbi').value});}"
"function setVfob(){send({cmd:'vfob',on:document.getElementById('vfob').checked});}"
"function setSwap(){send({cmd:'iqswap',on:document.getElementById('swap').checked});}"
"function setKeyed(){send({cmd:'rfkeyed',on:document.getElementById('keyed').checked});}"
"function forget(){if(confirm('Erase WiFi and reboot to setup AP?'))send({cmd:'forget'});}"
"function calib(){let t=+document.getElementById('caltgt').value,m=+document.getElementById('calmeas').value;"
"if(t>0&&m>0)send({cmd:'cal',target:t,measured:m});}"
"let wpmr=document.getElementById('wpmr');"
"wpmr.oninput=()=>{document.getElementById('wv').textContent=wpmr.value;send({cmd:'wpm',value:+wpmr.value});};"
"let phr=document.getElementById('phr');"
"phr.oninput=()=>{document.getElementById('phv').textContent=phr.value;send({cmd:'phase',value:+phr.value});};"
"let iqr=document.getElementById('iqr');"
"iqr.oninput=()=>{document.getElementById('iqv').textContent=iqr.value;send({cmd:'iqbal',value:+iqr.value});};"
"let livei=document.getElementById('livei');"
"livei.oninput=()=>{if(!document.getElementById('live').checked)return;"
"let v=livei.value;if(v){let rb=document.getElementById('repbtn');"
"if(rb.className=='active'){send({cmd:'stop'});rb.className='sec';}"
"send({cmd:'send',text:v});livei.value='';}};"
"function mems(){return JSON.parse(localStorage.getItem('cwmem')||'[]');}"
"function drawMems(){let d=document.getElementById('mems');d.innerHTML='';"
"mems().forEach((m,i)=>{let r=document.createElement('div');r.className='mem';"
"r.innerHTML=`<button onclick='loadMem(${i})'>${m.n}</button>"
"<button class=sec onclick='sendMem(${i})'>Send</button>"
"<button class=warn onclick='delMem(${i})'>x</button>`;d.appendChild(r);});}"
"function saveMem(){let a=mems();a.push({n:document.getElementById('mname').value||'mem',t:document.getElementById('msg').value});"
"localStorage.setItem('cwmem',JSON.stringify(a));drawMems();}"
"function loadMem(i){document.getElementById('msg').value=mems()[i].t;}"
"function sendMem(i){send({cmd:'send',text:mems()[i].t});}"
"function delMem(i){let a=mems();a.splice(i,1);localStorage.setItem('cwmem',JSON.stringify(a));drawMems();}"
"drawMems();conn();setInterval(()=>send({cmd:'status'}),1000);"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, k_page, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------- websocket */
static void dispatch(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        const char *c = cmd->valuestring;
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        const cJSON *val  = cJSON_GetObjectItem(root, "value");
        const cJSON *gap  = cJSON_GetObjectItem(root, "gap");
        if (!strcmp(c, "send") && cJSON_IsString(text) && s_cb.on_send) {
            s_cb.on_send(text->valuestring);
        } else if (!strcmp(c, "repeat") && cJSON_IsString(text) && s_cb.on_repeat) {
            uint16_t g = cJSON_IsNumber(gap) ? (uint16_t)(gap->valuedouble * 1000) : 3000;
            s_cb.on_repeat(text->valuestring, g);
        } else if (!strcmp(c, "stop") && s_cb.on_stop) {
            s_cb.on_stop();
        } else if (!strcmp(c, "wpm") && cJSON_IsNumber(val) && s_cb.on_wpm) {
            s_cb.on_wpm((uint16_t)val->valuedouble);
        } else if (!strcmp(c, "freq") && cJSON_IsNumber(val) && s_cb.on_freq) {
            s_cb.on_freq((uint64_t)val->valuedouble);
        } else if (!strcmp(c, "cal") && s_cb.on_cal) {
            const cJSON *m = cJSON_GetObjectItem(root, "measured");
            const cJSON *t = cJSON_GetObjectItem(root, "target");
            if (cJSON_IsNumber(m) && cJSON_IsNumber(t))
                s_cb.on_cal((uint64_t)m->valuedouble, (uint64_t)t->valuedouble);
        } else if (!strcmp(c, "freqb") && cJSON_IsNumber(val) && s_cb.on_freqb) {
            s_cb.on_freqb((uint64_t)val->valuedouble);
        } else if (!strcmp(c, "vfob") && s_cb.on_vfob) {
            s_cb.on_vfob(cJSON_IsTrue(cJSON_GetObjectItem(root, "on")));
        } else if (!strcmp(c, "iqswap") && s_cb.on_iqswap) {
            s_cb.on_iqswap(cJSON_IsTrue(cJSON_GetObjectItem(root, "on")));
        } else if (!strcmp(c, "rfkeyed") && s_cb.on_rfkeyed) {
            s_cb.on_rfkeyed(cJSON_IsTrue(cJSON_GetObjectItem(root, "on")));
        } else if (!strcmp(c, "phase") && cJSON_IsNumber(val) && s_cb.on_phase) {
            s_cb.on_phase((int32_t)val->valuedouble);
        } else if (!strcmp(c, "iqbal") && cJSON_IsNumber(val) && s_cb.on_iqbal) {
            s_cb.on_iqbal((int32_t)val->valuedouble);
        } else if (!strcmp(c, "forget")) {
            websrv_forget_wifi();
        }
        /* "status" falls through: we always reply with status below. */
    }
    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK;   /* handshake */

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    if (frame.len && frame.len < 1024) {
        uint8_t buf[1024];
        frame.payload = buf;
        if (httpd_ws_recv_frame(req, &frame, frame.len) == ESP_OK) {
            buf[frame.len] = '\0';
            dispatch((char *)buf);
        }
    }

    char status[256] = "{}";
    if (s_cb.get_status) s_cb.get_status(status, sizeof(status));
    httpd_ws_frame_t out = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)status,
        .len = strlen(status),
    };
    return httpd_ws_send_frame(req, &out);
}

void websrv_broadcast_status(void)
{
    if (!s_httpd || s_ap_mode) return;
    char status[256] = "{}";
    if (s_cb.get_status) s_cb.get_status(status, sizeof(status));

    size_t n = 7;
    int fds[7];
    if (httpd_get_client_list(s_httpd, &n, fds) != ESP_OK) return;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_httpd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_ws_frame_t out = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)status,
                .len = strlen(status),
            };
            httpd_ws_send_frame_async(s_httpd, fds[i], &out);
        }
    }
}

/* ------------------------------------------------------------------ wifi */
static void sta_timeout_cb(void *arg)
{
    if (!s_got_ip) {
        ESP_LOGW(TAG, "no IP after %ds -> rebooting to provisioning AP",
                 STA_TIMEOUT_US / 1000000);
        set_force_ap(1);
        esp_restart();
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_retries <= 20) esp_wifi_connect();   /* keep trying until timeout */
        ESP_LOGW(TAG, "WiFi disconnected (retry %d)", s_retries);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_got_ip = true;
        ESP_LOGI(TAG, "connected -> open http://" IPSTR "/ in a browser",
                 IP2STR(&e->ip_info.ip));
    }
}

static void wifi_common_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}

static void wifi_start_sta(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "station connecting to \"%s\"...", ssid);

    /* mDNS: advertise http://vfo-keyer.local/ once we're on the LAN. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOST);
        mdns_instance_name_set("CW VFO Keyer");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: http://%s.local/", MDNS_HOST);
    }

    /* Reboot into provisioning if we never associate. */
    const esp_timer_create_args_t ta = { .callback = sta_timeout_cb, .name = "sta_to" };
    esp_timer_handle_t t;
    esp_timer_create(&ta, &t);
    esp_timer_start_once(t, STA_TIMEOUT_US);
}

static void wifi_start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(AP_SSID);
    wc.ap.authmode = WIFI_AUTH_OPEN;      /* open network for easy setup */
    wc.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG, "provisioning: join WiFi \"%s\" then open http://192.168.4.1/",
             AP_SSID);
}

int websrv_start(const websrv_cb_t *cb)
{
    s_cb = *cb;

    char ssid[33] = "", pass[65] = "";
    bool force_ap = false;
    bool have = creds_load(ssid, sizeof(ssid), pass, sizeof(pass), &force_ap);
    s_ap_mode = (!have || force_ap);

    wifi_common_init();
    if (s_ap_mode) {
        if (force_ap) set_force_ap(0);    /* one-shot: consume the flag */
        wifi_start_ap();
    } else {
        wifi_start_sta(ssid, pass);
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 4;
    esp_err_t err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) return err;

    if (s_ap_mode) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = prov_get };
        httpd_register_uri_handler(s_httpd, &root);
        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = prov_save };
        httpd_register_uri_handler(s_httpd, &save);
    } else {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        httpd_register_uri_handler(s_httpd, &root);
        httpd_uri_t ws = {
            .uri = "/ws", .method = HTTP_GET,
            .handler = ws_handler, .is_websocket = true,
        };
        httpd_register_uri_handler(s_httpd, &ws);
    }

    ESP_LOGI(TAG, "HTTP server started (%s mode)", s_ap_mode ? "AP setup" : "station");
    return ESP_OK;
}
