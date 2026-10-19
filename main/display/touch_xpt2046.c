#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#include "app_config.h"
#include "touch_xpt2046.h"

static const char *TAG = "touch";

/* Comandos XPT2046 (12 bits, diferencial, power-down entre conversões) */
#define CMD_Z1  0xB1
#define CMD_Z2  0xC1
#define CMD_X   0xD1
#define CMD_Y   0x91

/* Calibração afim ativa (default = mapeamento linear da placa) */
static float s_cal[6];
static bool s_cal_active = false;

/* ---------- SPI software (modo 0: amostra na subida do clock) ---------- */

static inline void clk_pulse(void)
{
    gpio_set_level(TOUCH_PIN_CLK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(TOUCH_PIN_CLK, 0);
    esp_rom_delay_us(1);
}

static void spi_write8(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(TOUCH_PIN_MOSI, (b >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(TOUCH_PIN_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(TOUCH_PIN_CLK, 0);
        esp_rom_delay_us(1);
    }
}

/* Lê 16 bits após o comando: [busy | D11..D0 | pad] -> retorna 12 bits */
static uint16_t spi_xfer12(uint8_t cmd)
{
    gpio_set_level(TOUCH_PIN_CS, 0);
    esp_rom_delay_us(2);
    spi_write8(cmd);
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        gpio_set_level(TOUCH_PIN_CLK, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | (gpio_get_level(TOUCH_PIN_MISO) & 1);
        gpio_set_level(TOUCH_PIN_CLK, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level(TOUCH_PIN_CS, 1);
    esp_rom_delay_us(2);
    return (v >> 3) & 0x0FFF;
}

/* ---------- API ---------- */

esp_err_t xpt2046_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_CLK) | (1ULL << TOUCH_PIN_MOSI) |
                        (1ULL << TOUCH_PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_MISO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    gpio_set_level(TOUCH_PIN_CS, 1);
    gpio_set_level(TOUCH_PIN_CLK, 0);

    /* descarta primeiras leituras (estabilização) */
    for (int i = 0; i < 3; i++) {
        spi_xfer12(CMD_X);
        spi_xfer12(CMD_Y);
    }
    ESP_LOGI(TAG, "XPT2046 inicializado (SPI software)");
    return ESP_OK;
}

bool xpt2046_read_raw(uint16_t *rx, uint16_t *ry)
{
    uint16_t z1 = spi_xfer12(CMD_Z1);
    uint16_t z2 = spi_xfer12(CMD_Z2);
    /* pressão: quanto maior z1 e menor z2, mais forte o toque */
    uint32_t z = z1 + (TOUCH_ADC_MAX - z2);
    if (z < TOUCH_Z_THRESHOLD) {
        return false;
    }

    /* primeira leitura de X costuma vir suja: descarta */
    spi_xfer12(CMD_X);

    uint32_t sx = 0, sy = 0;
    int ok = 0;
    for (int i = 0; i < TOUCH_SAMPLES_AVG * 2 && ok < TOUCH_SAMPLES_AVG; i++) {
        uint16_t x = spi_xfer12(CMD_X);
        uint16_t y = spi_xfer12(CMD_Y);
        if (x < 20 || x > TOUCH_ADC_MAX - 20 || y < 20 || y > TOUCH_ADC_MAX - 20) {
            continue;    /* amostra fora de faixa: ruído de borda */
        }
        sx += x;
        sy += y;
        ok++;
    }
    if (ok == 0) {
        return false;
    }
    *rx = sx / ok;
    *ry = sy / ok;
    return true;
}

void xpt2046_set_calibration(const float c[6])
{
    if (c) {
        memcpy(s_cal, c, sizeof(s_cal));
        s_cal_active = true;
    } else {
        s_cal_active = false;
    }
}

bool xpt2046_read_full(uint16_t *x, uint16_t *y, uint16_t *rx, uint16_t *ry)
{
    uint16_t raw_x, raw_y;
    if (!xpt2046_read_raw(&raw_x, &raw_y)) {
        return false;
    }
    if (rx) *rx = raw_x;
    if (ry) *ry = raw_y;

    int sx, sy;
    if (s_cal_active) {
        /* transformação afim calibrada */
        sx = (int)(s_cal[0] * raw_x + s_cal[1] * raw_y + s_cal[2]);
        sy = (int)(s_cal[3] * raw_x + s_cal[4] * raw_y + s_cal[5]);
    } else {
        /* mapeamento padrão: 12 bits -> pixels, com swap/espelhos da placa */
        int a = raw_x, b = raw_y;
#if TOUCH_SWAP_XY
        int t = a; a = b; b = t;
#endif
        sx = a * LCD_H_RES / TOUCH_ADC_MAX;
        sy = b * LCD_V_RES / TOUCH_ADC_MAX;
#if TOUCH_MIRROR_X
        sx = LCD_H_RES - 1 - sx;
#endif
#if TOUCH_MIRROR_Y
        sy = LCD_V_RES - 1 - sy;
#endif
    }

    if (sx < 0) sx = 0;
    if (sx > LCD_H_RES - 1) sx = LCD_H_RES - 1;
    if (sy < 0) sy = 0;
    if (sy > LCD_V_RES - 1) sy = LCD_V_RES - 1;
    *x = (uint16_t)sx;
    *y = (uint16_t)sy;
    return true;
}

bool xpt2046_read(uint16_t *x, uint16_t *y)
{
    return xpt2046_read_full(x, y, NULL, NULL);
}
