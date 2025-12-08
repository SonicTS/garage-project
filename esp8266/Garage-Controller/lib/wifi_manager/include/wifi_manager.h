#pragma once
#include "app_events.h"

#include <stdint.h>
#include "service_interface.h"

#include "config_store.h"   /* for wifi_config_t */
#include "log.h"

/**
 * Simple WiFi config struct.
 * You can reuse this inside your global app_config_t in config_store.
 */
/**
 * Initialize the WiFi manager:
 *  - copies initial_cfg
 *  - registers SDK event handler
 *  - starts internal wifi_task which manages STA <-> AP switching
 */
void wifi_manager_init(const wifi_config_t *initial_cfg);

/**
 * Update WiFi config at runtime (e.g. from service_interface)
 * and trigger a reconnect in STA mode.
 */
void wifi_manager_update_config(const wifi_config_t *new_cfg);

/**
 * Returns non-zero if STA currently has an IP.
 */
int wifi_manager_has_ip(void);

/**
 * Returns current STA IP as uint32 (lwIP ip_addr_t .addr).
 * Returns 0 if no IP is known.
 */
uint32_t wifi_manager_get_ip(void);
