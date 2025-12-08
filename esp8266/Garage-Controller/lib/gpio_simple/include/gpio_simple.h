#pragma once
/* Simple GPIO helper for ESP8266 (RTOS SDK / NONOS common APIs).
 * Provides convenience functions to configure pins as input/output,
 * set/get/toggle level, and enable/disable internal pull-ups.
 *
 * NOTES:
 *  - Pins 6..11 are used for flash and should NOT be touched.
 *  - Pin 16 has a different register set and is not fully supported here.
 *  - This helper supports GPIOs: 0-5, 12-15. Attempts to use others return -1.
 */

#include <stdint.h>
#include "log.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Return 0 on success, -1 on invalid pin */
int gpio_simple_make_output(uint8_t pin, int initial_level);
int gpio_simple_make_input(uint8_t pin, int enable_pullup); /* enable_pullup: 0/1 */

int gpio_simple_set(uint8_t pin, int level); /* level: 0/1 */
int gpio_simple_toggle(uint8_t pin);
int gpio_simple_get(uint8_t pin);            /* returns 0/1 or -1 if invalid pin */

int gpio_simple_pullup_enable(uint8_t pin);
int gpio_simple_pullup_disable(uint8_t pin);

#ifdef __cplusplus
}
#endif
