
---

## `lib/app/README.md`

```markdown
# app – Top-Level Application Orchestrator

This module owns the **overall startup** of the firmware. It is the only
library directly called from `user_init()` in `src/main.c`.

## Responsibilities

- Initialize board-level pieces in the right order:
  - GPIO / LED / basic hardware
  - Wi-Fi stack
  - HTTP service interface
  - Garage control logic
  - MQTT
- Create global / cross-module tasks (e.g. a debug blink task).
- Wire modules together:
  - Pass initial configuration to `wifi_manager`.
  - Register service callbacks with `service_interface`.
  - Pass configuration + event hooks to `garage_control` and `mqtt`.

## Public API

Defined in `include/app.h`:

- `void app_start(void);`  
  Called once from `user_init()`. This function must never return.

## Typical flow

1. Configure UART/console (done in `main.c`).
2. Call `app_start()`.
3. `app_start()`:
   - Initializes GPIO via `gpio_ctrl` (if used).
   - Starts a blink / watchdog indicator task.
   - Loads configuration via `config_store`.
   - Calls `wifi_manager_init(&config->wifi)`.
   - Calls `service_interface_init(...)`.
   - Calls `garage_control_init(...)`.
   - Optionally calls `mqtt_init(...)`.

All other logic should live in the submodules to keep `app` small and readable.
