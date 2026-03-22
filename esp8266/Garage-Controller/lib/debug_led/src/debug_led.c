#include "debug_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_common.h"

static void gc_led_task(void *pv);

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