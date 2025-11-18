# Garage Controller (ESP8266 RTOS)

Wi-Fi–enabled garage controller running on an ESP8266 (ESP-12F) using the
ESP8266 RTOS SDK and PlatformIO.

Main features (planned / in progress):

- STA + AP fallback Wi-Fi manager
- HTTP service interface for configuration
- Persistent configuration storage (Wi-Fi, MQTT, logic)
- Garage control logic (relays, sensors, alarms)
- MQTT integration for remote control / monitoring

## Project layout

- `src/`
  - `main.c` – SDK entrypoint (`user_init` + RF cal) that just calls `app_start()`.
- `lib/`
  - `app/` – top-level orchestration: starts Wi-Fi, HTTP, garage logic, etc.
  - `wifi_manager/` – STA/AP state machine, retries, IP tracking.
  - `service_interface/` – HTTP/REST-style configuration API.
  - `config_store/` – configuration structs + persistence (flash).
  - `garage_control/` – garage-door specific logic (GPIO, sensors, alarms).
  - `mqtt/` – MQTT client wrapper and integration.

## Build / flash

```bash
# Build
pio run

# Upload firmware
pio run -t upload

# Serial monitor
pio device monitor
