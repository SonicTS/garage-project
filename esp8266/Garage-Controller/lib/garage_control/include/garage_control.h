#pragma once

#include <stdint.h>
#include "debug_led.h"
#include "app_events.h"
#include "gpio_simple.h"
#include "config_store.h"
#include "mcp23017.h"
#include "log.h"


typedef enum {
    SENSOR_UNKNOWN = 0,
    SENSOR_CLOSED,
    SENSOR_PARTIALLY_OPEN,
    SENSOR_OPEN,
    SENSOR_ERROR
} garage_sensor_state_t;

typedef enum {
    GARAGE_CONTROL_IDLE = 0,
    GARAGE_CONTROL_OPENING,
    GARAGE_CONTROL_WAITING_TIMER_FOR_CLOSING,
    GARAGE_CONTROL_CLOSING
} garage_control_state_t;

typedef enum {
    GARAGE_STATE_UNKNOWN = 0,
    GARAGE_STATE_CLOSED,
    GARAGE_STATE_OPEN,
    GARAGE_STATE_PARTIALLY_OPEN,
    GARAGE_STATE_MOVING_UP,
    GARAGE_STATE_MOVING_DOWN,
    GARAGE_STATE_ERROR
} garage_state_t;

/* Initialize garage controller task and resources. */
void garage_control_init(void);

/* Commands (called by MQTT handler or UI). Non-blocking; only explicit open/close. */
void garage_control_command_open(void);
void garage_control_command_close(void);

/* Query current logical state (thread-safe snapshot). */
/* Query current logical state snapshot. */
garage_state_t garage_control_get_logical_state(void);

/* Update close-after seconds immediately (called after config change via MQTT).
 * 0 disables. If door currently OPEN and >0, restarts timer with new period.
 */
void garage_control_set_close_after_seconds(uint32_t seconds);

/* For low-level hardware integration: Weak hooks you may override in board-specific code.
 * By default these are stubs that do nothing (useful for simulation/testing).
 */
/* Internal sensor handling is private now; no public raw read hooks. */
void garage_hw_pulse_relay_open(void);  /* trigger motor/relay to open */
void garage_hw_pulse_relay_close(void); /* trigger motor/relay to close */

/* Return current logical door state */
garage_state_t garage_control_get_logical_state(void);

