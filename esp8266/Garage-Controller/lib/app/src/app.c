#include "app_events.h"

#include "app.h"
#include "wifi_manager.h"
#include "service_interface.h"
#include "mqtt.h"
#include "config_store.h"
#include "garage_control.h"
#include "debug_led.h"
#include "uart.h"

#include <string.h>

xQueueHandle g_app_event_queue = NULL;

/* Simple command handler: just logs the command for now. */
static void mqtt_command_handler(const char *topic,
                                 const char *payload,
                                 size_t payload_len)
{
    /* Some SDK LOGF variants don't support %.*s; copy and NUL-terminate. */
    char preview[128];
    size_t n = payload_len;
    if (n >= sizeof(preview)) n = sizeof(preview) - 1;
    if (n > 0 && payload) {
        memcpy(preview, payload, n);
    }
    preview[n] = '\0';
    LOGF("mqtt: command on topic \"%s\": %s\n", topic, preview);
    mqtt_client_publish_status("ack", "received");
    /* Interpret simple commands */
    if (strncmp(preview, "OPEN", 4) == 0 || strncmp(preview, "open", 4) == 0) {
        garage_control_command_open();
    } else if (strncmp(preview, "CLOSE_AFTER:", 12) == 0 || strncmp(preview, "close_after:", 12) == 0) {
        int seconds = atoi(preview + 12);
        if (seconds >= 0) {
            config_store_set_garage_close_after((uint32_t)seconds);
            garage_control_set_close_after_seconds((uint32_t)seconds);
            LOGF("app: close_after updated to %d seconds\n", seconds);
        }
        
    } else if (strncmp(preview, "CLOSE", 5) == 0 || strncmp(preview, "close", 5) == 0) {
        garage_control_command_close();
    } 
}

void init_uart_for_debug(void)
{
    UART_WaitTxFifoEmpty(UART0);
    UART_WaitTxFifoEmpty(UART1);

    UART_ConfigTypeDef uart_config;
    uart_config.baud_rate    = BIT_RATE_9600;
    uart_config.data_bits     = UART_WordLength_8b;
    uart_config.parity          = USART_Parity_None;
    uart_config.stop_bits     = USART_StopBits_1;
    uart_config.flow_ctrl      = USART_HardwareFlowControl_None;
    uart_config.UART_RxFlowThresh = 120;
    uart_config.UART_InverseMask = UART_None_Inverse;
    UART_ParamConfig(UART0, &uart_config);
    UART_SetPrintPort(UART0);
}

void app_start(void)
{
    /* Create global event queue used by all modules */
    init_uart_for_debug();
    vTaskDelay(100 / portTICK_RATE_MS);
    LOGF("\napp: UART initialized for debug\n");  
    LOGF("app: Starting app...\n");
    g_app_event_queue = xQueueCreate(10, sizeof(app_evt_t));
    LOGF("app: event queue created\n");
    config_store_init();
    LOGF("app: config store initialized\n");
    app_config_t cfg;
    config_store_load(&cfg);
    LOGF("app: config loaded\n");
    wifi_manager_init(&cfg.wifi);
    LOGF("app: wifi manager initialized\n");
    service_interface_init();
    LOGF("app: service interface initialized\n");
    mqtt_client_init(&cfg.mqtt, mqtt_command_handler);
    LOGF("app: mqtt client initialized\n");
    garage_control_blink_debug_led(GARAGE_LED_SLOW, 5);
    garage_control_init();
    garage_control_blink_debug_led(GARAGE_LED_SLOW, 5);
    LOGF("app: garage control initialized\n");
}
