#pragma once

#include <stddef.h>
#include "config_store.h"
#include "app_events.h"
#include "garage_control.h"
#include "log.h"


/* Called when a command message is received on the command topic. */
typedef void (*mqtt_command_cb_t)(const char *topic,
                                  const char *payload,
                                  size_t payload_len);

/* Initialize MQTT client and start MQTT task.
 *  - cfg: MQTT config loaded from config_store
 *  - cb : callback when a command arrives
 */
void mqtt_client_init(const mqtt_config_t *cfg, mqtt_command_cb_t cb);

/* Publish a status message to base_topic + "/" + subtopic.
 *  Example: base_topic="garage/door1", subtopic="status" ->
 *           topic "garage/door1/status"
 *
 *  NOTE: For this first version, this is intended to be called from
 *        inside the MQTT task or from a single context. For a fully
 *        multi-task-safe version, you'd add a FreeRTOS queue.
 */
void mqtt_client_publish_status(const char *subtopic, const char *payload);

/* Get current MQTT runtime config (copy). */
void mqtt_get_config(mqtt_config_t *out);

/* Update MQTT config, persist to config_store, and request reconnect. */
void mqtt_update_config(const mqtt_config_t *in);
