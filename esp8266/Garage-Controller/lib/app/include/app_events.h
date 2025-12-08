#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

typedef enum {
    APP_EVT_WIFI_UP = 1,
    APP_EVT_WIFI_DOWN = 2,
    /* Garage related events */
    APP_EVT_GARAGE_STATE = 0x10,
    // add more events here later
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    union {
        struct {
            uint32_t ip;   // IPv4 address as uint32_t (network order)
        } wifi_up;
        struct {
            int state;    /* garage_state_t (see garage_control.h) */
            int control;  /* 0 = idle, 1 = active (opening/closing) */
        } garage_state;
    } data;
} app_evt_t;

// Created in app.c, used by modules (wifi_manager, service_interface, mqtt, ...)
extern xQueueHandle g_app_event_queue;
