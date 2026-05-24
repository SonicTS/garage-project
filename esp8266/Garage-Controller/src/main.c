#include "app.h"
#include "esp_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log.h"

#include <string.h>
#include <stdio.h>


/******** RF CAL (required) ********/
void ICACHE_FLASH_ATTR user_rf_pre_init(void)
{
    /* Critical: Force full RF calibration on every boot for stability.
       This is especially important for cheap ESP8266 modules with poor RF. */
    /* Use supported API to request RF calibration behavior after power-up */
    system_phy_set_rfoption(1);
    /* Additional: Set max RF TX power (82 = 20.5dBm) for better range */
    system_phy_set_max_tpw(82);
}

uint32 ICACHE_FLASH_ATTR user_rf_cal_sector_set(void)
{
    flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0;

    switch(size_map) {
        case FLASH_SIZE_4M_MAP_256_256:        rf_cal_sec = 128 - 5; break;
        case FLASH_SIZE_8M_MAP_512_512:        rf_cal_sec = 256 - 5; break;
        case FLASH_SIZE_16M_MAP_512_512:
        case FLASH_SIZE_16M_MAP_1024_1024:     rf_cal_sec = 512 - 5; break;
        case FLASH_SIZE_32M_MAP_512_512:
        case FLASH_SIZE_32M_MAP_1024_1024:     rf_cal_sec = 1024 - 5; break;
        case FLASH_SIZE_64M_MAP_1024_1024:     rf_cal_sec = 2048 - 5; break;
        case FLASH_SIZE_128M_MAP_1024_1024:    rf_cal_sec = 4096 - 5; break;
        default: rf_cal_sec = 0; break;
    }
    return rf_cal_sec;
}

/******** Entrypoint ********/
void user_init(void)
{
    
    uart_div_modify(0, UART_CLK_FREQ / 9600);
    LOGF("Booting...\n");
    flash_size_map sm = system_get_flash_size_map();
    uint32 rfsec = user_rf_cal_sector_set();
    LOGF("Flash map=%d, rf_cal_sector=%u\n", (int)sm, (unsigned)rfsec);
    struct rst_info *ri = system_get_rst_info();
    if (ri) {
        LOGF("Reset reason: %d, exccause=%d, epc1=0x%08x\n",
                ri->reason, ri->exccause, ri->epc1);
    }
    
    app_start();
}
