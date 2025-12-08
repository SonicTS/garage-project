#include "garage_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_common.h"
#include <stdio.h>
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* Internal state */
static garage_sensor_state_t g_sensor_state = SENSOR_UNKNOWN;
static garage_control_state_t g_control_state = GARAGE_CONTROL_IDLE;
static garage_state_t g_logical_state = GARAGE_STATE_UNKNOWN;
static uint32_t g_pending_close_seconds = 0; /* 0 = no pending timer */
static uint32_t g_move_start_ms = 0;          /* timestamp of movement start */
static const uint32_t MOVE_TIMEOUT_MS = 30000; /* max allowed move duration */

static xTimerHandle g_close_timer = NULL;
static xTaskHandle g_gc_task = NULL;
static xQueueHandle g_cmd_queue = NULL;

/* Command queue message */
typedef enum { GC_CMD_OPEN, GC_CMD_CLOSE } gc_cmd_t;



typedef struct { gc_cmd_t cmd; uint32_t arg; } gc_cmd_msg_t;
typedef struct { uint8_t mode; uint8_t repeats; } blink_params_t;

/* Pin Assignments*/

const int PIN_REED_SWT_BOTTOM = 14;  // D5
const int PIN_REED_SWT_MIDDLE = 13; // D7
const int PIN_REED_SWT_TOP = 3; // RX
const int PIN_STOP_BTN = 5; // D1
const int PIN_UP_BTN = 4; // D2
const int PIN_DOWN_BTN = 12; // D6
const int TOGGLE_COUNTS = 4;

/* Forward declarations */
static void gc_task(void *pv);
static void close_timer_cb(xTimerHandle t);
static void gc_led_task(void *pv);

/* Weak hardware hooks -- user can provide board-specific implementations.
 * Default implementations are provided here to keep code linkable.
 */

void toggle_pin(int pin)
{
    for (size_t i = 0; i < TOGGLE_COUNTS * 2; i++)
    {
        gpio_simple_toggle(pin);
        vTaskDelay(250 / portTICK_RATE_MS);
    }
}

void garage_hw_pulse_relay_open(void) { 
    LOGF("garage_hw: pulse open (stub)\n"); 
    gpio_simple_make_output(PIN_STOP_BTN, 1);
    gpio_simple_make_output(PIN_UP_BTN, 1);
    toggle_pin(PIN_STOP_BTN);
    toggle_pin(PIN_UP_BTN);
    gpio_simple_make_input(PIN_STOP_BTN, 1);
    gpio_simple_make_input(PIN_UP_BTN, 1);
    garage_control_blink_debug_led(GARAGE_LED_FAST, 10);
}
void garage_hw_pulse_relay_close(void) { 
    LOGF("garage_hw: pulse close (stub)\n"); 
    gpio_simple_make_output(PIN_STOP_BTN, 1);
    gpio_simple_make_output(PIN_DOWN_BTN, 1);
    toggle_pin(PIN_STOP_BTN);
    toggle_pin(PIN_DOWN_BTN);
    gpio_simple_make_input(PIN_STOP_BTN, 1);
    gpio_simple_make_input(PIN_DOWN_BTN, 1);
    garage_control_blink_debug_led(GARAGE_LED_SLOW, 2);
}

/* Optional hook to set the debug LED state. Override in board code if needed.
 * Default implementation attempts to control GPIO2 on Wemos D1 mini (active LOW).
 */
void garage_hw_set_debug_led(int on)
{
    /* Configure pinmux for GPIO2 */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
    if (on) {
        /* Drive LOW to turn the LED on (active LOW) */
        gpio_output_set(0, (1 << 2), (1 << 2), 0);
    } else {
        /* Drive HIGH to turn the LED off */
        gpio_output_set((1 << 2), 0, (1 << 2), 0);
    }
}

garage_sensor_state_t garage_read_sensor(void) { 
    static int prev_b=-1, prev_m=-1, prev_t=-1; /* last raw sample */
    static int same_count = 0;                  /* how many identical in a row */
    enum { DEBOUNCE_COUNT = 2 }; /* consecutive identical samples */
    int b = gpio_simple_get(PIN_REED_SWT_BOTTOM);
    int m = gpio_simple_get(PIN_REED_SWT_MIDDLE);
    int t = gpio_simple_get(PIN_REED_SWT_TOP);
    //LOGF("garage_hw: sensor read b=%d m=%d t=%d\n", b, m, t);
    static garage_sensor_state_t stable = SENSOR_UNKNOWN; /* last stable composite */

    if (b==prev_b && m==prev_m && t==prev_t) {
        if (same_count < 255) same_count++;
    } else {
        prev_b=b; prev_m=m; prev_t=t;
        same_count = 0;
    }
    if (same_count >= DEBOUNCE_COUNT) {
        if (b && !m && !t)       stable = SENSOR_CLOSED;
        else if (t && !m && !b)  stable = SENSOR_OPEN;
        else if (m && !b && !t)  stable = SENSOR_PARTIALLY_OPEN;
        else if (!b && !m && !t) stable = SENSOR_UNKNOWN;
        else                     stable = SENSOR_ERROR;
    }
    return stable;
}


/* Helpers to emit app event */
static void emit_state_event(void)
{
    if (!g_app_event_queue) return;
    app_evt_t ev;
    ev.type = APP_EVT_GARAGE_STATE;
    ev.data.garage_state.state = (int)g_logical_state; /* publish logical state enum */
    int active = (g_control_state == GARAGE_CONTROL_OPENING || g_control_state == GARAGE_CONTROL_CLOSING) ? 1 : 0;
    ev.data.garage_state.control = active;
    /* Try to enqueue; wait up to 10ms if the queue is momentarily full. */
    int r = xQueueSend(g_app_event_queue, &ev, (10 / portTICK_RATE_MS));
    if (r != pdTRUE) { LOGF("garage_control: event queue full, dropping event\n"); }
}

void gpio_init(void)
{
    // Initialize GPIO pins
    
    int err = gpio_simple_make_input(PIN_REED_SWT_BOTTOM, 1);
    err += gpio_simple_make_input(PIN_REED_SWT_MIDDLE, 1);
    err += gpio_simple_make_input(PIN_REED_SWT_TOP, 1);
    err += gpio_simple_make_input(PIN_STOP_BTN, 1);
    err += gpio_simple_make_input(PIN_UP_BTN, 1);
    err += gpio_simple_make_input(PIN_DOWN_BTN, 1);
    LOGF("GPIO init result: %d\n", err);
}

void garage_control_init(void)
{
    if (g_gc_task) return;
    gpio_init();
    g_close_timer = xTimerCreate("gc_close", (1000 / portTICK_RATE_MS), pdFALSE, NULL, close_timer_cb);
    g_cmd_queue = xQueueCreate(6, sizeof(gc_cmd_msg_t));
    /* Load persisted close-after seconds */
    uint32_t persisted = 0;
    config_store_get_garage(&persisted);
    g_pending_close_seconds = persisted;
    xTaskCreate(gc_task, "garage_ctrl", 512, NULL, 3, &g_gc_task);
    LOGF("garage_control: initialized (task=%p)\n", g_gc_task);
}

/* API: push commands to queue so handlers don't block other tasks. */
void garage_control_command_open(void)
{
    if (!g_cmd_queue) return;
    gc_cmd_msg_t m = { .cmd = GC_CMD_OPEN, .arg = 0 };
    xQueueSend(g_cmd_queue, &m, 0);
}

void garage_control_command_close(void)
{
    if (!g_cmd_queue) return;
    gc_cmd_msg_t m = { .cmd = GC_CMD_CLOSE, .arg = 0 };
    xQueueSend(g_cmd_queue, &m, 0);
}

/* Blink API: spawn a short-lived task to blink the onboard debug LED.
 * mode: GARAGE_LED_FAST(100ms) or GARAGE_LED_SLOW(500ms)
 * repeats: clipped to [1..20]
 */
void garage_control_blink_debug_led(garage_led_mode_t mode, uint8_t repeats)
{
    if (repeats == 0) repeats = 1;
    if (repeats > 20) repeats = 20;

    blink_params_t *p = malloc(sizeof(*p));
    if (!p) return;
    p->mode = (uint8_t)mode;
    p->repeats = repeats;

    if (xTaskCreate(gc_led_task, "gc_led", 384, p, 2, NULL) != pdPASS) {
        free(p);
    }
}

static void gc_led_task(void *pv)
{
    blink_params_t *p = (blink_params_t *)pv;
    if (!p) { vTaskDelete(NULL); return; }

    const int period_ms = (p->mode == GARAGE_LED_FAST) ? 100 : 500;
    for (uint8_t i = 0; i < p->repeats; ++i) {
        garage_hw_set_debug_led(1);
        vTaskDelay(period_ms / portTICK_RATE_MS);
        garage_hw_set_debug_led(0);
        vTaskDelay(period_ms / portTICK_RATE_MS);
    }
    free(p);
    vTaskDelete(NULL);
}

static void start_open_sequence(void)
{
    if (g_control_state == GARAGE_CONTROL_OPENING || g_control_state == GARAGE_CONTROL_CLOSING) return;
    g_control_state = GARAGE_CONTROL_OPENING;
    g_logical_state = GARAGE_STATE_MOVING_UP;
    g_move_start_ms = xTaskGetTickCount() * portTICK_RATE_MS;
    emit_state_event();
    garage_hw_pulse_relay_open();
}

static void start_close_sequence(void)
{
    if (g_control_state == GARAGE_CONTROL_CLOSING || g_control_state == GARAGE_CONTROL_OPENING) return;
    g_control_state = GARAGE_CONTROL_CLOSING;
    g_logical_state = GARAGE_STATE_MOVING_DOWN;
    g_move_start_ms = xTaskGetTickCount() * portTICK_RATE_MS;
    emit_state_event();
    garage_hw_pulse_relay_close();
}

static void close_timer_cb(xTimerHandle t)
{
    /* Timer fired: enqueue a close command */
    if (!g_cmd_queue) return;
    gc_cmd_msg_t m = { .cmd = GC_CMD_CLOSE, .arg = 0 };
    xQueueSend(g_cmd_queue, &m, 0);
}

garage_state_t garage_control_get_logical_state(void) { return g_logical_state; }

void garage_control_set_close_after_seconds(uint32_t seconds)
{
    g_pending_close_seconds = seconds;
    /* When updated, adjust timer only if door presently OPEN */
    if (g_logical_state == GARAGE_STATE_OPEN) {
        if (seconds > 0) {
            if (xTimerIsTimerActive(g_close_timer) == pdTRUE) {
                xTimerChangePeriod(g_close_timer, (seconds * 1000) / portTICK_RATE_MS, 0);
            } else {
                xTimerChangePeriod(g_close_timer, (seconds * 1000) / portTICK_RATE_MS, 0);
                xTimerStart(g_close_timer, 0);
            }
        } else {
            if (xTimerIsTimerActive(g_close_timer) == pdTRUE) {
                xTimerStop(g_close_timer, 0);
            }
        }
    } else {
        /* Not currently open: stop timer if disabling; if enabling keep it idle until OPEN */
        if (seconds == 0 && xTimerIsTimerActive(g_close_timer) == pdTRUE) {
            xTimerStop(g_close_timer, 0);
        }
    }
}

static void gc_task(void *pv)
{
    const int POLL_MS = 200;
    garage_state_t prev_logical = g_logical_state;
    uint32_t loop_cnt = 0;

    for (;;) {
        /* Process any pending commands quickly */
        gc_cmd_msg_t m;
        while (xQueueReceive(g_cmd_queue, &m, 0) == pdTRUE) {
            switch (m.cmd) {
            case GC_CMD_OPEN:
                start_open_sequence();
                break;
            case GC_CMD_CLOSE:
                // When issueing a close command the sensor state open should not trigger the close timer for another x seconds
                start_close_sequence();
                break;
            }
        }

        /* Poll debounced sensor state */
        garage_sensor_state_t g_sensor_state = garage_read_sensor();
        {
            /* Logical state transitions (use current debounced sensor state) */
            uint32_t now_ms = xTaskGetTickCount() * portTICK_RATE_MS;
            if (g_control_state == GARAGE_CONTROL_OPENING) {
                if (g_sensor_state == SENSOR_OPEN) {
                    LOGF("garage_control: door reached OPEN position\n");
                    g_control_state = GARAGE_CONTROL_IDLE;
                    g_logical_state = GARAGE_STATE_OPEN;
                } else if (now_ms - g_move_start_ms > MOVE_TIMEOUT_MS) {
                    LOGF("garage_control: OPEN timeout - ERROR\n");
                    g_control_state = GARAGE_CONTROL_IDLE;
                    g_logical_state = GARAGE_STATE_ERROR;
                } else {
                    LOGF("garage_control: door moving UP\n");
                    g_logical_state = GARAGE_STATE_MOVING_UP;
                }
            } else if (g_control_state == GARAGE_CONTROL_CLOSING) {
                if (g_sensor_state == SENSOR_CLOSED) {
                    LOGF("garage_control: door reached CLOSED position\n");
                    g_control_state = GARAGE_CONTROL_IDLE;
                    g_logical_state = GARAGE_STATE_CLOSED;
                } else if (now_ms - g_move_start_ms > MOVE_TIMEOUT_MS) {
                    LOGF("garage_control: CLOSE timeout - ERROR\n");
                    g_control_state = GARAGE_CONTROL_IDLE;
                    g_logical_state = GARAGE_STATE_ERROR;
                } else {
                    LOGF("garage_control: door moving DOWN\n");
                    g_logical_state = GARAGE_STATE_MOVING_DOWN;
                }
            } else { /* idle */
                //LOGF("sensor state: %d\n", g_sensor_state);
                switch (g_sensor_state) {
                case SENSOR_OPEN:          g_logical_state = GARAGE_STATE_OPEN; break;
                case SENSOR_CLOSED:        g_logical_state = GARAGE_STATE_CLOSED; break;
                case SENSOR_PARTIALLY_OPEN:g_logical_state = GARAGE_STATE_PARTIALLY_OPEN; break;
                case SENSOR_ERROR:         g_logical_state = GARAGE_STATE_ERROR; break;
                case SENSOR_UNKNOWN:       g_logical_state = GARAGE_STATE_UNKNOWN; break;
                }
            }

            /* Auto close timer handling */
            //LOGF("garage_control: logical state=%d, pending_close_seconds=%d\n", g_logical_state, g_pending_close_seconds);
            if (g_logical_state == GARAGE_STATE_OPEN && g_pending_close_seconds > 0) {
                if (xTimerIsTimerActive(g_close_timer) == pdFALSE) {
                    LOGF("garage_control: starting auto-close timer (%d seconds)\n", g_pending_close_seconds);
                    xTimerChangePeriod(g_close_timer, (g_pending_close_seconds * 1000) / portTICK_RATE_MS, 0);
                    xTimerStart(g_close_timer, 0);
                }
            } else {
                if (xTimerIsTimerActive(g_close_timer) == pdTRUE && g_pending_close_seconds > 0) {
                    LOGF("garage_control: stopping auto-close timer\n");
                    xTimerStop(g_close_timer, 0);
                }
            }

            if (prev_logical != g_logical_state) {
                emit_state_event();
                prev_logical = g_logical_state;
            }
        }

        vTaskDelay(POLL_MS / portTICK_RATE_MS);
        if ((++loop_cnt & 0x3F) == 0) { LOG_STACK("garage_ctrl"); }
    }
}
