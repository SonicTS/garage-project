# mqtt – MQTT Client Wrapper

This module will provide an **MQTT abstraction layer** on top of whatever
MQTT client you choose (e.g. `nopoll`, a lightweight client, or a custom
implementation using raw sockets / TLS).

## Responsibilities

- Connect to the configured MQTT broker.
- Reconnect on Wi-Fi reconnect.
- Subscribe to relevant topics (e.g. `garage/cmd/#`).
- Publish state updates (door state, sensor readings, alarms).
- Provide a simple C API for other modules to:
  - publish messages,
  - register callbacks for incoming commands.

## Public API (suggested)

In `include/mqtt.h`:

```c
typedef struct {
    char broker[64];
    uint16_t port;
    char client_id[32];
    char username[32];
    char password[32];
    char base_topic[64]; // e.g. "garage"
} mqtt_config_t;

void mqtt_init(const mqtt_config_t *cfg);
void mqtt_publish(const char *topic_suffix, const char *payload);
void mqtt_set_command_handler(void (*handler)(const char *topic,
                                              const char *payload));
```

Interactions:

config_store – provides broker / topic / auth configuration.

garage_control – uses MQTT to receive commands and publish state.

wifi_manager – should inform MQTT about Wi-Fi connectivity changes.