#pragma once
#include <stdint.h>
/* Blink the onboard debug LED (Wemos D1 mini = GPIO2). Non-blocking.
 * mode: 0 = short (100 ms period), 1 = fast (500 ms period)
 * repeats: number of blinks, clipped to [1..20]
 */
typedef enum { GARAGE_LED_FAST = 0, GARAGE_LED_SLOW = 1 } garage_led_mode_t;
void garage_control_blink_debug_led(garage_led_mode_t mode, uint8_t repeats);
typedef struct { uint8_t mode; uint8_t repeats; } blink_params_t;