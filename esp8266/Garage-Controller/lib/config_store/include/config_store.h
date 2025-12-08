#pragma once

#include <stdint.h>
#include "log.h"

/* ---- WiFi sub-config ---- */
typedef struct {
    char ssid[32];
    char password[64];
} wifi_config_t;

/* ---- MQTT sub-config (placeholder for later) ---- */
typedef struct {
    char broker[64];      // e.g. "mqtt.example.com"
    uint16_t port;        // e.g. 8883 for TLS
    char client_id[32];   // e.g. "garage-esp-01"
    char username[32];    // optional
    char password[32];    // optional
    char base_topic[64];  // e.g. "garage/door1"
    uint8_t use_tls;      // 1 = use TLS (mbedTLS), 0 = plain TCP
} mqtt_config_t;


/* ---- App-wide config ---- */
typedef struct {
    wifi_config_t wifi;
    mqtt_config_t mqtt;
    /* Garage door related settings */
    struct {
        /* Automatically close the door this many seconds after reaching OPEN.
           0 = disabled. Persisted in flash. */
        uint32_t close_after_seconds;
    } garage;
    /* later:
       - garage settings
       - alarm thresholds
       - etc.
     */
} app_config_t;

/* Initialize config store (set defaults in RAM, prepare for flash later). */
void config_store_init(void);

/* Load full config into cfg_out (from RAM for now, later from flash). */
void config_store_load(app_config_t *cfg_out);

/* Save full config (to RAM now, later to flash). */
void config_store_save(const app_config_t *cfg);

/* Convenience helpers for just WiFi part. */
void config_store_get_wifi(wifi_config_t *wifi_out);
void config_store_set_wifi(const wifi_config_t *wifi_in);

/* Get/Set just the garage portion */
void config_store_get_garage(uint32_t *close_after_seconds_out);
void config_store_set_garage_close_after(uint32_t close_after_seconds);
