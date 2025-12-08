# service_interface – HTTP Configuration & Status API

This module implements the **“service interface”**: a minimal HTTP server
that allows configuring and monitoring the device.

Currently built on top of the ESP8266 RTOS SDK `espconn` TCP API.

## Responsibilities

- Start a small HTTP server on port 80 (in STA and/or AP mode).
- Parse incoming requests (initially simple `GET /?ssid=...&pass=...`).
- Call into `config_store` and `wifi_manager` when configuration changes.
- Provide basic status endpoints (Wi-Fi status, IP, sensor states, etc.).

## Public API (suggested)

In `include/service_interface.h`:

- `void service_interface_init(void);`
- Optionally: `void service_interface_set_wifi_config_handler(...)`
  if you want to inject callbacks instead of calling modules directly.

`service_interface_init()`:

- Calls `espconn_init()` if not already done.
- Sets up a single TCP listener on port 80.
- Registers espconn callbacks (`connect`, `recv`, `sent`).

## Future extensions

- Replace plain-text API with:
  - simple HTML page for human config, and/or
  - JSON API (`/api/status`, `/api/config`).
- Add authentication / token to avoid open configuration.
- Support for triggering actions (e.g. `/api/garage/open`).
