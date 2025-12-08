#pragma once
#include "config_store.h"
#include "wifi_manager.h"
#include "log.h"

/* Initialize/start HTTP service interface (socket based). */
void service_interface_init(void);

/* Explicit control for AP-mode gating */
void service_interface_start(void);
void service_interface_stop(void);
