#include "config_store.h"

#include "esp_common.h"
#include "spi_flash.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ------- Flash layout & format ------- */

/* Magic + version so we can detect valid data */
#define CONFIG_FLASH_MAGIC   0x47434647u  /* 'GCFG' */
#define CONFIG_FLASH_VERSION 2

/* One flash sector is 4 KB */
#ifndef SPI_FLASH_SEC_SIZE
#define SPI_FLASH_SEC_SIZE 4096
#endif

/* We reuse your existing function from main.c to know where RF cal lives.
 * That way we can choose our config sector just below the RF/SDK reserved area.
 */
extern uint32 user_rf_cal_sector_set(void);

/* Our on-flash record format */
typedef struct {
    uint32_t     magic;
    uint16_t     version;
    uint16_t     reserved;
    app_config_t cfg;
    uint32_t     checksum;   /* simple additive checksum over cfg */
} flash_config_record_t;

/* In-RAM copy of the config */
static app_config_t g_cfg;

/* Flash sector index where we store the config */
static uint16_t g_cfg_sector = 0xFFFF;

/* ------- Helpers ------- */

static uint32_t config_checksum(const app_config_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(app_config_t); ++i) {
        sum += p[i];
    }
    return sum;
}

static void set_default_config(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    /* WiFi defaults: empty -> forces AP fallback until user configures */
    strncpy(g_cfg.wifi.ssid,     "", sizeof(g_cfg.wifi.ssid) - 1);
    strncpy(g_cfg.wifi.password, "", sizeof(g_cfg.wifi.password) - 1);

    /* MQTT defaults (placeholder) */
    strncpy(g_cfg.mqtt.broker,   "mqtt.example.com", sizeof(g_cfg.mqtt.broker) - 1);
    g_cfg.mqtt.port = 8883;
    strncpy(g_cfg.mqtt.client_id, "garage-esp", sizeof(g_cfg.mqtt.client_id) - 1);
    g_cfg.mqtt.username[0] = '\0';
    g_cfg.mqtt.password[0] = '\0';
    strncpy(g_cfg.mqtt.base_topic, "garage/door1", sizeof(g_cfg.mqtt.base_topic) - 1);
    g_cfg.mqtt.use_tls = 1;

    /* Garage defaults */
    g_cfg.garage.close_after_seconds = 0; /* disabled by default */

    LOGF("config_store: using default config\n");
}

/* Decide which flash sector to use for config.
 * We put it JUST BELOW the RF calibration / SDK reserved area.
 */
static void init_config_sector(void)
{
    if (g_cfg_sector != 0xFFFF) return; /* already set */

    uint32 rf_cal_sec = user_rf_cal_sector_set();
    if (rf_cal_sec <= 3) {
        /* Extremely unlikely, but avoid underflow */
        g_cfg_sector = 16;
    } else {
        /* system reserves 5 sectors at the top for RF/SDK params.
         * user_rf_cal_sector_set() returns the first of those 5.
         * We take 1 sector just below that region.
         */
        g_cfg_sector = (uint16_t)(rf_cal_sec - 1);
    }

    LOGF("config_store: using flash sector %u for config\n", g_cfg_sector);
}

/* Try to load config from flash into g_cfg.
 * Returns 1 if OK, 0 if invalid or error.
 */
static int load_from_flash(void)
{
    flash_config_record_t rec;
    uint32_t addr;
    SpiFlashOpResult r;

    init_config_sector();
    addr = g_cfg_sector * SPI_FLASH_SEC_SIZE;

    /* Read whole record (size is multiple of 4, aligned) */
    r = spi_flash_read(addr, (uint32_t *)&rec, sizeof(rec));
    if (r != SPI_FLASH_RESULT_OK) {
        LOGF("config_store: spi_flash_read failed (%d)\n", r);
        return 0;
    }

    if (rec.magic != CONFIG_FLASH_MAGIC) {
        LOGF("config_store: bad magic (0x%08x)\n", (unsigned)rec.magic);
        return 0;
    }
    if (rec.version != CONFIG_FLASH_VERSION) {
        LOGF("config_store: version mismatch (got %u, expect %u)\n",
               rec.version, CONFIG_FLASH_VERSION);
        return 0;
    }

    uint32_t chk = config_checksum(&rec.cfg);
    if (chk != rec.checksum) {
        LOGF("config_store: checksum mismatch (0x%08x vs 0x%08x)\n",
               (unsigned)chk, (unsigned)rec.checksum);
        return 0;
    }

    g_cfg = rec.cfg;
    LOGF("config_store: loaded config from flash\n");
    return 1;
}

/* Save current g_cfg (or provided cfg) to flash */
static void save_to_flash(const app_config_t *cfg)
{
    flash_config_record_t rec;
    uint32_t addr;
    SpiFlashOpResult r;

    if (!cfg) cfg = &g_cfg;

    init_config_sector();
    addr = g_cfg_sector * SPI_FLASH_SEC_SIZE;

    memset(&rec, 0, sizeof(rec));
    rec.magic    = CONFIG_FLASH_MAGIC;
    rec.version  = CONFIG_FLASH_VERSION;
    rec.cfg      = *cfg;
    rec.checksum = config_checksum(&rec.cfg);

    /* Erase sector before writing */
    r = spi_flash_erase_sector(g_cfg_sector);
    if (r != SPI_FLASH_RESULT_OK) {
        LOGF("config_store: spi_flash_erase_sector failed (%d)\n", r);
        return;
    }

    r = spi_flash_write(addr, (uint32_t *)&rec, sizeof(rec));
    if (r != SPI_FLASH_RESULT_OK) {
        LOGF("config_store: spi_flash_write failed (%d)\n", r);
        return;
    }

    LOGF("config_store: saved config to flash (sector %u)\n", g_cfg_sector);
}

/* ------- Public API ------- */

void config_store_init(void)
{
    init_config_sector();

    if (!load_from_flash()) {
        /* Flash invalid or empty: use defaults and save them */
        set_default_config();
        /* Do NOT write defaults at boot: avoid erasing a possibly
           unsafe sector if flash map assumptions are wrong. We'll
           persist only when the user updates config explicitly. */
    }
}

void config_store_load(app_config_t *cfg_out)
{
    if (!cfg_out) return;
    *cfg_out = g_cfg;
}

void config_store_save(const app_config_t *cfg)
{
    if (cfg) {
        g_cfg = *cfg;
    }
    save_to_flash(&g_cfg);
}

void config_store_get_wifi(wifi_config_t *wifi_out)
{
    if (!wifi_out) return;
    *wifi_out = g_cfg.wifi;
}

void config_store_set_wifi(const wifi_config_t *wifi_in)
{
    if (!wifi_in) return;
    g_cfg.wifi = *wifi_in;
    save_to_flash(&g_cfg);
}

void config_store_get_garage(uint32_t *close_after_seconds_out)
{
    if (!close_after_seconds_out) return;
    *close_after_seconds_out = g_cfg.garage.close_after_seconds;
}

void config_store_set_garage_close_after(uint32_t close_after_seconds)
{
    g_cfg.garage.close_after_seconds = close_after_seconds;
    save_to_flash(&g_cfg);
}
