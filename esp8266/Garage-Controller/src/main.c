#include "esp_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espressif/esp_wifi.h"
#include "espconn/espconn.h"
#include "gpio.h"

#include <string.h>
#include <stdio.h>

#define LED_GPIO         2
#define WIFI_MAX_RETRIES 3

// Initial defaults – change these to whatever you want initially
static struct {
    char ssid[32];
    char password[64];
} wifi_cfg = {
    "YourRealSSID",
    "YourRealPassword"
};

static int sta_retry               = 0;
static volatile int want_ap        = 0;
static volatile int want_sta_recon = 0;
static volatile int wifi_got_ip    = 0;
static int http_started            = 0;

static struct espconn http_server;
static esp_tcp http_tcp;

/* Small static buffers (avoid stack blowups in callbacks) */
static char http_req_buf[256];
static char http_resp_buf[256];

/******** RF CAL (required) ********/
uint32 ICACHE_FLASH_ATTR user_rf_cal_sector_set(void)
{
    flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0;

    switch(size_map) {
        case FLASH_SIZE_4M_MAP_256_256:        rf_cal_sec = 128 - 5; break;
        case FLASH_SIZE_8M_MAP_512_512:        rf_cal_sec = 256 - 5; break;
        case FLASH_SIZE_16M_MAP_512_512:
        case FLASH_SIZE_16M_MAP_1024_1024:     rf_cal_sec = 512 - 5; break;
        case FLASH_SIZE_32M_MAP_512_512:
        case FLASH_SIZE_32M_MAP_1024_1024:     rf_cal_sec = 1024 - 5; break;
        case FLASH_SIZE_64M_MAP_1024_1024:     rf_cal_sec = 2048 - 5; break;
        case FLASH_SIZE_128M_MAP_1024_1024:    rf_cal_sec = 4096 - 5; break;
        default: rf_cal_sec = 0; break;
    }
    return rf_cal_sec;
}

/******** Blink task ********/
static void blink_task(void *pv)
{
    (void)pv;
    while (1) {
        GPIO_OUTPUT_SET(LED_GPIO, 0);   // ON (active-low)
        vTaskDelay(300 / portTICK_RATE_MS);
        GPIO_OUTPUT_SET(LED_GPIO, 1);   // OFF
        vTaskDelay(300 / portTICK_RATE_MS);
    }
}

/******** Wi-Fi helpers ********/
static void start_sta(void)
{
    struct station_config st;
    memset(&st, 0, sizeof(st));

    wifi_set_opmode_current(STATION_MODE);

    strncpy((char *)st.ssid,     wifi_cfg.ssid,     sizeof(st.ssid)    - 1);
    strncpy((char *)st.password, wifi_cfg.password, sizeof(st.password) - 1);

    wifi_station_set_config_current(&st);
    wifi_station_connect();

    printf("STA: connecting to \"%s\"\n", wifi_cfg.ssid);
}

static void start_softap(void)
{
    struct softap_config ap;
    memset(&ap, 0, sizeof(ap));

    wifi_set_opmode_current(SOFTAP_MODE);

    strcpy((char *)ap.ssid,     "ESP8266-Setup");
    strcpy((char *)ap.password, "esp8266pw");
    ap.ssid_len        = 0;
    ap.channel         = 1;
    ap.authmode        = AUTH_WPA_WPA2_PSK;
    ap.ssid_hidden     = 0;
    ap.max_connection  = 4;
    ap.beacon_interval = 100;

    wifi_softap_set_config_current(&ap);

    printf("AP: SSID=ESP8266-Setup, password=esp8266pw, IP usually 192.168.4.1\n");
}

/******** Simple URL param parsing (no URL-encoding) ********/
static void get_param_value(const char *query, const char *key,
                            char *out, int out_len)
{
    out[0] = '\0';
    if (!query || !key || !out || out_len <= 0) return;

    const char *p = strstr(query, key);
    if (!p) return;

    p += strlen(key);          // move past "ssid" or "pass"
    if (*p == '=') p++;        // skip '=' if key did not include it

    int i = 0;
    while (*p && *p != '&' && *p != ' ' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

/******** HTTP (espconn) ********/

static void http_send_cb(void *arg)
{
    struct espconn *conn = (struct espconn *)arg;
    espconn_disconnect(conn);
}

/* minimal parser: GET /?ssid=...&pass=... */
static void http_recv_cb(void *arg, char *data, unsigned short len)
{
    (void)arg;

    /* Copy request into static buffer and terminate */
    int copy_len = (len < (sizeof(http_req_buf) - 1)) ? len : (sizeof(http_req_buf) - 1);
    memcpy(http_req_buf, data, copy_len);
    http_req_buf[copy_len] = '\0';

    char *first_line = strstr(http_req_buf, "GET ");
    char *query = NULL;
    if (first_line) {
        char *path_start = first_line + 4; // after "GET "
        char *space = strchr(path_start, ' ');
        if (space) *space = '\0';

        char *q = strchr(path_start, '?');
        if (q) query = q + 1;
    }

    if (query) {
        char new_ssid[32];
        char new_pass[64];

        get_param_value(query, "ssid", new_ssid, sizeof(new_ssid));
        get_param_value(query, "pass", new_pass, sizeof(new_pass));

        if (new_ssid[0]) {
            strncpy(wifi_cfg.ssid, new_ssid, sizeof(wifi_cfg.ssid) - 1);
            wifi_cfg.ssid[sizeof(wifi_cfg.ssid) - 1] = '\0';
        }
        if (new_pass[0]) {
            strncpy(wifi_cfg.password, new_pass, sizeof(wifi_cfg.password) - 1);
            wifi_cfg.password[sizeof(wifi_cfg.password) - 1] = '\0';
        }

        if (new_ssid[0] || new_pass[0]) {
            printf("Config updated via HTTP: ssid=\"%s\"\n", wifi_cfg.ssid);
            want_sta_recon = 1;
        }
    }

    /* Build a tiny plain-text response */
    int n = snprintf(
        http_resp_buf, sizeof(http_resp_buf),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK\nSSID=%s\nPASS=%s\n",
        wifi_cfg.ssid, wifi_cfg.password
    );
    if (n < 0) n = 0;
    if (n > (int)sizeof(http_resp_buf)) n = sizeof(http_resp_buf);

    struct espconn *conn = (struct espconn *)arg;
    espconn_regist_sentcb(conn, http_send_cb);
    espconn_send(conn, (uint8_t *)http_resp_buf, n);
}

static void http_connect_cb(void *arg)
{
    struct espconn *conn = (struct espconn *)arg;
    espconn_regist_recvcb(conn, http_recv_cb);
}


static void http_start_server(void)
{
    if (http_started) return;
    http_started = 1;

    memset(&http_server, 0, sizeof(http_server));
    memset(&http_tcp,    0, sizeof(http_tcp));

    http_server.type      = ESPCONN_TCP;
    http_server.state     = ESPCONN_NONE;
    http_server.proto.tcp = &http_tcp;
    http_server.proto.tcp->local_port = 80;

    espconn_regist_connectcb(&http_server, http_connect_cb);
    espconn_accept(&http_server);
    espconn_regist_time(&http_server, 300, 0);

    printf("HTTP: espconn server started on port 80\n");
}


/******** Wi-Fi event handler ********/
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt)
{
    switch (evt->event_id) {

    case EVENT_STAMODE_GOT_IP:
        printf("Got IP: " IPSTR "\n", IP2STR(&evt->event_info.got_ip.ip));
        sta_retry   = 0;
        wifi_got_ip = 1;
        break;

    case EVENT_STAMODE_DISCONNECTED:
        printf("Disconnected, retry=%d\n", sta_retry);
        wifi_got_ip = 0;
        if (sta_retry < WIFI_MAX_RETRIES) {
            sta_retry++;
            wifi_station_connect();
        } else {
            printf("STA failed, asking for AP\n");
            want_ap = 1;
        }
        break;

    default:
        break;
    }
}

/******** Wi-Fi management task ********/
static void wifi_task(void *pv)
{
    (void)pv;

    // Start with STA
    start_sta();

    while (1) {
        if (want_ap) {
            want_ap = 0;
            printf("wifi_task: switching to AP\n");
            start_softap();
            http_start_server();  // HTTP in AP mode
        }

        if (wifi_got_ip && !http_started) {
            printf("wifi_task: starting HTTP server in STA mode\n");
            http_start_server();
        }

        if (want_sta_recon) {
            want_sta_recon = 0;
            printf("wifi_task: reconnecting STA with new config\n");
            wifi_got_ip = 0;
            sta_retry   = 0;
            start_sta();
        }

        vTaskDelay(200 / portTICK_RATE_MS);
    }
}

/******** Entrypoint ********/
void user_init(void)
{
    uart_div_modify(0, UART_CLK_FREQ / 74880);
    printf("Booting...\n");
    espconn_init();  // IMPORTANT: init espconn before use
    // configure LED GPIO
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
    GPIO_OUTPUT_SET(LED_GPIO, 1);   // LED off initially

    xTaskCreate(blink_task, "blink", 256, NULL, 2, NULL);
    xTaskCreate(wifi_task,  "wifi",  768, NULL, 3, NULL);  // slightly more stack

    wifi_set_event_handler_cb(wifi_event_handler);
}
