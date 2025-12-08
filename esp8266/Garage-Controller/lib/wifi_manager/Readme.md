# wifi_manager – Wi-Fi State Machine (STA + AP Fallback)

This module manages **all Wi-Fi behavior**:

- Station mode (STA) connection to the configured SSID.
- Retry logic with a maximum retry count.
- Automatic fallback to SoftAP mode when STA fails.
- Notifies other modules when:
  - STA obtained an IP address.
  - STA disconnected.
  - SoftAP is active.

## Responsibilities

- Hide ESP8266 RTOS Wi-Fi details (`wifi_station_*`, `wifi_softap_*`,
  `System_Event_t`) behind a clean, app-level API.
- Run a dedicated `wifi_task` FreeRTOS task that:
  - reacts to events through flags (`want_ap`, `want_sta_recon`, etc.),
  - performs mode switches *outside* the Wi-Fi event context.
- Expose the current status to other modules (e.g. HTTP, app logic).

## Public API (suggested)

In `include/wifi_manager.h`:

- `typedef struct { char ssid[32]; char password[64]; } wifi_config_t;`
- `void wifi_manager_init(const wifi_config_t *initial_cfg);`
- `void wifi_manager_update_config(const wifi_config_t *new_cfg);`
- `int  wifi_manager_has_ip(void);`  
- `void wifi_manager_get_ip(uint32_t *ip_out);` (optional helper)

## Interactions

- **service_interface**  
  Calls `wifi_manager_update_config()` when user changes SSID/Password.
- **config_store**  
  Provides initial Wi-Fi config and saves updated config when Wi-Fi changes.
- **app**  
  Calls `wifi_manager_init()` during startup.
