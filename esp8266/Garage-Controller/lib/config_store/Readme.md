# config_store – Configuration Structures & Persistence

This module owns the **persistent configuration** of the device:

- Wi-Fi credentials (SSID / password).
- MQTT settings (broker, topic, auth).
- Garage control settings (timings, sensor thresholds, alarm parameters).
- Any other tunable parameters.

Initially it can keep everything in RAM; later it will write to flash.

## Responsibilities

- Define the central `config_t` struct used across the project.
- Provide functions to:
  - load config (from flash or defaults),
  - save config, and
  - update sub-configs (Wi-Fi, MQTT, garage logic).

## Public API (suggested)

In `include/config_store.h`:

```c
typedef struct {
    char ssid[32];
    char password[64];
} wifi_config_t;

typedef struct {
    char broker[64];
    uint16_t port;
    char topic[64];
    char username[32];
    char password[32];
} mqtt_config_t;

typedef struct {
    wifi_config_t wifi;
    mqtt_config_t mqtt;
    // garage logic options...
} app_config_t;

void config_store_init(void);
void config_store_load(app_config_t *cfg_out);
void config_store_save(const app_config_t *cfg);
```

config_store_init() can prepare flash sectors / SPIFFS / system params.

## Future work

Implement actual flash storage (system param API or custom sector layout).
add versioning / defaults / migration in case the struct changes.