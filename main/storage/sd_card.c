#include <string.h>
#include "sd_card.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"

static const char *TAG = "sd_card";
static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_init(void)
{
    /* Barramento SPI3 dedicado ao SD (pinos nativos do VSPI: 18/19/23) */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = SD_PIN_MOSI,
        .miso_io_num   = SD_PIN_MISO,
        .sclk_io_num   = SD_PIN_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "falha ao montar SD: %s — siga sem cartão", esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

void sd_card_get_usage(uint64_t *total, uint64_t *free_b)
{
    *total = 0;
    *free_b = 0;
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_info(SD_MOUNT_POINT, total, free_b);
}
