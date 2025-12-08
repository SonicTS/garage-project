#include "wifi_manager.h"

#include "esp_common.h"
#include "espressif/esp_wifi.h"
#include "espressif/esp_sta.h"   // for wifi_station_scan, struct bss_info, struct scan_config
#include "lwip/ip_addr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "queue.h"   // for STAILQ_HEAD, STAILQ_FOREACH, etc.


#include "app_events.h"

#include <string.h>
#include <stdio.h>

#define WIFI_MAX_RETRIES  3
#define DHCP_IP_TIMEOUT_MS 15000
#define MAX_WIFI_CANDIDATES 10
#define WIFI_AP_SSID      "ESP8266-Setup"
#define WIFI_AP_PASSWORD  "esp8266pw"

static wifi_config_t g_cfg;
static int       g_sta_retry     = 0;
static int       g_has_ip        = 0;
static uint32_t  g_current_ip    = 0;

static volatile int g_want_ap         = 0;
static volatile int g_want_sta_scan   = 0;
static volatile int g_scan_in_progress = 0;
static volatile int g_request_connect = 0;
static volatile int g_in_ap_mode      = 0;
/* Internal queue for deferred WiFi config updates */
static xQueueHandle g_cfg_update_queue = NULL;

typedef struct { wifi_config_t wifi; } wifi_cfg_msg_t;

/* Candidate BSS list from last scan */
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
static int             g_tried_ssid_only = 0;

/* Forward declarations */
static void wifi_task(void *pv);
static void start_softap(void);
static void wifi_event_handler(System_Event_t *evt);
static void scan_done_cb(void *arg, STATUS status);
/* Local helpers to avoid duplicated candidate-switching logic */
static void connect_current_candidate(void);
static void advance_candidate_or_ap(const char *reason);

static void send_app_event(const app_evt_t *ev)
{
    if (g_app_event_queue) {
        xQueueSend(g_app_event_queue, ev, 0);
    }
}

/* ===== Public API ===== */

void wifi_manager_init(const wifi_config_t *initial_cfg)
{
    if (initial_cfg) {
        g_cfg = *initial_cfg;
    } else {
        memset(&g_cfg, 0, sizeof(g_cfg));
    }

    /* Improve scan/association stability: disable sleep and prefer 11n */
    wifi_set_sleep_type(NONE_SLEEP_T);
    wifi_set_phy_mode(PHY_MODE_11N);

    g_sta_retry  = 0;
    g_has_ip     = 0;
    g_current_ip = 0;

    if (g_cfg.ssid[0] == '\0') {
        /* No SSID configured yet -> go to AP mode for setup */
        g_want_ap = 1;
        g_want_sta_scan = 0;
    } else {
        /* SSID known -> try to connect via scan */
        g_want_ap = 0;
        g_want_sta_scan = 1;
    }

    g_scan_in_progress = 0;

    wifi_set_event_handler_cb(wifi_event_handler);

    if (!g_cfg_update_queue) {
        g_cfg_update_queue = xQueueCreate(4, sizeof(wifi_cfg_msg_t));
    }

    /* Increase stack to reduce risk of overflow during event bursts */
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

int wifi_manager_has_ip(void)
{
    return g_has_ip;
}

uint32_t wifi_manager_get_ip(void)
{
    return g_current_ip;
}

/* ===== Internal helpers ===== */

/* Called from scan_done_cb once we know which BSS to use */
static void start_sta_with_bss(const struct bss_info *best)
{
    struct station_config st;
    memset(&st, 0, sizeof(st));

    wifi_set_opmode_current(STATION_MODE);
    g_in_ap_mode = 0;


    strncpy((char *)st.ssid,     g_cfg.ssid,     sizeof(st.ssid)     - 1);
    strncpy((char *)st.password, g_cfg.password, sizeof(st.password) - 1);

    if (best) {
        /* Lock to this exact BSSID + channel */
        memcpy(st.bssid, best->bssid, sizeof(st.bssid));
        st.bssid_set = 1;
        /* In some SDKs, station_config has a .channel field.
           If yours does, set it here. If not, the BSSID lock is usually enough. */
        // st.channel = best->channel;  // Uncomment if station_config has channel member
        LOGF("wifi_manager: using BSSID %02x:%02x:%02x:%02x:%02x:%02x, channel %d\n",
               best->bssid[0], best->bssid[1], best->bssid[2],
               best->bssid[3], best->bssid[4], best->bssid[5],
               best->channel);
    } else {
        st.bssid_set = 0;
        LOGF("wifi_manager: no BSSID lock, connecting by SSID only\n");
    }

    wifi_station_set_auto_connect(false);
    wifi_station_disconnect();
    wifi_station_set_config(&st);
    wifi_station_set_reconnect_policy(true);

    wifi_station_connect();

    LOGF("wifi_manager: STA connecting to \"%s\"\n", g_cfg.ssid);
    g_waiting_ip = 1;
    g_connect_start_tick = xTaskGetTickCount();
}

/* Build bss_info from current candidate index and start STA */
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
    start_sta_with_bss(&b);
}

/* Disconnect and move to next candidate, or fall back to AP */
static void advance_candidate_or_ap(const char *reason)
{
    wifi_station_disconnect();
    if (g_candidate_index + 1 < g_candidates_count) {
        g_candidate_index++;
        LOGF("wifi_manager: switching to candidate %d/%d%s%s\n",
               g_candidate_index + 1,
               g_candidates_count,
               reason ? " (" : "",
               reason ? reason : "");
        connect_current_candidate();
    } else {
        if (!g_tried_ssid_only) {
            LOGF("wifi_manager: candidates exhausted, trying SSID-only connect%s%s\n",
                   reason ? " (" : "",
                   reason ? reason : "");
            g_tried_ssid_only = 1;
            start_sta_with_bss(NULL);
        } else {
            LOGF("wifi_manager: all candidates tried -> AP mode\n");
            g_waiting_ip = 0;
            g_want_ap = 1;
        }
    }
}

static void start_softap(void)
{
    struct softap_config ap;
    memset(&ap, 0, sizeof(ap));

    wifi_set_opmode_current(SOFTAP_MODE);
    g_in_ap_mode = 1;

    strcpy((char *)ap.ssid,     WIFI_AP_SSID);
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
        /* Service interface lifecycle is owned by its module/app_start; no calls here. */
}

/* ==== WiFi event handler ==== */

/* Define MINIMAL_WIFI_EVENTS to strip prints & extra logic inside event handler
   for isolation of crash causes. */
#ifdef MINIMAL_WIFI_EVENTS
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt)
{
    if (!evt) return;
    switch (evt->event_id) {
    case EVENT_STAMODE_GOT_IP:
        g_has_ip = 1;
        g_current_ip = evt->event_info.got_ip.ip.addr;
        g_waiting_ip = 0;
        if (g_app_event_queue) {
            app_evt_t ev; ev.type = APP_EVT_WIFI_UP; ev.data.wifi_up.ip = g_current_ip; xQueueSend(g_app_event_queue, &ev, 0);
        }
        break;
    case EVENT_STAMODE_DISCONNECTED:
        g_has_ip = 0; g_current_ip = 0;
        if (g_app_event_queue) { app_evt_t ev; ev.type = APP_EVT_WIFI_DOWN; xQueueSend(g_app_event_queue, &ev, 0); }
        if (!g_in_ap_mode) { g_want_sta_scan = 1; }
        break;
    default:
        /* SoftAP events ignored in minimal mode */
        break;
    }
}
#else
static void ICACHE_FLASH_ATTR wifi_event_handler(System_Event_t *evt)
{
    app_evt_t ev;

    switch (evt->event_id) {

    case EVENT_STAMODE_GOT_IP:
        LOGF("wifi_manager: Got IP: " IPSTR "\n",
               IP2STR(&evt->event_info.got_ip.ip));

        g_sta_retry  = 0;
        g_has_ip     = 1;
        g_current_ip = evt->event_info.got_ip.ip.addr;
        g_waiting_ip = 0;

        ev.type = APP_EVT_WIFI_UP;
        ev.data.wifi_up.ip = g_current_ip;
        send_app_event(&ev);
        break;

    case EVENT_STAMODE_DISCONNECTED:
        LOGF("wifi_manager: Disconnected, retry=%d\n", g_sta_retry);
        g_has_ip     = 0;
        g_current_ip = 0;

        ev.type = APP_EVT_WIFI_DOWN;
        send_app_event(&ev);

        /* If we were waiting for IP, do not switch candidates here.
           Let the DHCP timeout decide; station reconnect policy may retry.
           Also skip rescan requests while in AP mode. */
        if (!(g_waiting_ip && g_candidates_count > 0) && !g_in_ap_mode) {
            if (g_sta_retry < WIFI_MAX_RETRIES) {
                g_sta_retry++;
                LOGF("wifi_manager: requesting re-scan for SSID \"%s\"\n", g_cfg.ssid);
                g_want_sta_scan = 1;
            } else {
                LOGF("wifi_manager: STA failed, requesting AP mode\n");
                g_want_ap = 1;
            }
        }
        break;

    default:
        if (evt->event_id == EVENT_SOFTAPMODE_STACONNECTED) {
            LOGF("wifi_manager: AP client connected (heap=%u)\n", (unsigned)system_get_free_heap_size());
        } else if (evt->event_id == EVENT_SOFTAPMODE_STADISCONNECTED) {
            LOGF("wifi_manager: AP client disconnected (heap=%u)\n", (unsigned)system_get_free_heap_size());
        }
        break;
    }
}
#endif /* MINIMAL_WIFI_EVENTS */

/* ==== Scan callback ==== */
/* NOTE: 'arg' is a pointer to a linked list of struct bss_info.
 * Depending on your SDK, you might need to iterate with a macro like:
 *   STAILQ_FOREACH(bss, (STAILQ_HEAD(, bss_info) *)arg, next)
 */
static void scan_done_cb(void *arg, STATUS status)
{
    g_scan_in_progress = 0;

    if (status != OK) {
        LOGF("wifi_manager: scan failed (status=%d)\n", status);
        /* If scan failed, fall back to AP */
        g_want_ap = 1;
        return;
    }

    if (!arg) {
        LOGF("wifi_manager: scan done, but arg == NULL\n");
        g_want_ap = 1;
        return;
    }

    /* 'arg' is actually an STAILQ_HEAD for struct bss_info */
    typedef STAILQ_HEAD(bss_head, bss_info) bss_head_t;
    bss_head_t *head = (bss_head_t *)arg;
    struct bss_info *bss = NULL;
    g_candidates_count = 0;
    g_candidate_index  = 0;
    g_tried_ssid_only  = 0;

    /* Collect all matching SSID candidates */
    STAILQ_FOREACH(bss, head, next) {
        if (strcmp((char *)bss->ssid, g_cfg.ssid) == 0) {
            if (g_candidates_count < MAX_WIFI_CANDIDATES) {
                memcpy(g_candidates[g_candidates_count].bssid, bss->bssid, 6);
                g_candidates[g_candidates_count].channel = bss->channel;
                g_candidates[g_candidates_count].rssi    = bss->rssi;
                g_candidates_count++;
            }
        }
    }

    if (g_candidates_count == 0) {
        LOGF("wifi_manager: scan done, SSID \"%s\" not found -> AP fallback\n", g_cfg.ssid);
        g_want_ap = 1;
        return;
    }

    /* Sort candidates by RSSI descending (simple bubble sort due to small N) */
    for (int i = 0; i < g_candidates_count - 1; ++i) {
        for (int j = i + 1; j < g_candidates_count; ++j) {
            if (g_candidates[j].rssi > g_candidates[i].rssi) {
                bss_candidate_t tmp = g_candidates[i];
                g_candidates[i] = g_candidates[j];
                g_candidates[j] = tmp;
            }
        }
    }

    LOGF("wifi_manager: scan done, %d candidate(s) for SSID \"%s\"\n", g_candidates_count, g_cfg.ssid);
    for (int i = 0; i < g_candidates_count; ++i) {
        LOGF("  cand %d: BSSID %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d\n",
               i + 1,
               g_candidates[i].bssid[0], g_candidates[i].bssid[1], g_candidates[i].bssid[2],
               g_candidates[i].bssid[3], g_candidates[i].bssid[4], g_candidates[i].bssid[5],
               g_candidates[i].channel,
               g_candidates[i].rssi);
    }

    g_sta_retry = 0;
    g_request_connect = 1; /* Let wifi_task initiate connection */
}

/* ==== WiFi management task ==== */

static void wifi_task(void *pv)
{
    (void)pv;
    uint32_t loop_cnt = 0;
    for (;;) {
        /* Apply any pending WiFi config update (non-blocking) */
        if (g_cfg_update_queue) {
            wifi_cfg_msg_t upd;
            if (xQueueReceive(g_cfg_update_queue, &upd, 0)) {
                g_cfg = upd.wifi;
                LOGF("wifi_manager: received new WiFi config, scheduling STA scan\n");
                g_want_ap = 0;
                g_want_sta_scan = 1;
            }
        }
        if (g_want_ap) {
            g_want_ap = 0;
            if (!g_in_ap_mode) {
                LOGF("wifi_manager: switching to AP mode\n");
                start_softap();
            } else {
                LOGF("wifi_manager: already in AP mode, skip re-init\n");
            }
            /* Clear pending STA operations while in AP mode */
            g_want_sta_scan = 0;
            g_request_connect = 0;
            g_waiting_ip = 0;
        }

        if (g_want_sta_scan && !g_scan_in_progress && g_cfg.ssid[0] != '\0') {
            g_want_sta_scan   = 0;
            g_scan_in_progress = 1;

            LOGF("wifi_manager: starting scan for SSID \"%s\"\n", g_cfg.ssid);

            wifi_set_opmode_current(STATION_MODE);
            g_in_ap_mode = 0;

                struct scan_config sc;
                memset(&sc, 0, sizeof(sc));
                /* Scan all networks; filter SSID ourselves to collect all
                    BSSID/channel variants for the target SSID. */
                sc.ssid       = NULL;
                sc.bssid      = NULL;
                sc.channel    = 0;       // 0 = scan all channels
                sc.show_hidden = 0;

            if (!wifi_station_scan(&sc, scan_done_cb)) {
                LOGF("wifi_manager: wifi_station_scan() call failed\n");
                g_scan_in_progress = 0;
                /* fall back to AP in this case */
                g_want_ap = 1;
            }
        }

        /* Initiate connection after scan, from task context */
        if (g_request_connect && g_candidates_count > 0 && !g_has_ip && !g_waiting_ip) {
            g_request_connect = 0;
            g_candidate_index = 0; /* start from best */
            g_tried_ssid_only = 0;
            connect_current_candidate();
        }

        /* No candidate switch on disconnect; DHCP timeout controls switching */

        /* Handle DHCP timeout while waiting for IP */
        if (g_waiting_ip && !g_has_ip) {
            uint32_t now = xTaskGetTickCount();
            if ((now - g_connect_start_tick) * portTICK_RATE_MS >= DHCP_IP_TIMEOUT_MS) {
                LOGF("wifi_manager: DHCP timeout, switching candidate\n");
                advance_candidate_or_ap("dhcp-timeout");
            }
        }
        if ((++loop_cnt & 0x3F) == 0) { LOG_STACK("wifi_manager"); }
        vTaskDelay(200 / portTICK_RATE_MS);
    }
}
