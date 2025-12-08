#include "gpio_simple.h"
#include "esp_common.h"
#include "gpio.h"  /* Provided by the ESP8266 SDK */

/* Mapping table for supported GPIO pins: mux register + function selector */
typedef struct {
    uint8_t pin;
    uint32_t mux_reg;
    uint8_t func;
} gpio_map_t;

/* Pins 0-5, 12-15 (exclude flash pins 6-11) */
static const gpio_map_t s_map[] = {
    {0,  PERIPHS_IO_MUX_GPIO0_U,  FUNC_GPIO0},
    {1,  PERIPHS_IO_MUX_U0TXD_U,  FUNC_GPIO1},   /* if you ever use GPIO1 as GPIO */
    {2,  PERIPHS_IO_MUX_GPIO2_U,  FUNC_GPIO2},
    {3,  PERIPHS_IO_MUX_U0RXD_U,  FUNC_GPIO3},   /* <<< change to FUNC_GPIO3 */
    {4,  PERIPHS_IO_MUX_GPIO4_U,  FUNC_GPIO4},
    {5,  PERIPHS_IO_MUX_GPIO5_U,  FUNC_GPIO5},
    {12, PERIPHS_IO_MUX_MTDI_U,   FUNC_GPIO12},
    {13, PERIPHS_IO_MUX_MTCK_U,   FUNC_GPIO13},
    {14, PERIPHS_IO_MUX_MTMS_U,   FUNC_GPIO14},
    {15, PERIPHS_IO_MUX_MTDO_U,   FUNC_GPIO15},
};


static const gpio_map_t* find_pin(uint8_t pin){
    for (unsigned i=0; i<sizeof(s_map)/sizeof(s_map[0]); ++i){
        if (s_map[i].pin == pin) return &s_map[i];
    }
    return NULL;
}

static int select_func(const gpio_map_t *m){
    if(!m) return -1;
    PIN_FUNC_SELECT(m->mux_reg, m->func);
    return 0;
}

int gpio_simple_make_output(uint8_t pin, int initial_level)
{
    const gpio_map_t *m = find_pin(pin);
    if(!m) return -1;
    select_func(m);
    /* Enable pin as output */
    gpio_output_set(initial_level ? (1<<pin) : 0,
                    initial_level ? 0 : (1<<pin),
                    (1<<pin), /* enable mask */
                    0);
    return 0;
}

int gpio_simple_make_input(uint8_t pin, int enable_pullup)
{
    const gpio_map_t *m = find_pin(pin);
    if(!m) return -1;
    select_func(m);
    /* Disable output enable for this pin */
    gpio_output_set(0, 0, 0, (1<<pin));
    if(enable_pullup){ PIN_PULLUP_EN(m->mux_reg); } else { PIN_PULLUP_DIS(m->mux_reg); }
    return 0;
}

int gpio_simple_set(uint8_t pin, int level)
{
    const gpio_map_t *m = find_pin(pin);
    if(!m) return -1;
    if(level){
        gpio_output_set((1<<pin), 0, 0, 0);
    } else {
        gpio_output_set(0, (1<<pin), 0, 0);
    }
    return 0;
}

int gpio_simple_toggle(uint8_t pin)
{
    int v = gpio_simple_get(pin);
    if(v < 0) return -1;
    return gpio_simple_set(pin, !v);
}

int gpio_simple_get(uint8_t pin)
{
    const gpio_map_t *m = find_pin(pin);
    if(!m) return -1;
    /* Use SDK macro to read pin */
    return GPIO_INPUT_GET(pin) ? 1 : 0;
}

int gpio_simple_pullup_enable(uint8_t pin)
{
    const gpio_map_t *m = find_pin(pin);
    if(!m) return -1;
    PIN_PULLUP_EN(m->mux_reg);
    return 0;
}

int gpio_simple_pullup_disable(uint8_t pin)
{
    const gpio_map_t *m = find_pin(pin);
    if(!m) return -1;
    PIN_PULLUP_DIS(m->mux_reg);
    return 0;
}
