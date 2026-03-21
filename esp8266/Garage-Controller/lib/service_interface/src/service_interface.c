/* HTML service interface: WiFi + MQTT settings */

#include "service_interface.h"
#include "config_store.h"

#include "esp_common.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* HTTP server state */
static int http_started = 0; /* 0=stopped, 1=running */
static int http_listen_sock = -1; /* listening socket */
/* Static buffers to avoid per-request malloc fragmentation & stack overflow */
static char http_req_buf[512]; /* trimmed request buffer */

/* Socket-based HTTP server (replace espconn to avoid heap corruption). */

static void get_param_value(const char *query,const char *key,char *out,int out_len){ out[0]='\0'; if(!query||!key||!out||out_len<=0) return; const char *p=strstr(query,key); if(!p) return; p+=strlen(key); if(*p=='=') p++; int i=0; while(*p&&*p!='&'&&*p!=' '&&i<out_len-1){ out[i++]=*p++; } out[i]='\0'; }

static int send_all(int sock, const char *buf, int len){
    int total = 0; while (total < len){ int sent = send(sock, buf+total, len-total, 0); if (sent <= 0) return -1; total += sent; } return 0; }

static void send_redirect(int csock, const char *location)
{
    if (!location) location = "/";
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 303 See Other\r\n"
                        "Location: %s\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        location);
    if (hlen < 0 || hlen >= (int)sizeof(hdr)) return;
    (void)send_all(csock, hdr, hlen);
}

static void send_html_form(int csock, const char *status)
{
    app_config_t cfg;
    config_store_load(&cfg);

    mqtt_config_t mc;
    mqtt_get_config(&mc);

    /* Build a masked password for MQTT display */
    char masked[16];
    size_t plen = strlen(mc.password);
    size_t copy = plen > 2 ? 2 : plen;
    memcpy(masked, mc.password, copy);
    size_t fill = (plen > copy) ? (plen - copy) : 0;
    if (fill > (sizeof(masked) - 1 - copy))
        fill = (sizeof(masked) - 1 - copy);
    memset(masked + copy, '*', fill);
    masked[copy + fill] = '\0';

    const char *status_html = status ? status : "";

    /* Send header WITHOUT Content-Length (Connection: close will delimit) */
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send_all(csock, hdr, strlen(hdr)) != 0) return;

    /* Now stream the HTML in small chunks */

    const char *part1 =
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP Setup</title>"
        "<style>"
        "body{font-family:sans-serif;font-size:14px;margin:8px;background:#fff;color:#000;}"
        "label{display:block;margin-top:6px;}"
        "input{width:100%;box-sizing:border-box;padding:2px;margin-top:2px;}"
        "button{margin-top:10px;padding:4px 8px;}"
        "small{color:#090;}"
        "</style>"
        "</head><body>";

    if (send_all(csock, part1, strlen(part1)) != 0) return;

    /* Status banner */
    if (status_html[0]) {
        if (send_all(csock, status_html, strlen(status_html)) != 0) return;
    }

    if (send_all(csock, "<h3>WiFi</h3><form method=\"GET\" action=\"/\">", 42) != 0) return;

    char line[192];

    /* WiFi SSID */
    snprintf(line, sizeof(line),
             "<label>SSID:<input name=\"wifi_ssid\" value=\"%s\" maxlength=\"31\"></label>",
             cfg.wifi.ssid);
    if (send_all(csock, line, strlen(line)) != 0) return;

    /* WiFi password */
    snprintf(line, sizeof(line),
             "<label>Pass:<input type=\"password\" name=\"wifi_pass\" value=\"%s\" maxlength=\"63\"></label>",
             cfg.wifi.password);
    if (send_all(csock, line, strlen(line)) != 0) return;

    /* MQTT section */
    if (send_all(csock, "<h3>MQTT</h3>", 12) != 0) return;

    snprintf(line, sizeof(line),
             "<label>Broker:<input name=\"mqtt_broker\" value=\"%s\" maxlength=\"63\"></label>",
             mc.broker);
    if (send_all(csock, line, strlen(line)) != 0) return;

    snprintf(line, sizeof(line),
             "<label>Port:<input name=\"mqtt_port\" value=\"%u\" maxlength=\"5\"></label>",
             (unsigned)mc.port);
    if (send_all(csock, line, strlen(line)) != 0) return;

    snprintf(line, sizeof(line),
             "<label>Client:<input name=\"mqtt_client\" value=\"%s\" maxlength=\"31\"></label>",
             mc.client_id);
    if (send_all(csock, line, strlen(line)) != 0) return;

    snprintf(line, sizeof(line),
             "<label>User:<input name=\"mqtt_user\" value=\"%s\" maxlength=\"31\"></label>",
             mc.username);
    if (send_all(csock, line, strlen(line)) != 0) return;

    snprintf(line, sizeof(line),
             "<label>Pass:<input type=\"password\" name=\"mqtt_pass\" value=\"%s\" maxlength=\"31\"></label>",
             masked);
    if (send_all(csock, line, strlen(line)) != 0) return;

    snprintf(line, sizeof(line),
             "<label>Base:<input name=\"mqtt_base\" value=\"%s\" maxlength=\"63\"></label>",
             mc.base_topic);
    if (send_all(csock, line, strlen(line)) != 0) return;

    /* TLS checkbox */
    snprintf(line, sizeof(line),
             "<label>TLS:<input type=\"checkbox\" name=\"mqtt_tls\" value=\"1\" %s></label>",
             mc.use_tls ? "checked" : "");
    if (send_all(csock, line, strlen(line)) != 0) return;

    /* GPIO Inverted checkbox */
    snprintf(line, sizeof(line),
             "<label>GPIO Inverted:<input type=\"checkbox\" name=\"gpio_inverted\" value=\"1\" %s></label>",
             cfg.gpio_inverted ? "checked" : "");
    if (send_all(csock, line, strlen(line)) != 0) return;

    if (send_all(csock, "<button type=\"submit\">Save</button></form>", 44) != 0) return;

    /* Footer with heap info */
    snprintf(line, sizeof(line),
             "<hr><small>Heap:%u</small></body></html>",
             (unsigned)system_get_free_heap_size());
    if (send_all(csock, line, strlen(line)) != 0) return;
}


static void send_text(int csock,const char *status,const char *body){
    if (!status) status = "200 OK";
    if (!body) body = "";
    int blen = (int)strlen(body);
    char hdr[192];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, blen);
    if (hlen < 0 || hlen >= (int)sizeof(hdr)) return;
    if (send_all(csock, hdr, hlen) != 0) return;
    if (blen > 0) { (void)send_all(csock, body, blen); }
}

static void handle_client(int csock)
{
    int r = recv(csock, http_req_buf, sizeof(http_req_buf) - 1, 0);
    if (r <= 0) {
        LOGF("service_interface: recv failed r=%d\n", r);
        return;
    }

    http_req_buf[r] = '\0';
    LOGF("service_interface: request (%d bytes)\n", r);

    char *first = strstr(http_req_buf, "GET ");
    char *query = NULL;
    char path[32];
    path[0] = '\0';

    if (first) {
        char *ps = first + 4;
        char *space = strchr(ps, ' ');
        if (space) *space = '\0';
        strncpy(path, ps, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';

        char *q = strchr(ps, '?');
        if (q) {
            *q = '\0';
            query = q + 1;
        }
    }

    if (strncmp(path, "/ping", 5) == 0) {
        send_text(csock, "200 OK", "pong\n");
        LOGF("service_interface: /ping served\n");
        return;
    }

    app_config_t cfg;
    config_store_load(&cfg);

    mqtt_config_t mc;
    mqtt_get_config(&mc);

    if (query) {
        char wifi_ssid[32], wifi_pass[64];
        char br[64], cid[32], user[32], pass[32], base[64], port[8];

        get_param_value(query, "wifi_ssid",   wifi_ssid, sizeof(wifi_ssid));
        get_param_value(query, "wifi_pass",   wifi_pass, sizeof(wifi_pass));
        get_param_value(query, "mqtt_broker", br,        sizeof(br));
        get_param_value(query, "mqtt_port",   port,      sizeof(port));
        get_param_value(query, "mqtt_client", cid,       sizeof(cid));
        get_param_value(query, "mqtt_user",   user,      sizeof(user));
        get_param_value(query, "mqtt_pass",   pass,      sizeof(pass));
        get_param_value(query, "mqtt_base",   base,      sizeof(base));

        int wifi_changed = 0;
        int mqtt_changed = 0;

        /* WiFi: only update if at least one field is non-empty */
        if (wifi_ssid[0] || wifi_pass[0]) {
            if (strcmp(cfg.wifi.ssid, wifi_ssid) != 0 ||
                strcmp(cfg.wifi.password, wifi_pass) != 0) {

                wifi_changed = 1;
                strncpy(cfg.wifi.ssid, wifi_ssid, sizeof(cfg.wifi.ssid) - 1);
                cfg.wifi.ssid[sizeof(cfg.wifi.ssid) - 1] = '\0';

                strncpy(cfg.wifi.password, wifi_pass, sizeof(cfg.wifi.password) - 1);
                cfg.wifi.password[sizeof(cfg.wifi.password) - 1] = '\0';
            }
        }

        /* MQTT: only touch fields that were non-empty in the query */
        if (br[0])  { strncpy(mc.broker,     br,   sizeof(mc.broker) - 1);     mqtt_changed = 1; }
        if (port[0]){ mc.port = (uint16_t)atoi(port);                         mqtt_changed = 1; }
        if (cid[0]) { strncpy(mc.client_id,  cid,  sizeof(mc.client_id) - 1);  mqtt_changed = 1; }
        if (user[0]){ strncpy(mc.username,   user, sizeof(mc.username) - 1);   mqtt_changed = 1; }
        if (pass[0]){ strncpy(mc.password,   pass, sizeof(mc.password) - 1);   mqtt_changed = 1; }
        if (base[0]){ strncpy(mc.base_topic, base, sizeof(mc.base_topic) - 1); mqtt_changed = 1; }

        /* Checkbox semantics: present means enabled, absent means disabled. */
        {
            uint8_t new_tls = (strstr(query, "mqtt_tls=1") != NULL) ? 1 : 0;
            if (mc.use_tls != new_tls) {
                mc.use_tls = new_tls;
                mqtt_changed = 1;
            }
        }

        {
            bool new_gpio_inverted = (strstr(query, "gpio_inverted=1") != NULL) ? true : false;
            if (cfg.gpio_inverted != new_gpio_inverted) {
                cfg.gpio_inverted = new_gpio_inverted;
                LOGF("service_interface: GPIO inversion set to %d\n", cfg.gpio_inverted);
                config_store_set_gpio_inverted(cfg.gpio_inverted);
            }
        }

        LOGF("service_interface: wifi_changed=%d mqtt_changed=%d\n",
               wifi_changed, mqtt_changed);

        if (wifi_changed) {
            /* WiFi change: tiny response, then reconfigure WiFi (connection may drop). */
            send_text(csock, "200 OK",
                      "WiFi settings saved.\n"
                      "The device is reconnecting WiFi now.\n"
                      "If you changed SSID/password, reconnect to that network,\n"
                      "then open this page again.\n");

            config_store_set_wifi(&cfg.wifi);
            wifi_manager_update_config(&cfg.wifi);

            if (mqtt_changed) {
                mqtt_update_config(&mc);
            }
            return;
        }

        if (mqtt_changed) {
            /* Only MQTT changed: show full form with Saved banner */
            send_html_form(csock, "<p><b>Saved.</b> MQTT settings applied.</p>");
            mqtt_update_config(&mc);
            return;
        }

        /* Query but no actual changes: show form with "No changes" */
        send_html_form(csock, "<p>No changes.</p>");
        return;
    }

    /* Plain GET without query: just show the form */
    send_html_form(csock, NULL);
}

static void http_server_task(void *pv); /* forward */

void service_interface_start(void){
    if(http_started) return;
    http_started=1;
    LOGF("service_interface: starting socket HTTP server on 80 (heap=%u)\n", (unsigned)system_get_free_heap_size());

    http_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (http_listen_sock < 0){
        LOGF("service_interface: socket() failed\n");
        return;
    }
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET; addr.sin_port=htons(80); addr.sin_addr.s_addr=INADDR_ANY;
    int opt=1; setsockopt(http_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(http_listen_sock, (struct sockaddr*)&addr, sizeof(addr))!=0){
        LOGF("service_interface: bind failed errno=%d\n", errno);
        closesocket(http_listen_sock); http_listen_sock=-1; http_started=0; return;
    }
    if (listen(http_listen_sock, 2)!=0){
        LOGF("service_interface: listen failed errno=%d\n", errno);
        closesocket(http_listen_sock); http_listen_sock=-1; http_started=0; return;
    }
    LOGF("service_interface: listening socket=%d\n", http_listen_sock);
    xTaskCreate(http_server_task, "http_srv", 1024, NULL, 2, NULL);
}

void service_interface_stop(void){
    if(!http_started) return;
    if (http_listen_sock >= 0){ closesocket(http_listen_sock); http_listen_sock = -1; }
    http_started = 0;
}

/* Separate task implementation (cannot use lambda in C). */
void http_server_task(void *pv){
    (void)pv;
    LOGF("service_interface: http server task running\n");
    for(;;){
        if(http_listen_sock < 0){
            vTaskDelay(500/portTICK_RATE_MS);
            continue;
        }
        struct sockaddr_in caddr; socklen_t clen=sizeof(caddr);
        int cs = accept(http_listen_sock,(struct sockaddr*)&caddr,&clen);
        if (cs>=0){
            struct timeval tv;
            tv.tv_sec  = 2;
            tv.tv_usec = 0;
            setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            LOGF("service_interface: client accepted socket=%d heap=%u\n", cs, (unsigned)system_get_free_heap_size());
            handle_client(cs);
            closesocket(cs);
            LOGF("service_interface: client closed\n");
        } else {
            /* Accept timeout or error; keep loop light */
            vTaskDelay(50/portTICK_RATE_MS);
        }
    }
}

/* Backward-compatible alias */
void service_interface_init(void){ service_interface_start(); }
