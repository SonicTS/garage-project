#include "wifi_manager.h"

#include "esp_common.h"
#include "espressif/esp_wifi.h"
#include "espressif/esp_sta.h"
#include "lwip/ip_addr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "queue.h"

#include "app_events.h"

#include <string.h>
#include <stdio.h>

/* ==== Tunables ==== */
#define WIFI_MAX_RETRIES             5
#define DHCP_IP_TIMEOUT_MS           20000
#define MAX_WIFI_CANDIDATES          10
#define MIN_STATE_GAP_MS             400
#define WIFI_STUCK_TIMEOUT_MS        30000
#define WIFI_BACKOFF_BASE_MS         500
#define WIFI_BACKOFF_MAX_MS          30000
#define WIFI_MAX_BACKOFF_LEVEL       4
#define WIFI_MAX_RECOVERY_RESETS     2
#define WIFI_MAX_FAILURES_BEFORE_AP  12
#define WIFI_TASK_TICK_MS            200
#define WIFI_EVT_QUEUE_LEN           8

#define WIFI_AP_SSID                 "ESP8266-Setup"
#define WIFI_AP_PASSWORD             "esp8266pw"

/* Common disconnect reasons we care about (Non-OS SDK typically uses these numbers) */
#define REASON_AUTH_EXPIRE               2
#define REASON_ASSOC_EXPIRE              4
#define REASON_ASSOC_LEAVE               8
#define REASON_4WAY_HANDSHAKE_TIMEOUT   15
/* These vary across SDKs; keep for best-effort */
#define REASON_BEACON_TIMEOUT          200
#define REASON_NO_AP_FOUND             201

typedef enum {
    WIFI_BOOT = 0,
    WIFI_INIT,
    WIFI_SCAN,
    WIFI_SELECT_BEST_AP,
    WIFI_CONNECT_SSID,
    WIFI_CONNECT_BSSID,
    WIFI_WAIT_IP,
    WIFI_CONNECTED,
    WIFI_BACKOFF,
    WIFI_RECOVERY_RESET,
    WIFI_AP_MODE
} wifi_state_t;

typedef enum {
    CONNECT_MODE_NONE = 0,
    CONNECT_MODE_SSID,
    CONNECT_MODE_BSSID
} wifi_connect_mode_t;

typedef enum {
    WIFI_EVT_GOT_IP = 0,
    WIFI_EVT_DISCONNECTED,
    WIFI_EVT_SCAN_DONE
} wifi_evt_type_t;

typedef struct {
    wifi_evt_type_t type;
    union {
        struct {
            uint32_t ip;
        } got_ip;
        struct {
            uint8_t reason;
        } disconnect;
        struct {
            STATUS status;
            struct bss_info *list;
        } scan;
    } data;
} wifi_evt_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
} bss_candidate_t;

typedef struct {
    wifi_state_t state;
    wifi_config_t cfg;

    bss_candidate_t candidates[MAX_WIFI_CANDIDATES];
    int candidate_count;
    int candidate_index;

    int retry_count;
    int backoff_level;

    uint32_t last_action_tick;
    uint32_t connect_tick;
    uint32_t state_enter_tick;

    int has_ip;
    uint32_t ip;

    uint8_t last_reason;
    int consecutive_failures;
    int stuck_counter;
    int recovery_reset_count;

    int in_ap_mode;
    int scan_in_progress;
    int state_started;
    wifi_connect_mode_t connect_mode;
} wifi_ctx_t;

static wifi_ctx_t g_wifi;

static xQueueHandle g_cfg_update_queue = NULL;
static xQueueHandle g_wifi_evt_queue = NULL;
typedef struct { wifi_config_t wifi; } wifi_cfg_msg_t;

/* Forward declarations */
static void wifi_task(void *pv);
static void start_softap(void);
static void start_station(const bss_candidate_t *candidate);
static void handle_disconnect(uint8_t reason);
static void handle_scan(struct bss_info *list);
static void wifi_transition(wifi_state_t next);
static void wifi_run_state(void);
static void wifi_process_event(const wifi_evt_t *evt);
static void wifi_flush_event_queue(void);
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt);
static void scan_done_cb(void *arg, STATUS status);

static void state_init(void);
static void state_scan(void);
static void state_select_best_ap(void);
static void state_connect_ssid(void);
static void state_connect_bssid(void);
static void state_wait_ip(void);
static void state_connected(void);
static void state_backoff(void);
static void state_recovery_reset(void);
static void state_ap_mode(void);

static void send_app_event(const app_evt_t *ev)
{
    if (g_app_event_queue) {
        xQueueSend(g_app_event_queue, ev, 0);
    }
}

static uint32_t wifi_now_ticks(void)
{
    return xTaskGetTickCount();
}

static uint32_t wifi_elapsed_ms(uint32_t since_tick)
{
    return (wifi_now_ticks() - since_tick) * portTICK_RATE_MS;
}

static int wifi_gap_ready(void)
{
    return wifi_elapsed_ms(g_wifi.last_action_tick) >= MIN_STATE_GAP_MS;
}

static void wifi_mark_action(void)
{
    g_wifi.last_action_tick = wifi_now_ticks();
}

static uint32_t wifi_backoff_delay_ms(void)
{
    uint32_t delay = WIFI_BACKOFF_BASE_MS;

    if (g_wifi.backoff_level > 1) {
        delay <<= (g_wifi.backoff_level - 1);
    }
    if (delay > WIFI_BACKOFF_MAX_MS) {
        delay = WIFI_BACKOFF_MAX_MS;
    }
    return delay;
}

static const char *wifi_state_name(wifi_state_t state)
{
    switch (state) {
    case WIFI_BOOT: return "BOOT";
    case WIFI_INIT: return "INIT";
    case WIFI_SCAN: return "SCAN";
    case WIFI_SELECT_BEST_AP: return "SELECT_BEST_AP";
    case WIFI_CONNECT_SSID: return "CONNECT_SSID";
    case WIFI_CONNECT_BSSID: return "CONNECT_BSSID";
    case WIFI_WAIT_IP: return "WAIT_IP";
    case WIFI_CONNECTED: return "CONNECTED";
    case WIFI_BACKOFF: return "BACKOFF";
    case WIFI_RECOVERY_RESET: return "RECOVERY_RESET";
    case WIFI_AP_MODE: return "AP_MODE";
    default: return "UNKNOWN";
    }
}

static void wifi_flush_event_queue(void)
{
    wifi_evt_t evt;

    if (!g_wifi_evt_queue) {
        return;
    }

    while (xQueueReceive(g_wifi_evt_queue, &evt, 0) == pdTRUE) {
    }
}

static void wifi_transition(wifi_state_t next)
{
    if (g_wifi.state != next) {
        LOGF("wifi_manager: state %s -> %s\n",
             wifi_state_name(g_wifi.state), wifi_state_name(next));
    }

    g_wifi.state = next;
    g_wifi.state_enter_tick = wifi_now_ticks();
    g_wifi.state_started = 0;
}

static void start_station(const bss_candidate_t *candidate)
{
    struct station_config st;

    memset(&st, 0, sizeof(st));

    wifi_set_opmode_current(STATION_MODE);
    g_wifi.in_ap_mode = 0;

    strncpy((char *)st.ssid, g_wifi.cfg.ssid, sizeof(st.ssid));
    st.ssid[sizeof(st.ssid) - 1] = 0;

    strncpy((char *)st.password, g_wifi.cfg.password, sizeof(st.password));
    st.password[sizeof(st.password) - 1] = 0;

    if (candidate) {
        memcpy(st.bssid, candidate->bssid, sizeof(st.bssid));
        st.bssid_set = 1;
        LOGF("wifi_manager: connect BSSID %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d\n",
             candidate->bssid[0], candidate->bssid[1], candidate->bssid[2],
             candidate->bssid[3], candidate->bssid[4], candidate->bssid[5],
             candidate->channel, candidate->rssi);
    } else {
        st.bssid_set = 0;
        LOGF("wifi_manager: connect SSID-only to \"%s\"\n", g_wifi.cfg.ssid);
    }

    wifi_station_set_auto_connect(false);
    wifi_station_set_reconnect_policy(false);
    wifi_set_sleep_type(NONE_SLEEP_T);

    wifi_station_disconnect();
    while (wifi_station_get_connect_status() == STATION_CONNECTING) {
        vTaskDelay(100 / portTICK_RATE_MS);
    }
    LOGF("wifi_manager: disconnected before connect\n");
    wifi_station_set_config_current(&st);
    vTaskDelay(50 / portTICK_RATE_MS);

    wifi_station_dhcpc_stop();
    vTaskDelay(100 / portTICK_RATE_MS);
    wifi_station_dhcpc_start();

    wifi_station_connect();
}

static void start_softap(void)
{
    struct softap_config ap;

    memset(&ap, 0, sizeof(ap));

    wifi_set_opmode_current(SOFTAP_MODE);
    g_wifi.in_ap_mode = 1;
    g_wifi.connect_mode = CONNECT_MODE_NONE;

    strcpy((char *)ap.ssid, WIFI_AP_SSID);
    strcpy((char *)ap.password, WIFI_AP_PASSWORD);
    ap.ssid_len        = 0;
    ap.channel         = 1;
    ap.authmode        = AUTH_WPA_WPA2_PSK;
    ap.ssid_hidden     = 0;
    ap.max_connection  = 4;
    ap.beacon_interval = 100;

    wifi_softap_set_config_current(&ap);

    LOGF("wifi_manager: SoftAP active: SSID=%s, password=%s\n",
         WIFI_AP_SSID, WIFI_AP_PASSWORD);
}

static void handle_scan(struct bss_info *list)
{
    struct bss_info *it = list;

    g_wifi.candidate_count = 0;
    g_wifi.candidate_index = 0;

    LOGF("wifi_manager: === Scan Results ===\n");
    while (it && g_wifi.candidate_count < MAX_WIFI_CANDIDATES) {
        char ssid[33] = {0};

        /* Some ESP8266 SDK scan results expose an unusable ssid_len.
         * Copy the fixed-size SSID buffer and terminate locally instead.
         */
        memcpy(ssid, it->ssid, 32);
        ssid[32] = '\0';

        LOGF("  SSID: %-32s | BSSID: %02x:%02x:%02x:%02x:%02x:%02x | Ch:%2u | RSSI:%4d\n",
             ssid,
             it->bssid[0], it->bssid[1], it->bssid[2],
             it->bssid[3], it->bssid[4], it->bssid[5],
             it->channel,
             it->rssi);

        if (strncmp(ssid, g_wifi.cfg.ssid, sizeof(g_wifi.cfg.ssid)) == 0) {
            memcpy(g_wifi.candidates[g_wifi.candidate_count].bssid, it->bssid, 6);
            g_wifi.candidates[g_wifi.candidate_count].channel = it->channel;
            g_wifi.candidates[g_wifi.candidate_count].rssi = it->rssi;
            g_wifi.candidate_count++;
        }

        it = STAILQ_NEXT(it, next);
    }

    LOGF("wifi_manager: scan candidates=%d\n", g_wifi.candidate_count);

    for (int i = 0; i < g_wifi.candidate_count - 1; ++i) {
        for (int j = i + 1; j < g_wifi.candidate_count; ++j) {
            if (g_wifi.candidates[j].rssi > g_wifi.candidates[i].rssi) {
                bss_candidate_t tmp = g_wifi.candidates[i];
                g_wifi.candidates[i] = g_wifi.candidates[j];
                g_wifi.candidates[j] = tmp;
            }
        }
    }
    for (int i = 0; i < g_wifi.candidate_count; ++i) {
        LOGF("candidate[%d] rssi=%d ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
            i,
            g_wifi.candidates[i].rssi,
            g_wifi.candidates[i].channel,
            g_wifi.candidates[i].bssid[0],
            g_wifi.candidates[i].bssid[1],
            g_wifi.candidates[i].bssid[2],
            g_wifi.candidates[i].bssid[3],
            g_wifi.candidates[i].bssid[4],
            g_wifi.candidates[i].bssid[5]);
    }
}

static void handle_disconnect(uint8_t reason)
{
    g_wifi.has_ip = 0;
    g_wifi.ip = 0;
    g_wifi.last_reason = reason;
    g_wifi.consecutive_failures++;
    g_wifi.retry_count++;

    LOGF("wifi_manager: handle disconnect reason=%u mode=%d retry=%d candidate=%d/%d failures=%d\n",
         reason,
         (int)g_wifi.connect_mode,
         g_wifi.retry_count,
         g_wifi.candidate_index + 1,
         g_wifi.candidate_count,
         g_wifi.consecutive_failures);

    switch (reason) {
    case REASON_AUTH_EXPIRE:
    case REASON_ASSOC_EXPIRE:
    case REASON_4WAY_HANDSHAKE_TIMEOUT:
        if (g_wifi.connect_mode == CONNECT_MODE_SSID && g_wifi.candidate_count > 0) {
            g_wifi.candidate_index = 0;
            wifi_transition(WIFI_CONNECT_BSSID);
            return;
        }

        if (g_wifi.connect_mode == CONNECT_MODE_BSSID &&
            g_wifi.candidate_index + 1 < g_wifi.candidate_count) {
            g_wifi.candidate_index++;
            wifi_transition(WIFI_CONNECT_BSSID);
            return;
        }

        if (g_wifi.retry_count < WIFI_MAX_RETRIES) {
            wifi_transition(WIFI_SCAN);
        } else {
            wifi_transition(WIFI_BACKOFF);
        }
        return;

    case REASON_NO_AP_FOUND:
    case REASON_BEACON_TIMEOUT:
    case REASON_ASSOC_LEAVE:
            wifi_transition(WIFI_BACKOFF);
        return;

    default:
        if (g_wifi.retry_count >= WIFI_MAX_RETRIES) {
            wifi_transition(WIFI_BACKOFF);
        } else {
            wifi_transition(WIFI_SCAN);
        }
        return;
    }
}

static void wifi_process_event(const wifi_evt_t *evt)
{
    if (!evt) {
        return;
    }

    switch (evt->type) {
    case WIFI_EVT_GOT_IP: {
        app_evt_t app_evt;

        g_wifi.has_ip = 1;
        g_wifi.ip = evt->data.got_ip.ip;
        g_wifi.retry_count = 0;
        g_wifi.backoff_level = 0;
        g_wifi.consecutive_failures = 0;
        g_wifi.recovery_reset_count = 0;
        g_wifi.last_reason = 0;

        app_evt.type = APP_EVT_WIFI_UP;
        app_evt.data.wifi_up.ip = evt->data.got_ip.ip;
        send_app_event(&app_evt);

        wifi_transition(WIFI_CONNECTED);
        break;
    }

    case WIFI_EVT_DISCONNECTED: {
        if (g_wifi.state != WIFI_WAIT_IP &&
            g_wifi.state != WIFI_CONNECTED &&
            g_wifi.state != WIFI_CONNECT_SSID &&
            g_wifi.state != WIFI_CONNECT_BSSID) {
            LOGF("wifi_manager: ignoring stale DISCONNECTED reason=%u in state=%s\n",
                evt->data.disconnect.reason,
                wifi_state_name(g_wifi.state));
            break;
        }
            handle_disconnect(evt->data.disconnect.reason);
        break;
    }

    case WIFI_EVT_SCAN_DONE:
        g_wifi.scan_in_progress = 0;

        if (evt->data.scan.status != OK || !evt->data.scan.list) {
            LOGF("wifi_manager: scan failed status=%d\n", evt->data.scan.status);
            g_wifi.retry_count++;
            wifi_transition(WIFI_BACKOFF);
            return;
        }

        handle_scan(evt->data.scan.list);

        if (g_wifi.candidate_count == 0) {
            wifi_transition(WIFI_AP_MODE);
        } else {
            wifi_transition(WIFI_SELECT_BEST_AP);
        }
        break;

    default:
        break;
    }
}

/* ===== Public API ===== */
void wifi_manager_init(const wifi_config_t *initial_cfg)
{
    memset(&g_wifi, 0, sizeof(g_wifi));

    if (initial_cfg) {
        g_wifi.cfg = *initial_cfg;
    }

    wifi_set_sleep_type(NONE_SLEEP_T);

    if (!g_cfg_update_queue) {
        g_cfg_update_queue = xQueueCreate(4, sizeof(wifi_cfg_msg_t));
    }
    if (!g_wifi_evt_queue) {
        g_wifi_evt_queue = xQueueCreate(WIFI_EVT_QUEUE_LEN, sizeof(wifi_evt_t));
    }

    wifi_set_event_handler_cb(wifi_event_handler);

    g_wifi.last_action_tick = wifi_now_ticks();
    g_wifi.state_enter_tick = g_wifi.last_action_tick;
    wifi_transition(WIFI_BOOT);

    xTaskCreate(wifi_task, "wifi_task", 512, NULL, 3, NULL);
}

void wifi_manager_update_config(const wifi_config_t *new_cfg)
{
    wifi_cfg_msg_t msg;

    if (!new_cfg) {
        return;
    }

    msg.wifi = *new_cfg;
    if (g_cfg_update_queue) {
        xQueueSend(g_cfg_update_queue, &msg, 0);
    }
}

int wifi_manager_has_ip(void)
{
    return g_wifi.has_ip;
}

uint32_t wifi_manager_get_ip(void)
{
    return g_wifi.ip;
}

/* ===== WiFi event handler (SDK callback context) ===== */
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt)
{
    wifi_evt_t msg;

    if (!evt || !g_wifi_evt_queue) {
        return;
    }

    memset(&msg, 0, sizeof(msg));

    switch (evt->event_id) {
    case EVENT_STAMODE_GOT_IP:
        msg.type = WIFI_EVT_GOT_IP;
        msg.data.got_ip.ip = evt->event_info.got_ip.ip.addr;
        xQueueSend(g_wifi_evt_queue, &msg, 0);
        LOGF("wifi_manager: queued GOT_IP event\n");
        break;

    case EVENT_STAMODE_DISCONNECTED:
        msg.type = WIFI_EVT_DISCONNECTED;
        msg.data.disconnect.reason = evt->event_info.disconnected.reason;
        xQueueSend(g_wifi_evt_queue, &msg, 0);
        LOGF("wifi_manager: queued DISCONNECTED reason=%u\n",
             evt->event_info.disconnected.reason);
        break;

    case EVENT_SOFTAPMODE_STACONNECTED:
        LOGF("wifi_manager: AP client connected (heap=%u)\n",
             (unsigned)system_get_free_heap_size());
        break;

    case EVENT_SOFTAPMODE_STADISCONNECTED:
        LOGF("wifi_manager: AP client disconnected (heap=%u)\n",
             (unsigned)system_get_free_heap_size());
        break;

    default:
        break;
    }
}

/* ===== Scan callback ===== */
static void scan_done_cb(void *arg, STATUS status)
{
    wifi_evt_t msg;

    if (!g_wifi_evt_queue) {
        return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.type = WIFI_EVT_SCAN_DONE;
    msg.data.scan.status = status;
    msg.data.scan.list = (struct bss_info *)arg;

    xQueueSend(g_wifi_evt_queue, &msg, 0);
}

static void state_init(void)
{
    if (!g_wifi.state_started) {
        g_wifi.state_started = 1;
        g_wifi.has_ip = 0;
        g_wifi.ip = 0;
        g_wifi.connect_mode = CONNECT_MODE_NONE;
        g_wifi.scan_in_progress = 0;
        g_wifi.candidate_count = 0;
        g_wifi.candidate_index = 0;
        g_wifi.stuck_counter = 0;
        g_wifi.in_ap_mode = 0;

        wifi_station_disconnect();
        while (wifi_station_get_connect_status() == STATION_CONNECTING) {
            vTaskDelay(100 / portTICK_RATE_MS);
        }
        LOGF("wifi_manager: disconnected before init\n");
        wifi_set_opmode_current(STATION_MODE);
        wifi_station_set_auto_connect(false);
        wifi_station_set_reconnect_policy(false);
        wifi_set_sleep_type(NONE_SLEEP_T);
        wifi_mark_action();
    }

    if (!wifi_gap_ready()) {
        return;
    }

    if (g_wifi.cfg.ssid[0] == '\0') {
        wifi_transition(WIFI_AP_MODE);
    } else {
        wifi_transition(WIFI_SCAN);
    }
}

static void state_scan(void)
{
    struct scan_config sc;

    if (g_wifi.scan_in_progress) {
        return;
    }
    if (!g_wifi.state_started && !wifi_gap_ready()) {
        return;
    }
    if (g_wifi.state_started) {
        return;
    }

    memset(&sc, 0, sizeof(sc));

    g_wifi.state_started = 1;
    g_wifi.connect_mode = CONNECT_MODE_NONE;
    g_wifi.candidate_count = 0;
    g_wifi.candidate_index = 0;
    g_wifi.scan_in_progress = 1;

    LOGF("wifi_manager: starting scan for SSID \"%s\"\n", g_wifi.cfg.ssid);

    wifi_mark_action();
    wifi_station_disconnect();
    while (wifi_station_get_connect_status() == STATION_CONNECTING) {
        vTaskDelay(100 / portTICK_RATE_MS);
    }
    LOGF("wifi_manager: disconnected before scan\n");

    wifi_set_opmode_current(STATION_MODE);
    g_wifi.in_ap_mode = 0;

    sc.show_hidden = 1;

    if (!wifi_station_scan(&sc, scan_done_cb)) {
        LOGF("wifi_manager: wifi_station_scan() call failed\n");
        g_wifi.scan_in_progress = 0;
        g_wifi.retry_count++;
        wifi_transition(WIFI_BACKOFF);
    }
}

static void state_select_best_ap(void)
{
    if (!g_wifi.state_started) {
        g_wifi.state_started = 1;
        g_wifi.candidate_index = 0;
    }

    wifi_transition(WIFI_CONNECT_BSSID);
}

static void state_connect_ssid(void)
{
    if (!wifi_gap_ready()) {
        return;
    }

    g_wifi.connect_mode = CONNECT_MODE_SSID;
    g_wifi.connect_tick = wifi_now_ticks();
    g_wifi.state_started = 1;

    wifi_mark_action();
    start_station(NULL);
    wifi_transition(WIFI_WAIT_IP);
}

static void state_connect_bssid(void)
{
    if (!wifi_gap_ready()) {
        return;
    }

    if (g_wifi.candidate_count == 0 || g_wifi.candidate_index >= g_wifi.candidate_count) {
        wifi_transition(WIFI_SCAN);
        return;
    }

    g_wifi.connect_mode = CONNECT_MODE_BSSID;
    g_wifi.connect_tick = wifi_now_ticks();
    g_wifi.state_started = 1;

    wifi_mark_action();
    start_station(&g_wifi.candidates[g_wifi.candidate_index]);
    wifi_transition(WIFI_WAIT_IP);
}

static void state_wait_ip(void)
{
    if (wifi_elapsed_ms(g_wifi.connect_tick) > DHCP_IP_TIMEOUT_MS) {
        STATION_STATUS st = wifi_station_get_connect_status();

        LOGF("wifi_manager: DHCP/IP timeout in mode=%d status=%d\n",
             (int)g_wifi.connect_mode, st);

        wifi_flush_event_queue();

        wifi_station_dhcpc_stop();
        wifi_station_disconnect();

        while (wifi_station_get_connect_status() == STATION_CONNECTING) {
            vTaskDelay(100 / portTICK_RATE_MS);
        }

        vTaskDelay(500 / portTICK_RATE_MS);

        handle_disconnect(REASON_BEACON_TIMEOUT);
    }
}

static void state_connected(void)
{
}

static void state_backoff(void)
{
    if (!g_wifi.state_started) {
        g_wifi.state_started = 1;
        g_wifi.backoff_level++;
        LOGF("wifi_manager: backoff level=%d delay=%u ms\n",
             g_wifi.backoff_level,
             (unsigned)wifi_backoff_delay_ms());
    }

    if (wifi_elapsed_ms(g_wifi.state_enter_tick) < wifi_backoff_delay_ms()) {
        return;
    }

    g_wifi.retry_count = 0;
    if (g_wifi.backoff_level > WIFI_MAX_BACKOFF_LEVEL) {
        wifi_transition(WIFI_RECOVERY_RESET);
    } else {
        wifi_transition(WIFI_SCAN);
    }
}

static void state_recovery_reset(void)
{
    if (!g_wifi.state_started) {
        if (!wifi_gap_ready()) {
            return;
        }

        g_wifi.state_started = 1;
        g_wifi.recovery_reset_count++;
        g_wifi.connect_mode = CONNECT_MODE_NONE;
        g_wifi.scan_in_progress = 0;
        g_wifi.candidate_count = 0;
        g_wifi.candidate_index = 0;
        g_wifi.retry_count = 0;

        LOGF("wifi_manager: recovery reset #%d\n", g_wifi.recovery_reset_count);

        wifi_flush_event_queue();
        wifi_mark_action();
        wifi_station_disconnect();

        while (wifi_station_get_connect_status() == STATION_CONNECTING) {
            vTaskDelay(100 / portTICK_RATE_MS);
        }
        LOGF("wifi_manager: disconnected before recovery reset\n");

        wifi_set_opmode_current(STATION_MODE);
        wifi_station_set_auto_connect(false);
        wifi_station_set_reconnect_policy(false);
        wifi_set_sleep_type(NONE_SLEEP_T);
        vTaskDelay(1000 / portTICK_RATE_MS);

        wifi_station_dhcpc_stop();
        vTaskDelay(100 / portTICK_RATE_MS);
        wifi_station_dhcpc_start();

        wifi_mark_action();
    }

    if (g_wifi.recovery_reset_count > WIFI_MAX_RECOVERY_RESETS ||
        g_wifi.consecutive_failures >= WIFI_MAX_FAILURES_BEFORE_AP) {
        wifi_transition(WIFI_AP_MODE);
    } else if (g_wifi.cfg.ssid[0] == '\0') {
        wifi_transition(WIFI_AP_MODE);
    } else {
        wifi_transition(WIFI_SCAN);
    }
}

static void state_ap_mode(void)
{
    if (!g_wifi.state_started) {
        if (!wifi_gap_ready()) {
            return;
        }

        g_wifi.state_started = 1;
        g_wifi.has_ip = 0;
        g_wifi.ip = 0;
        g_wifi.connect_mode = CONNECT_MODE_NONE;

        wifi_mark_action();
        start_softap();
    }
}

static void wifi_run_state(void)
{
    switch (g_wifi.state) {
    case WIFI_BOOT:
        wifi_transition(WIFI_INIT);
        break;

    case WIFI_INIT:
        state_init();
        break;

    case WIFI_SCAN:
        state_scan();
        break;

    case WIFI_SELECT_BEST_AP:
        state_select_best_ap();
        break;

    case WIFI_CONNECT_SSID:
        state_connect_ssid();
        break;

    case WIFI_CONNECT_BSSID:
        state_connect_bssid();
        break;

    case WIFI_WAIT_IP:
        state_wait_ip();
        break;

    case WIFI_CONNECTED:
        state_connected();
        break;

    case WIFI_BACKOFF:
        state_backoff();
        break;

    case WIFI_RECOVERY_RESET:
        state_recovery_reset();
        break;

    case WIFI_AP_MODE:
        state_ap_mode();
        break;

    default:
        wifi_transition(WIFI_RECOVERY_RESET);
        break;
    }
}

/* ===== WiFi management task ===== */
static void wifi_task(void *pv)
{
    uint32_t loop_cnt = 0;
    wifi_evt_t evt;

    (void)pv;

    for (;;) {
        if (g_cfg_update_queue) {
            wifi_cfg_msg_t upd;

            if (xQueueReceive(g_cfg_update_queue, &upd, 0)) {
                g_wifi.cfg = upd.wifi;
                g_wifi.backoff_level = 0;
                g_wifi.retry_count = 0;
                g_wifi.recovery_reset_count = 0;
                g_wifi.consecutive_failures = 0;

                LOGF("wifi_manager: new WiFi config received\n");
                wifi_flush_event_queue();
                if (g_wifi.cfg.ssid[0] == '\0') {
                    wifi_transition(WIFI_AP_MODE);
                } else {
                    wifi_transition(WIFI_RECOVERY_RESET);
                }
            }
        }

        while (g_wifi_evt_queue && xQueueReceive(g_wifi_evt_queue, &evt, 0) == pdTRUE) {
            wifi_process_event(&evt);
        }

        wifi_run_state();

        if (g_wifi.state != WIFI_CONNECTED &&
            g_wifi.state != WIFI_AP_MODE &&
            wifi_elapsed_ms(g_wifi.state_enter_tick) > WIFI_STUCK_TIMEOUT_MS) {
            g_wifi.stuck_counter++;
            LOGF("wifi_manager: state %s stuck for %u ms, recovery reset\n",
                 wifi_state_name(g_wifi.state),
                 (unsigned)wifi_elapsed_ms(g_wifi.state_enter_tick));
            wifi_transition(WIFI_RECOVERY_RESET);
        }

        if ((++loop_cnt & 0x3F) == 0) {
            LOG_STACK("wifi_manager");
        }
        vTaskDelay(WIFI_TASK_TICK_MS / portTICK_RATE_MS);
    }
}