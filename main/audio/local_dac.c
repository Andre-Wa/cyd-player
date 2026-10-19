#include <string.h>
#include "local_dac.h"
#include "app_config.h"
#include "driver/dac_continuous.h"
#include "esp_log.h"

static const char *TAG = "local_dac";
static dac_continuous_handle_t s_dac = NULL;
static bool s_enabled = false;

esp_err_t local_dac_init(void)
{
    dac_continuous_config_t cfg = {
        .chan_mask  = DAC_CHANNEL_MASK_CH1,      /* GPIO26 -> SC8002D */
        .desc_num   = 4,
        .buf_size   = 512,
        .freq_hz    = AUDIO_SAMPLE_RATE,         /* 44,1 kHz */
        .offset     = 0,
        /* PLL_D2 com divisor fracionário: erro < 0,1 % em 44,1 kHz, mais que
         * suficiente para a saída local (a rota BT é cadenciada pelo sink).
         * Evita a dependência da APLL. */
        .clk_src    = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode  = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t err = dac_continuous_new_channels(&cfg, &s_dac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_channels: %s", esp_err_to_name(err));
        return err;
    }
    err = dac_continuous_enable(s_dac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable: %s", esp_err_to_name(err));
        return err;
    }
    /* nível médio: amplificador em repouso sem "pop" */
    uint8_t mid[256];
    memset(mid, 0x80, sizeof(mid));
    size_t loaded = 0;
    dac_continuous_write(s_dac, mid, sizeof(mid), &loaded, 100);
    s_enabled = true;
    ESP_LOGI(TAG, "DAC contínuo @%d Hz em GPIO26 (mono 8 bits)", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t local_dac_write(const uint8_t *mono8, size_t len, int timeout_ms)
{
    if (!s_dac || !s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t loaded = 0;
    return dac_continuous_write(s_dac, (uint8_t *)mono8, len, &loaded, timeout_ms);
}

void local_dac_enable(bool on)
{
    if (!s_dac || s_enabled == on) {
        return;
    }
    if (on) {
        dac_continuous_enable(s_dac);
    } else {
        dac_continuous_disable(s_dac);
    }
    s_enabled = on;
}
