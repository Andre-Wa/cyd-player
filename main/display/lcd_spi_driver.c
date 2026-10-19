#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "app_config.h"
#include "lcd_spi_driver.h"
#include "nvs_config.h"

static const char *TAG = "lcd";

static esp_lcd_panel_io_handle_t s_io = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;   /* contador de conclusões */
static volatile int s_pending = 0;             /* transmissões em voo */

/* Comandos ILI9341 */
#define LCD_CMD_SWRESET 0x01
#define LCD_CMD_SLPOUT  0x11
#define LCD_CMD_INVOFF  0x20
#define LCD_CMD_INVON   0x21
#define LCD_CMD_DISPON  0x29
#define LCD_CMD_CASET   0x2A
#define LCD_CMD_RASET   0x2B
#define LCD_CMD_RAMWR   0x2C
#define LCD_CMD_MADCTL  0x36
#define LCD_CMD_COLMOD  0x3A

/* Sequência de inicialização do fabricante (baseada no driver ILI9341 do
 * projeto esp32-smartdisplay, validada para o painel do CYD). */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_cmds[] = {
    {0xCF, {0x00, 0xAA, 0xE0}, 3, 0},
    {0xED, {0x67, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, {0x8A, 0x01, 0x78}, 3, 0},
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, {0x20}, 1, 0},
    {0xEA, {0x00, 0x00}, 2, 0},
    {0xC0, {0x23}, 1, 0},                  /* Power control 1 */
    {0xC1, {0x11}, 1, 0},                  /* Power control 2 */
    {0xC5, {0x43, 0x4C}, 2, 0},            /* VCOM 1 */
    {0xC7, {0xA0}, 1, 0},                  /* VCOM 2 */
    {0xB1, {0x00, 0x1B}, 2, 0},            /* Frame rate ~70 Hz */
    {0xF2, {0x00}, 1, 0},                  /* 3Gamma off */
    {0x26, {0x01}, 1, 0},                  /* Gamma curve 1 */
    {0xE0, {0x1F, 0x36, 0x36, 0x3A, 0x0C, 0x05, 0x4F, 0x87,
            0x3C, 0x08, 0x11, 0x35, 0x19, 0x13, 0x00}, 15, 0},
    {0xE1, {0x00, 0x09, 0x09, 0x05, 0x13, 0x0A, 0x30, 0x78,
            0x43, 0x07, 0x0E, 0x0A, 0x26, 0x2C, 0x1F}, 15, 0},
    {0xB7, {0x07}, 1, 0},                  /* Entry mode */
    {0xB6, {0x08, 0x82, 0x27}, 3, 0},      /* Display function */
};

/* -------- callback de conclusão de transmissão de cor -------- */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
        esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t hp_woken = pdFALSE;
    if (s_flush_sem) {
        xSemaphoreGiveFromISR(s_flush_sem, &hp_woken);
    }
    return hp_woken == pdTRUE;
}

/* -------- backlight (LEDC PWM, GPIO21) -------- */
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void lcd_set_backlight(uint8_t pct)
{
    uint32_t duty = (pct > 100 ? 100 : pct) * 1023 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t lcd_init(void)
{
    s_flush_sem = xSemaphoreCreateCounting(8, 0);
    if (!s_flush_sem) {
        return ESP_ERR_NO_MEM;
    }

    /* SPI2 (HSPI) com pinos nativos: MOSI=13 MISO=12 SCLK=14 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_STRIP_BYTES * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                        &io_cfg, &s_io), TAG, "panel io");

    /* Reset por software + sequência do fabricante */
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        esp_lcd_panel_io_tx_param(s_io, s_init_cmds[i].cmd,
                                  s_init_cmds[i].data, s_init_cmds[i].len);
        if (s_init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_init_cmds[i].delay_ms));
        }
    }
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_MADCTL, (uint8_t[]){LCD_MADCTL}, 1);
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_COLMOD, (uint8_t[]){0x55}, 1); /* RGB565 */
    /* Os painéis ILI9341 do CYD ficam corretos com inversão ligada */
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_INVON, NULL, 0);
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    backlight_init();
    lcd_set_backlight(appcfg_get()->brightness);

    ESP_LOGI(TAG, "ILI9341 320x240 inicializado (SPI2 @ %d MHz, DMA)",
             LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

esp_err_t lcd_flush_async(int x, int y, int w, int h, const uint16_t *buf)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t ca[4] = { x >> 8, x & 0xFF, (x + w - 1) >> 8, (x + w - 1) & 0xFF };
    uint8_t ra[4] = { y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF };
    esp_err_t err;
    err = esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, ca, 4);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, ra, 4);
    if (err != ESP_OK) {
        return err;
    }
    s_pending++;
    err = esp_lcd_panel_io_tx_color(s_io, LCD_CMD_RAMWR, buf, w * h * 2);
    if (err != ESP_OK) {
        s_pending--;
    }
    return err;
}

void lcd_wait_one(void)
{
    if (s_pending > 0 && s_flush_sem) {
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        s_pending--;
    }
}

void lcd_wait_all(void)
{
    while (s_pending > 0) {
        lcd_wait_one();
    }
}
