#include "wifi_manager.h"

#include "esp_common.h"
#include "espressif/esp_wifi.h"
#include "espressif/esp_sta.h"   // wifi_station_scan, struct bss_info, struct scan_config
#include "lwip/ip_addr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "queue.h"               // STAILQ_HEAD, STAILQ_FOREACH

#include "app_events.h"

#include <string.h>
#include <stdio.h>

/* ==== Tunables ==== */
#define WIFI_MAX_RETRIES         5
#define DHCP_IP_TIMEOUT_MS       20000
#define MAX_WIFI_CANDIDATES      10

#define WIFI_AP_SSID             "ESP8266-Setup"
#define WIFI_AP_PASSWORD         "esp8266pw"

/* Common disconnect reasons we care about (Non-OS SDK typically uses these numbers) */
#define REASON_AUTH_EXPIRE               2
#define REASON_ASSOC_EXPIRE              4
#define REASON_ASSOC_LEAVE               8
#define REASON_4WAY_HANDSHAKE_TIMEOUT   15
/* These vary across SDKs; keep for best-effort */
#define REASON_BEACON_TIMEOUT          200
#define REASON_NO_AP_FOUND             201

/* ==== State ==== */
static wifi_config_t g_cfg;
static int       g_sta_retry     = 0;
static int       g_has_ip        = 0;
static uint32_t  g_current_ip    = 0;

static volatile int g_want_ap           = 0;
static volatile int g_want_sta_scan     = 0;
static volatile int g_scan_in_progress  = 0;
static volatile int g_request_connect   = 0;
static volatile int g_in_ap_mode        = 0;

static xQueueHandle g_cfg_update_queue = NULL;
typedef struct { wifi_config_t wifi; } wifi_cfg_msg_t;

/* Candidate list from last scan */
typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
} bss_candidate_t;

static bss_candidate_t g_candidates[MAX_WIFI_CANDIDATES];
static int             g_candidates_count = 0;
static int             g_candidate_index  = 0;

static int             g_waiting_ip       = 0;
static uint32_t        g_connect_start_tick = 0;

/* Connection attempt phase:
 * 0 = none/idle
 * 1 = trying SSID-only
 * 2 = trying BSSID candidates
 */
static int             g_attempt_phase    = 0;

/* Flags set by event handler; consumed by wifi_task */
static volatile uint8_t g_last_disc_reason = 0;
static volatile int     g_evt_force_ssid_only = 0;
static volatile int     g_evt_advance_candidate = 0;
static volatile int     g_evt_trigger_rescan = 0;
static volatile int     g_evt_trigger_ap = 0;

/* Forward declarations */
static void wifi_task(void *pv);
static void start_softap(void);
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt);
static void scan_done_cb(void *arg, STATUS status);

static void start_sta_with_bss(const struct bss_info *best);
static void connect_ssid_only(void);
static void connect_current_candidate(void);
static void advance_candidate_or_ap(const char *reason_tag);

/* ==== Helpers ==== */
static void send_app_event(const app_evt_t *ev)
{
    if (g_app_event_queue) {
        xQueueSend(g_app_event_queue, ev, 0);
    }
}

/* ===== Public API ===== */
void wifi_manager_init(const wifi_config_t *initial_cfg)
{
    if (initial_cfg) g_cfg = *initial_cfg;
    else memset(&g_cfg, 0, sizeof(g_cfg));

    /* Improve stability */
    wifi_set_sleep_type(NONE_SLEEP_T);

    g_sta_retry  = 0;
    g_has_ip     = 0;
    g_current_ip = 0;

    g_candidates_count = 0;
    g_candidate_index  = 0;
    g_waiting_ip       = 0;
    g_attempt_phase    = 0;

    g_evt_force_ssid_only   = 0;
    g_evt_advance_candidate = 0;
    g_evt_trigger_rescan    = 0;
    g_evt_trigger_ap        = 0;

    if (g_cfg.ssid[0] == '\0') {
        g_want_ap = 1;
        g_want_sta_scan = 0;
    } else {
        g_want_ap = 0;
        g_want_sta_scan = 1;
    }

    g_scan_in_progress = 0;

    wifi_set_event_handler_cb(wifi_event_handler);

    if (!g_cfg_update_queue) {
        g_cfg_update_queue = xQueueCreate(4, sizeof(wifi_cfg_msg_t));
    }

    xTaskCreate(wifi_task, "wifi_task", 512, NULL, 3, NULL);
}

void wifi_manager_update_config(const wifi_config_t *new_cfg)
{
    if (!new_cfg) return;
    wifi_cfg_msg_t m; m.wifi = *new_cfg;
    if (g_cfg_update_queue) {
        xQueueSend(g_cfg_update_queue, &m, 0);
    }
}

int wifi_manager_has_ip(void) { return g_has_ip; }
uint32_t wifi_manager_get_ip(void) { return g_current_ip; }

/* ===== STA connect helpers ===== */
static void start_sta_with_bss(const struct bss_info *best)
{
    struct station_config st;
    memset(&st, 0, sizeof(st));

    wifi_set_opmode_current(STATION_MODE);
    g_in_ap_mode = 0;

    strncpy((char*)st.ssid, g_cfg.ssid, sizeof(st.ssid) - 1);
    st.ssid[sizeof(st.ssid) - 1] = 0;

    strncpy((char*)st.password, g_cfg.password, sizeof(st.password) - 1);
    st.password[sizeof(st.password) - 1] = 0;
    LOGF("wifi_manager: prepared station config for SSID \"%s\"\n", st.ssid);
    LOGF("wifi_manager: prepared station config with password \"%s\"\n", st.password[0] ? (char*)st.password : "(empty)");

    if (best) {
        memcpy(st.bssid, best->bssid, sizeof(st.bssid));
        st.bssid_set = 1;
        LOGF("wifi_manager: using BSSID %02x:%02x:%02x:%02x:%02x:%02x ch=%d\n",
             best->bssid[0], best->bssid[1], best->bssid[2],
             best->bssid[3], best->bssid[4], best->bssid[5],
             best->channel);
    } else {
        st.bssid_set = 0;
        LOGF("wifi_manager: connecting by SSID only\n");
    }

    /* Make join deterministic: we manage retries, not SDK auto policies */
    wifi_station_set_auto_connect(false);
    wifi_station_set_reconnect_policy(false);
    wifi_set_sleep_type(NONE_SLEEP_T);

    wifi_station_disconnect();
    vTaskDelay(100 / portTICK_RATE_MS);

    wifi_station_set_config(&st);
    vTaskDelay(20 / portTICK_RATE_MS);

    wifi_station_connect();

    LOGF("wifi_manager: STA connecting to \"%s\"\n", g_cfg.ssid);
    g_waiting_ip = 1;
    g_connect_start_tick = xTaskGetTickCount();
}

static void connect_ssid_only(void)
{
    g_attempt_phase = 1;
    start_sta_with_bss(NULL);
}

static void connect_current_candidate(void)
{
    if (g_candidates_count == 0 || g_candidate_index >= g_candidates_count) {
        LOGF("wifi_manager: no candidates to connect\n");
        return;
    }
    struct bss_info b;
    memset(&b, 0, sizeof(b));
    memcpy(b.bssid, g_candidates[g_candidate_index].bssid, sizeof(b.bssid));
    b.channel = g_candidates[g_candidate_index].channel;

    g_attempt_phase = 2;
    start_sta_with_bss(&b);
}

/* Try next step after a failure:
 * - If we were SSID-only: start candidate list (if any)
 * - If we were in candidates: next candidate
 * - Else: AP fallback
 */
static void advance_candidate_or_ap(const char *reason_tag)
{
    wifi_station_disconnect();

    if (g_attempt_phase == 1) {
        if (g_candidates_count > 0) {
            g_candidate_index = 0;
            LOGF("wifi_manager: SSID-only failed, trying candidate 1/%d%s%s\n",
                 g_candidates_count,
                 reason_tag ? " (" : "",
                 reason_tag ? reason_tag : "");
            connect_current_candidate();
        } else {
            LOGF("wifi_manager: no candidates; AP fallback\n");
            g_waiting_ip = 0;
            g_want_ap = 1;
        }
        return;
    }

    if (g_attempt_phase == 2) {
        if (g_candidate_index + 1 < g_candidates_count) {
            g_candidate_index++;
            LOGF("wifi_manager: switching to candidate %d/%d%s%s\n",
                 g_candidate_index + 1, g_candidates_count,
                 reason_tag ? " (" : "",
                 reason_tag ? reason_tag : "");
            connect_current_candidate();
        } else {
            LOGF("wifi_manager: all candidates tried; AP fallback\n");
            g_waiting_ip = 0;
            g_want_ap = 1;
        }
        return;
    }

    /* no phase yet -> start SSID-only */
    connect_ssid_only();
}

/* ===== SoftAP ===== */
static void start_softap(void)
{
    struct softap_config ap;
    memset(&ap, 0, sizeof(ap));

    wifi_set_opmode_current(SOFTAP_MODE);
    g_in_ap_mode = 1;

    strcpy((char*)ap.ssid, WIFI_AP_SSID);
    strcpy((char*)ap.password, WIFI_AP_PASSWORD);
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

/* ===== WiFi event handler (single) ===== */
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt)
{
    if (!evt) return;

    switch (evt->event_id) {

    case EVENT_STAMODE_GOT_IP: {
        g_sta_retry  = 0;
        g_has_ip     = 1;
        g_current_ip = evt->event_info.got_ip.ip.addr;
        g_waiting_ip = 0;

        app_evt_t ev;
        ev.type = APP_EVT_WIFI_UP;
        ev.data.wifi_up.ip = g_current_ip;
        send_app_event(&ev);

        LOGF("wifi_manager: Got IP: " IPSTR "\n", IP2STR(&evt->event_info.got_ip.ip));
        break;
    }

    case EVENT_STAMODE_DISCONNECTED: {
        uint8_t r = evt->event_info.disconnected.reason;
        g_last_disc_reason = r;

        g_has_ip = 0;
        g_current_ip = 0;

        app_evt_t ev;
        ev.type = APP_EVT_WIFI_DOWN;
        send_app_event(&ev);

        if (g_in_ap_mode) break;

        LOGF("wifi_manager: Disconnected, reason=%u, waiting_ip=%d, phase=%d\n",
             r, g_waiting_ip, g_attempt_phase);

        /* If we were joining, handle hard failures immediately */
        if (g_waiting_ip) {
            if (r == REASON_AUTH_EXPIRE || r == REASON_ASSOC_EXPIRE || r == REASON_4WAY_HANDSHAKE_TIMEOUT) {
                g_waiting_ip = 0;
                /* Prefer SSID-only once if we were trying candidates */
                if (g_attempt_phase == 2) {
                    g_evt_force_ssid_only = 1;
                } else {
                    g_evt_advance_candidate = 1;
                }
                break;
            }

            if (r == REASON_NO_AP_FOUND || r == REASON_BEACON_TIMEOUT) {
                g_waiting_ip = 0;
                g_evt_trigger_rescan = 1;
                break;
            }
        }

        /* General retry policy */
        if (g_sta_retry < WIFI_MAX_RETRIES) {
            g_sta_retry++;
            g_evt_trigger_rescan = 1;
        } else {
            g_evt_trigger_ap = 1;
        }
        break;
    }

    case EVENT_SOFTAPMODE_STACONNECTED:
        LOGF("wifi_manager: AP client connected (heap=%u)\n", (unsigned)system_get_free_heap_size());
        break;

    case EVENT_SOFTAPMODE_STADISCONNECTED:
        LOGF("wifi_manager: AP client disconnected (heap=%u)\n", (unsigned)system_get_free_heap_size());
        break;

    default:
        break;
    }
}

/* ===== Scan callback ===== */
static void scan_done_cb(void *arg, STATUS status)
{
    g_scan_in_progress = 0;

    if (status != OK) {
        LOGF("wifi_manager: scan failed (status=%d)\n", status);
        g_want_ap = 1;
        return;
    }
    if (!arg) {
        LOGF("wifi_manager: scan done, but arg == NULL\n");
        g_want_ap = 1;
        return;
    }

    typedef STAILQ_HEAD(bss_head, bss_info) bss_head_t;
    bss_head_t *head = (bss_head_t *)arg;
    struct bss_info *bss = NULL;

    g_candidates_count = 0;
    g_candidate_index  = 0;
    g_attempt_phase    = 0;

    LOGF("wifi_manager: === Scan Results ===\n");
    STAILQ_FOREACH(bss, head, next) {
        LOGF("  SSID: %-32s | BSSID: %02x:%02x:%02x:%02x:%02x:%02x | Ch:%2u | RSSI:%4d | Auth:%u\n",
             bss->ssid,
             bss->bssid[0], bss->bssid[1], bss->bssid[2],
             bss->bssid[3], bss->bssid[4], bss->bssid[5],
             bss->channel, bss->rssi, bss->authmode);
    }
    LOGF("wifi_manager: === End Scan ===\n");

    /* collect matching SSID */
    STAILQ_FOREACH(bss, head, next) {
        if (strcmp((char*)bss->ssid, g_cfg.ssid) == 0) {
            if (g_candidates_count < MAX_WIFI_CANDIDATES) {
                memcpy(g_candidates[g_candidates_count].bssid, bss->bssid, 6);
                g_candidates[g_candidates_count].channel = bss->channel;
                g_candidates[g_candidates_count].rssi    = bss->rssi;
                g_candidates_count++;
            }
        }
    }

    if (g_candidates_count == 0) {
        LOGF("wifi_manager: SSID \"%s\" not found -> AP fallback\n", g_cfg.ssid);
        g_want_ap = 1;
        return;
    }

    /* sort by RSSI desc */
    for (int i = 0; i < g_candidates_count - 1; ++i) {
        for (int j = i + 1; j < g_candidates_count; ++j) {
            if (g_candidates[j].rssi > g_candidates[i].rssi) {
                bss_candidate_t tmp = g_candidates[i];
                g_candidates[i] = g_candidates[j];
                g_candidates[j] = tmp;
            }
        }
    }

    LOGF("wifi_manager: scan done, %d candidate(s) for SSID \"%s\"\n",
         g_candidates_count, g_cfg.ssid);
    for (int i = 0; i < g_candidates_count; ++i) {
        LOGF("  cand %d: %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d\n",
             i + 1,
             g_candidates[i].bssid[0], g_candidates[i].bssid[1], g_candidates[i].bssid[2],
             g_candidates[i].bssid[3], g_candidates[i].bssid[4], g_candidates[i].bssid[5],
             g_candidates[i].channel,
             g_candidates[i].rssi);
    }

    g_sta_retry = 0;
    g_request_connect = 1; /* let wifi_task initiate connection */
}

/* ===== WiFi management task ===== */
static void wifi_task(void *pv)
{
    (void)pv;
    uint32_t loop_cnt = 0;

    for (;;) {

        /* Apply config updates */
        if (g_cfg_update_queue) {
            wifi_cfg_msg_t upd;
            if (xQueueReceive(g_cfg_update_queue, &upd, 0)) {
                g_cfg = upd.wifi;
                LOGF("wifi_manager: new WiFi config received, scheduling scan\n");
                g_want_ap = 0;
                g_want_sta_scan = 1;
                g_sta_retry = 0;
            }
        }

        /* Consume event-handler flags safely in task context */
        if (g_evt_trigger_ap) {
            g_evt_trigger_ap = 0;
            g_want_ap = 1;
        }
        if (g_evt_trigger_rescan) {
            g_evt_trigger_rescan = 0;
            g_want_sta_scan = 1;
        }
        if (g_evt_force_ssid_only) {
            g_evt_force_ssid_only = 0;
            /* schedule SSID-only connect attempt */
            if (!g_has_ip && !g_waiting_ip && g_cfg.ssid[0] != '\0') {
                g_request_connect = 1;
                g_attempt_phase = 0; /* will be set by connect_ssid_only() */
                /* mark that connect should be SSID-only even if candidates exist */
                g_candidate_index = 0;
                /* We’ll just call connect_ssid_only() when g_request_connect is handled */
                g_last_disc_reason = g_last_disc_reason; /* no-op; keep reason for logs */
                /* Use a tiny delay to let WiFi stack settle */
                vTaskDelay(50 / portTICK_RATE_MS);
                connect_ssid_only();
            }
        }
        if (g_evt_advance_candidate) {
            g_evt_advance_candidate = 0;
            if (!g_has_ip && g_cfg.ssid[0] != '\0') {
                advance_candidate_or_ap("disc");
            }
        }

        /* AP mode request */
        if (g_want_ap) {
            g_want_ap = 0;
            if (!g_in_ap_mode) {
                LOGF("wifi_manager: switching to AP mode\n");
                start_softap();
            } else {
                LOGF("wifi_manager: already in AP mode\n");
            }
            g_want_sta_scan = 0;
            g_request_connect = 0;
            g_waiting_ip = 0;
        }

        /* Start scan */
        if (g_want_sta_scan && !g_scan_in_progress && g_cfg.ssid[0] != '\0') {
            g_want_sta_scan = 0;
            g_scan_in_progress = 1;

            LOGF("wifi_manager: starting scan for SSID \"%s\"\n", g_cfg.ssid);

            wifi_set_opmode_current(STATION_MODE);
            g_in_ap_mode = 0;

            struct scan_config sc;
            memset(&sc, 0, sizeof(sc));
            sc.ssid = NULL;
            sc.bssid = NULL;
            sc.channel = 0;      /* all channels */
            sc.show_hidden = 0;

            /* Don’t scan while actively waiting for IP; it disrupts join */
            if (g_waiting_ip) {
                LOGF("wifi_manager: scan skipped (joining in progress)\n");
                g_scan_in_progress = 0;
            } else if (!wifi_station_scan(&sc, scan_done_cb)) {
                LOGF("wifi_manager: wifi_station_scan() call failed\n");
                g_scan_in_progress = 0;
                g_want_ap = 1;
            }
        }

        /* After scan, start connection. Strategy:
         * 1) Try SSID-only once (most robust)
         * 2) If that fails, try BSSID candidates (sorted)
         */
        if (g_request_connect && !g_has_ip && !g_waiting_ip && g_cfg.ssid[0] != '\0') {
            g_request_connect = 0;

            g_candidate_index = 0;
            g_attempt_phase = 1;
            connect_ssid_only();
        }

        /* DHCP timeout while waiting for IP -> advance */
        if (g_waiting_ip && !g_has_ip) {
            uint32_t now = xTaskGetTickCount();
            if ((now - g_connect_start_tick) * portTICK_RATE_MS >= DHCP_IP_TIMEOUT_MS) {
                LOGF("wifi_manager: DHCP timeout (phase=%d), advancing\n", g_attempt_phase);
                g_waiting_ip = 0;
                advance_candidate_or_ap("dhcp-timeout");
            }
        }

        if ((++loop_cnt & 0x3F) == 0) { LOG_STACK("wifi_manager"); }
        vTaskDelay(200 / portTICK_RATE_MS);
    }
}
