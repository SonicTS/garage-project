#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"
#include "mqtt.h"


#if defined(MBEDTLS_DEBUG_C)
static void mqtt_ssl_debug(void *ctx, int level, const char *file, int line, const char *str){
    (void)ctx;
    if(level <= 2){
        LOGF("mbedtls[%d] %s:%d: %s\n", level, file, line, str);
    }
}
#endif

#include "mqtt_cert.h"

#include "esp_common.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mbedtls/net.h"


#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* mbedTLS headers */


#define MQTT_TASK_STACK  1280
#define MQTT_TASK_PRIO   3

#define MQTT_KEEPALIVE_SEC 60
#define MQTT_RECV_BUF_SIZE 256
#define MQTT_SEND_BUF_SIZE 256

int max_content_len = 3048;   // or 3048 if you want to shave RAM


/* Global config and callback */
static mqtt_config_t    g_mqtt_cfg;
static mqtt_command_cb_t g_cmd_cb = NULL;
static volatile int      g_reconnect_req = 0;

/* MQTT/TLS state */
static int                g_sock        = -1;
static mbedtls_ssl_context g_ssl;
static mbedtls_ssl_config  g_conf;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_entropy_context  g_entropy;
static mbedtls_x509_crt    g_ca;

/* Buffers allocated after TLS handshake to save heap during handshake */
static unsigned char *g_mqtt_send_buf = NULL;
static unsigned char *g_mqtt_recv_buf = NULL;

static int mqtt_alloc_buffers(void){
    if(!g_mqtt_send_buf){ g_mqtt_send_buf = (unsigned char*)malloc(MQTT_SEND_BUF_SIZE); }
    if(!g_mqtt_recv_buf){ g_mqtt_recv_buf = (unsigned char*)malloc(MQTT_RECV_BUF_SIZE); }
    if(!g_mqtt_send_buf || !g_mqtt_recv_buf){
        LOGF("mqtt: buffer alloc failed (heap=%u)\n", (unsigned)system_get_free_heap_size());
        return -1;
    }
    return 0;
}

static void mqtt_free_buffers(void){
    if(g_mqtt_send_buf){ free(g_mqtt_send_buf); g_mqtt_send_buf=NULL; }
    if(g_mqtt_recv_buf){ free(g_mqtt_recv_buf); g_mqtt_recv_buf=NULL; }
}

/* Forward declarations */
static void mqtt_task(void *pv);
static int mqtt_tls_connect(void);
static void mqtt_tls_disconnect(void);
static int mqtt_send(const unsigned char *buf, int len);
static int mqtt_recv(unsigned char *buf, int len, int timeout_ms);

/* MQTT helpers */
static int mqtt_encode_remaining_length(unsigned char *buf, int len);
static int mqtt_send_connect(void);
static int mqtt_send_subscribe(const char *topic);
static int mqtt_send_ping(void);
static int mqtt_send_publish(const char *topic, const char *payload);
static int mqtt_read_and_dispatch(int timeout_ms);

/* ===== Public API ===== */

void mqtt_client_init(const mqtt_config_t *cfg, mqtt_command_cb_t cb)
{
    if (!cfg) return;
    g_mqtt_cfg = *cfg;
    g_cmd_cb   = cb;

    xTaskCreate(mqtt_task, "mqtt_task", MQTT_TASK_STACK, NULL, MQTT_TASK_PRIO, NULL);
}

void mqtt_client_publish_status(const char *subtopic, const char *payload)
{
    if (!g_mqtt_cfg.base_topic[0]) return;

    char topic[128];
    if (!subtopic || !subtopic[0]) {
        snprintf(topic, sizeof(topic), "%s", g_mqtt_cfg.base_topic);
    } else {
        snprintf(topic, sizeof(topic), "%s/%s", g_mqtt_cfg.base_topic, subtopic);
    }

    int rc = mqtt_send_publish(topic, payload ? payload : "");
    if (rc != 0) {
        LOGF("mqtt: publish failed topic=%s rc=%d\n", topic, rc);
    }
}

void mqtt_get_config(mqtt_config_t *out)
{
    if (!out) return;
    *out = g_mqtt_cfg;
}

void mqtt_update_config(const mqtt_config_t *in)
{
    if (!in) return;
    /* Update runtime copy */
    g_mqtt_cfg = *in;
    /* Persist into config_store */
    app_config_t cfg;
    config_store_load(&cfg);
    cfg.mqtt = g_mqtt_cfg;
    config_store_save(&cfg);
    /* Ask task to reconnect with new settings */
    g_reconnect_req = 1;
}

/* ===== TLS helpers ===== */

/* mbedTLS network callbacks */

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int sock = *(int *)ctx;
    int ret  = send(sock, buf, len, 0);

    if (ret > 0) {
        return ret;
    }

    if (ret == 0) {
        // peer closed
        LOGF("net_send: peer closed (ret=0)\n");
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    // ret < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    LOGF("net_send: error ret=%d errno=%d\n", ret, errno);
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int sock = *(int *)ctx;
    int ret  = recv(sock, buf, len, 0);

    if (ret > 0) {
        return ret;
    }

    if (ret == 0) {
        // peer closed
        LOGF("net_recv: peer closed (ret=0)\n");
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    // ret < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    LOGF("net_recv: error ret=%d errno=%d\n", ret, errno);
    return MBEDTLS_ERR_NET_RECV_FAILED;
}


static int mqtt_tls_connect(void)
{
    struct hostent *he;
    struct sockaddr_in addr;
    int ret;
    LOGF("mqtt: heap before DNS=%u\n", (unsigned)system_get_free_heap_size());

    /* Resolve hostname */
    he = gethostbyname(g_mqtt_cfg.broker);
    if (!he) {
        LOGF("mqtt: DNS lookup failed for %s\n", g_mqtt_cfg.broker);
        return -1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        LOGF("mqtt: socket() failed\n");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(g_mqtt_cfg.port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGF("mqtt: connect() failed\n");
        closesocket(g_sock);
        g_sock = -1;
        return -1;
    }

    /* Switch socket to non-blocking mode so mbedTLS can return WANT_READ/WRITE */
    {
        u_long nb = 1;
        int err = ioctlsocket(g_sock, FIONBIO, &nb);
        if (err != 0) {
            LOGF("mqtt: ioctlsocket(FIONBIO) failed, err=%d\n", err);
            // you might want to abort here (closesocket + return -1)
    }
}

    
    if (!g_mqtt_cfg.use_tls) {
        LOGF("mqtt: connected plain TCP to %s:%u\n",
               g_mqtt_cfg.broker, g_mqtt_cfg.port);
        return 0;
    }

    /* Initialize mbedTLS structures */
    LOGF("mqtt: heap before TLS init=%u\n", (unsigned)system_get_free_heap_size());
    mbedtls_ssl_init(&g_ssl);
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_x509_crt_init(&g_ca);

    const char *pers = "mqtt_client";

    if ((ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
                                     &g_entropy,
                                     (const unsigned char *)pers,
                                     strlen(pers))) != 0) {
        LOGF("mqtt: ctr_drbg_seed failed: -0x%04X\n", -ret);
        goto fail;
    }

    /* Load CA certificate unless no-verify mode */
    #ifndef MQTT_TLS_MODE
    #define MQTT_TLS_MODE 2
    #endif
    if (MQTT_TLS_MODE >= 2) {
        ret = mbedtls_x509_crt_parse(&g_ca,
                                     (const unsigned char *)mqtt_ca_pem,
                                     strlen(mqtt_ca_pem) + 1);
        if (ret < 0) {
            LOGF("mqtt: x509_crt_parse failed: -0x%04X\n", -ret);
            goto fail;
        }
    }

    if ((ret = mbedtls_ssl_config_defaults(&g_conf,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        LOGF("mqtt: ssl_config_defaults failed: -0x%04X\n", -ret);
        goto fail;
    }

    if (MQTT_TLS_MODE >= 2) {
        mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
    } else if (MQTT_TLS_MODE == 1) {
        mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);

    /* Prefer TLS 1.2 explicitly if available */
    #if defined(MBEDTLS_SSL_PROTO_TLS1_2)
    mbedtls_ssl_conf_min_version(&g_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    #endif

    /* ALPN hint "mqtt" if supported (some brokers require it on 443; harmless on 8883) */
    #if defined(MBEDTLS_SSL_ALPN)
    const char *alpn_protocols[] = { "mqtt", NULL };
    //mbedtls_ssl_conf_alpn_protocols(&g_conf, alpn_protocols);
    #endif

    /* Reduce fragment length (disabled: prebuilt SDK mbedTLS lacks symbol) */

    /* Debug callback (requires MBEDTLS_DEBUG_C enabled in config). */
    #if defined(MBEDTLS_DEBUG_C)
    mbedtls_ssl_conf_dbg(&g_conf, mqtt_ssl_debug, NULL);
    //mbedtls_debug_set_threshold(2);
    #endif

    if ((ret = mbedtls_ssl_setup(&g_ssl, &g_conf)) != 0) {
        LOGF("mqtt: ssl_setup failed: -0x%04X\n", -ret);
        goto fail;
    }

    /* Only send SNI when verifying; some brokers on IP/virtual hosts reject unknown SNI */
    if (MQTT_TLS_MODE >= 2) {
        if ((ret = mbedtls_ssl_set_hostname(&g_ssl, g_mqtt_cfg.broker)) != 0) {
            LOGF("mqtt: set_hostname failed: -0x%04X\n", -ret);
            goto fail;
        }
    }

    mbedtls_ssl_set_bio(&g_ssl, &g_sock, net_send, net_recv, NULL);

    /* TLS handshake */
    LOGF("mqtt: heap before handshake=%u\n", (unsigned)system_get_free_heap_size());
    while ((ret = mbedtls_ssl_handshake(&g_ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[128];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            if(errbuf[0] == '\0'){
                /* Fallback manual mapping for trimmed builds */
                const char *fallback = "(unknown)";
                switch(ret){
                    case -0x2700: fallback = "CERT_VERIFY_FAILED (name/CA mismatch)"; break;
                    case -0x3B00: fallback = "PK_INVALID_PUBKEY"; break;
                    case -0x3A80: fallback = "PK_INVALID_ALG"; break;
                    case -0x7F00: fallback = "SSL_ALLOC_FAILED"; break;
                    case -0x6E00: fallback = "SSL_INTERNAL_ERROR"; break;
                }
                LOGF("mqtt: handshake failed: -0x%04X %s (manual)\n", -ret, fallback);
            } else {
                LOGF("mqtt: handshake failed: -0x%04X (%s)\n", -ret, errbuf);
            }
            goto fail;
        }
        vTaskDelay(10 / portTICK_RATE_MS);
    }

    LOGF("mqtt: TLS handshake ok with %s:%u\n", g_mqtt_cfg.broker, g_mqtt_cfg.port);
    LOGF("mqtt: heap after handshake=%u\n", (unsigned)system_get_free_heap_size());

    return 0;

fail:
    mqtt_tls_disconnect();
    return -1;
}

static void mqtt_tls_disconnect(void)
{
    if (g_mqtt_cfg.use_tls) {
        mbedtls_ssl_close_notify(&g_ssl);
        mbedtls_ssl_free(&g_ssl);
        mbedtls_ssl_config_free(&g_conf);
        mbedtls_ctr_drbg_free(&g_ctr_drbg);
        mbedtls_entropy_free(&g_entropy);
        mbedtls_x509_crt_free(&g_ca);
    }

    if (g_sock >= 0) {
        closesocket(g_sock);
        g_sock = -1;
    }

    mqtt_free_buffers();
}

static int mqtt_send(const unsigned char *buf, int len)
{
    if (g_sock < 0) return -1;

    if (!g_mqtt_cfg.use_tls) {
        int sent = send(g_sock, buf, len, 0);
        return (sent == len) ? 0 : -1;
    } else {
        int ret;
        int sent = 0;
        while (sent < len) {
            ret = mbedtls_ssl_write(&g_ssl, buf + sent, len - sent);
            if (ret > 0) {
                sent += ret;
            } else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                       ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(5 / portTICK_RATE_MS);
                continue;
            } else {
                LOGF("mqtt: ssl_write failed: -0x%04X\n", -ret);
                return -1;
            }
        }
        return 0;
    }
}

static int mqtt_recv(unsigned char *buf, int len, int timeout_ms)
{
    if (g_sock < 0) return -1;

    int ret;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        if (!g_mqtt_cfg.use_tls) {
            ret = recv(g_sock, buf, len, MSG_DONTWAIT);
            if (ret > 0) return ret;
        } else {
            ret = mbedtls_ssl_read(&g_ssl, buf, len);
            if (ret > 0) return ret;
            if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                // no data yet
            } else if (ret == 0) {
                LOGF("mqtt: connection closed\n");
                return -1;
            } else {
                LOGF("mqtt: ssl_read failed: -0x%04X\n", -ret);
                return -1;
            }
        }

        vTaskDelay(10 / portTICK_RATE_MS);
        elapsed += 10;
    }

    return 0; /* timeout with no data */
}

/* ===== MQTT helpers (minimal v3.1.1, QoS 0 only) ===== */

static int mqtt_encode_remaining_length(unsigned char *buf, int len)
{
    int i = 0;
    do {
        int encoded = len % 128;
        len /= 128;
        if (len > 0) encoded |= 128;
        buf[i++] = (unsigned char)encoded;
    } while (len > 0 && i < 4);
    return i;
}

static int mqtt_send_connect(void)
{
    if(!g_mqtt_send_buf){ return -1; }
    unsigned char *p = g_mqtt_send_buf;
    int len, rem_len;

    /* Variable header and payload go into a temp buffer first */
    unsigned char vh[256];
    unsigned char *q = vh;

    /* Protocol name "MQTT" */
    *q++ = 0x00;
    *q++ = 0x04;
    *q++ = 'M';
    *q++ = 'Q';
    *q++ = 'T';
    *q++ = 'T';

    /* Protocol level 4 (MQTT 3.1.1) */
    *q++ = 0x04;

    /* Connect flags */
    unsigned char flags = 0;
    flags |= (1 << 1); /* clean session */
    if (g_mqtt_cfg.username[0]) flags |= (1 << 7);
    if (g_mqtt_cfg.password[0]) flags |= (1 << 6);
    *q++ = flags;

    /* Keep alive (seconds) */
    *q++ = (MQTT_KEEPALIVE_SEC >> 8) & 0xFF;
    *q++ = (MQTT_KEEPALIVE_SEC     ) & 0xFF;

    /* Payload: client ID */
    uint16_t cid_len = strlen(g_mqtt_cfg.client_id);
    *q++ = (cid_len >> 8) & 0xFF;
    *q++ = (cid_len     ) & 0xFF;
    memcpy(q, g_mqtt_cfg.client_id, cid_len);
    q += cid_len;

    /* Payload: username (optional) */
    if (g_mqtt_cfg.username[0]) {
        uint16_t ulen = strlen(g_mqtt_cfg.username);
        *q++ = (ulen >> 8) & 0xFF;
        *q++ = (ulen     ) & 0xFF;
        memcpy(q, g_mqtt_cfg.username, ulen);
        q += ulen;
    }

    /* Payload: password (optional) */
    if (g_mqtt_cfg.password[0]) {
        uint16_t plen = strlen(g_mqtt_cfg.password);
        *q++ = (plen >> 8) & 0xFF;
        *q++ = (plen     ) & 0xFF;
        memcpy(q, g_mqtt_cfg.password, plen);
        q += plen;
    }

    rem_len = q - vh;

    /* Fixed header */
    *p++ = 0x10; /* CONNECT */
    p += mqtt_encode_remaining_length(p, rem_len);

    /* Copy variable header + payload */
    memcpy(p, vh, rem_len);
    len = (p - g_mqtt_send_buf) + rem_len;

    return mqtt_send(g_mqtt_send_buf, len);
}

static int mqtt_send_subscribe(const char *topic)
{
    if(!g_mqtt_send_buf){ return -1; }
    unsigned char *p = g_mqtt_send_buf;
    unsigned char vh[4];
    unsigned char *q = vh;
    int rem_len, len;

    /* Packet identifier (arbitrary, here 1) */
    *q++ = 0x00;
    *q++ = 0x01;

    /* Topic */
    uint16_t tlen = strlen(topic);
    *q++ = (tlen >> 8) & 0xFF;
    *q++ = (tlen     ) & 0xFF;

    /* We will append topic + QoS in main buffer */
    *p++ = 0x82; /* SUBSCRIBE, QoS 1 required by spec */
    rem_len = 2 + 2 + tlen + 1; /* packet id (2) + topic len(2) + topic + QoS */
    p += mqtt_encode_remaining_length(p, rem_len);

    /* Packet ID */
    *p++ = 0x00;
    *p++ = 0x01;

    /* Topic string */
    *p++ = (tlen >> 8) & 0xFF;
    *p++ = (tlen     ) & 0xFF;
    memcpy(p, topic, tlen);
    p += tlen;

    /* Requested QoS = 0 */
    *p++ = 0x00;

    len = p - g_mqtt_send_buf;
    return mqtt_send(g_mqtt_send_buf, len);
}

static int mqtt_send_ping(void)
{
    unsigned char buf[2] = { 0xC0, 0x00 }; /* PINGREQ */
    return mqtt_send(buf, 2);
}

static int mqtt_send_publish(const char *topic, const char *payload)
{
    if(!g_mqtt_send_buf){ return -1; }
    unsigned char *p = g_mqtt_send_buf;
    int len;
    int payload_len = strlen(payload);
    uint16_t tlen   = strlen(topic);

    /* Fixed header */
    *p++ = 0x30; /* PUBLISH, QoS 0 */

    int rem_len = 2 + tlen + payload_len;
    p += mqtt_encode_remaining_length(p, rem_len);

    /* Topic */
    *p++ = (tlen >> 8) & 0xFF;
    *p++ = (tlen     ) & 0xFF;
    memcpy(p, topic, tlen);
    p += tlen;

    /* Payload */
    memcpy(p, payload, payload_len);
    p += payload_len;

    len = p - g_mqtt_send_buf;
    return mqtt_send(g_mqtt_send_buf, len);
}

/* Read one MQTT packet and dispatch commands (PUBLISH) */
static int mqtt_read_and_dispatch(int read_timeout_ms)
{
    if(!g_mqtt_recv_buf){ return -1; }
    int ret = mqtt_recv(g_mqtt_recv_buf, 1, read_timeout_ms); /* first byte (type + flags) */
    if (ret <= 0) return ret; /* timeout or error */

    unsigned char header = g_mqtt_recv_buf[0];

    /* Read remaining length */
    int multiplier = 1;
    int remaining_len = 0;
    int idx = 0;

    do {
        ret = mqtt_recv(&g_mqtt_recv_buf[idx], 1, read_timeout_ms);
        if (ret <= 0) return ret;

        unsigned char encoded = g_mqtt_recv_buf[idx];
        remaining_len += (encoded & 127) * multiplier;
        multiplier *= 128;
        idx++;
    } while ((g_mqtt_recv_buf[idx - 1] & 128) != 0 && idx < 4);

    if (remaining_len > (MQTT_RECV_BUF_SIZE - 1)) {
        /* too big, drain and drop */
        int to_read = remaining_len;
        while (to_read > 0) {
            int chunk = (to_read > 128) ? 128 : to_read;
            ret = mqtt_recv(g_mqtt_recv_buf, chunk, 1000);
            if (ret <= 0) break;
            to_read -= ret;
        }
        return 0;
    }

    /* Read the remaining payload */
    ret = mqtt_recv(g_mqtt_recv_buf, remaining_len, read_timeout_ms);
    if (ret <= 0) return ret;

    /* Handle PUBLISH packets only for now */
    unsigned char packet_type = header & 0xF0;
    if (packet_type == 0x30) {
        /* PUBLISH QoS 0 */
        int pos = 0;
        if (remaining_len < 2) return 0;
        uint16_t tlen = (g_mqtt_recv_buf[pos] << 8) | g_mqtt_recv_buf[pos + 1];
        pos += 2;
        if (tlen + pos > remaining_len) return 0;

        char topic[128];
        int copy_len = (tlen < (sizeof(topic) - 1)) ? tlen : (sizeof(topic) - 1);
        memcpy(topic, &g_mqtt_recv_buf[pos], copy_len);
        topic[copy_len] = '\0';
        pos += tlen;

        /* No packet ID (QoS 0). Payload is rest. */
        int payload_len = remaining_len - pos;
        if (payload_len < 0) return 0;

        if (g_cmd_cb) {
            g_cmd_cb(topic, (char *)&g_mqtt_recv_buf[pos], payload_len);
        }
    } else {
        /* For now, ignore CONNACK, SUBACK, PINGRESP, etc. */
    }

    return 0;
}

/* ===== MQTT task ===== */

static void mqtt_task(void *pv)
{
    (void)pv;

    app_evt_t ev;

    int waited_wifi_once = 0;

    for (;;) {
        /* Wait for WiFi to be up once (at startup). */
        if (!waited_wifi_once) {
            LOGF("mqtt: waiting for WIFI_UP...\n");
            for (;;) {
                if (xQueueReceive(g_app_event_queue, &ev, portMAX_DELAY)) {
                    if (ev.type == APP_EVT_WIFI_UP) {
                        waited_wifi_once = 1;
                        break;
                    }
                }
            }
        }

        LOGF("mqtt: WIFI_UP, connecting to broker %s:%u\n",
               g_mqtt_cfg.broker, g_mqtt_cfg.port);

        if (mqtt_tls_connect() != 0) {
            LOGF("mqtt: TLS/TCP connect failed, waiting before retry\n");
            vTaskDelay(20000 / portTICK_RATE_MS);
            continue;
        }

        /* Allocate MQTT buffers now that handshake is done */
        if(mqtt_alloc_buffers() != 0){
            LOGF("mqtt: buffers alloc failed after TLS\n");
            mqtt_tls_disconnect();
            vTaskDelay(3000 / portTICK_RATE_MS);
            continue;
        }

        if (mqtt_send_connect() != 0) {
            LOGF("mqtt: CONNECT failed\n");
            mqtt_tls_disconnect();
            vTaskDelay(3000 / portTICK_RATE_MS);
            continue;
        }

        /* Subscribe to command topic: base_topic + "/cmd" */
        char cmd_topic[128];
        snprintf(cmd_topic, sizeof(cmd_topic), "%s/cmd", g_mqtt_cfg.base_topic);
        if (mqtt_send_subscribe(cmd_topic) != 0) {
            LOGF("mqtt: SUBSCRIBE failed\n");
            mqtt_tls_disconnect();
            vTaskDelay(3000 / portTICK_RATE_MS);
            continue;
        }

        LOGF("mqtt: connected and subscribed to %s\n", cmd_topic);

        int keepalive_ms = MQTT_KEEPALIVE_SEC * 1000;
        int elapsed_ms   = 0;
        const int io_tick_ms = 100; /* reduce I/O wait to improve responsiveness */
        int state_pub_ms = 0; /* periodic garage state publish */
        bool disonnected = false;

        /* Main loop */
        uint32_t loop_cnt = 0;
        while (1) {
            /* Drain the app event queue to avoid backlog while network ops block */
            while (xQueueReceive(g_app_event_queue, &ev, 0) == pdTRUE) {
                if (ev.type == APP_EVT_WIFI_DOWN) {
                    LOGF("mqtt: WIFI_DOWN, disconnecting\n");
                    disonnected = true;
                    break;
                }
                if (ev.type == APP_EVT_GARAGE_STATE) {
                    /* Publish logical garage state */
                    const char *s = "unknown";
                    switch (ev.data.garage_state.state) {
                        case GARAGE_STATE_UNKNOWN:        s = "unknown"; break;
                        case GARAGE_STATE_CLOSED:         s = "closed"; break;
                        case GARAGE_STATE_OPEN:           s = "open"; break;
                        case GARAGE_STATE_PARTIALLY_OPEN: s = "partial"; break;
                        case GARAGE_STATE_MOVING_UP:      s = "moving_up"; break;
                        case GARAGE_STATE_MOVING_DOWN:    s = "moving_down"; break;
                        case GARAGE_STATE_ERROR:          s = "error"; break;
                    }
                    mqtt_client_publish_status("state", s);
                    mqtt_client_publish_status("control", ev.data.garage_state.control ? "active" : "idle");
                }
            }
            if (disonnected) {
                disonnected = false;
                break;
            }
            if (g_reconnect_req) {
                LOGF("mqtt: config changed, reconnecting\n");
                g_reconnect_req = 0;
                break;
            }

            /* Process one MQTT packet (or timeout) */
            int r = mqtt_read_and_dispatch(io_tick_ms);
            if (r < 0) {
                LOGF("mqtt: read error, breaking\n");
                break;
            }
            /* Keepalive: send ping periodically */
            elapsed_ms += io_tick_ms; /* approximate, based on I/O tick */
            if (elapsed_ms >= keepalive_ms) {
                mqtt_send_ping();
                elapsed_ms = 0;
            }

            /* Periodic state publish every ~5s independent of events */
            state_pub_ms += io_tick_ms;
            if (state_pub_ms >= 5000) {
                state_pub_ms = 0;
                garage_state_t ls = garage_control_get_logical_state();
                const char *s = "unknown";
                switch (ls) {
                    case GARAGE_STATE_OPEN:           s = "open"; break;
                    case GARAGE_STATE_CLOSED:         s = "closed"; break;
                    case GARAGE_STATE_PARTIALLY_OPEN: s = "partial"; break;
                    case GARAGE_STATE_MOVING_UP:       s = "moving up"; break;
                    case GARAGE_STATE_MOVING_DOWN:     s = "moving down"; break;
                    case GARAGE_STATE_ERROR:          s = "error"; break;
                    case GARAGE_STATE_UNKNOWN:        s = "unknown"; break;
                }
                mqtt_client_publish_status("state", s);
                const char *ctrl = (ls == GARAGE_STATE_MOVING_UP || ls == GARAGE_STATE_MOVING_DOWN) ? "active" : "idle";
                mqtt_client_publish_status("control", ctrl);
            }
            if ((++loop_cnt & 0x3F) == 0) { LOG_STACK("mqtt"); }
        }
        mqtt_tls_disconnect();
        LOGF("mqtt: disconnected, waiting before reconnect\n");
        vTaskDelay(3000 / portTICK_RATE_MS);
    }
}
