#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "app_config.h"
#include "lcd_spi_driver.h"
#include "nvs_config.h"

static const char *TAG = "lcd";

#if LCD_HW_PRESENT

static spi_device_handle_t s_dev = NULL;

/* Buffer de staging em RAM interna (DMA-capable): transações DMA não
 * conseguem ler direto da flash (onde vivem arrays `const`). */
static uint8_t s_io_buf[64];

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
#define LCD_CMD_RDID    0x04
#define LCD_CMD_RDMADCTL 0x0B
#define LCD_CMD_RDCOLMOD 0x0C

/* Tamanho dos chunks de cor por transação (<= max_transfer_sz do bus) */
#define FLUSH_CHUNK 2048

/* ---------- primitivas SPI no modelo clássico (CS/DC por GPIO) ---------- */

static inline void cs_low(void)  { gpio_set_level(LCD_PIN_CS, 0); }
static inline void cs_high(void) { gpio_set_level(LCD_PIN_CS, 1); }
static inline void dc_cmd(void)  { gpio_set_level(LCD_PIN_DC, 0); }
static inline void dc_dat(void)  { gpio_set_level(LCD_PIN_DC, 1); }

static void spi_tx(const void *buf, int len)
{
    const void *src = buf;
    if (len <= (int)sizeof(s_io_buf)) {
        /* copia p/ RAM interna: garante DMA-capable mesmo vindos da flash */
        memcpy(s_io_buf, buf, (size_t)len);
        src = s_io_buf;
    }
    spi_transaction_t t = {
        .tx_buffer = src,
        .length = (size_t)len * 8,
    };
    spi_device_polling_transmit(s_dev, &t);
}

static void spi_rx(void *buf, int len)
{
    int n = len > (int)sizeof(s_io_buf) ? (int)sizeof(s_io_buf) : len;
    spi_transaction_t t = {
        .rx_buffer = s_io_buf,
        .length = (size_t)n * 8,
    };
    spi_device_polling_transmit(s_dev, &t);
    memcpy(buf, s_io_buf, (size_t)n);
}

/* comando + parâmetros sob UMA asserção de CS (sequência comprovada em
 * ILI9341: é exatamente o que TFT_eSPI/Adafruit fazem) */
static void lcd_cmd_p(uint8_t cmd, const uint8_t *params, int n)
{
    cs_low();
    dc_cmd();
    spi_tx(&cmd, 1);
    if (params && n > 0) {
        dc_dat();
        spi_tx(params, n);
    }
    cs_high();
}

static void lcd_read(uint8_t cmd, uint8_t *out, int n)
{
    cs_low();
    dc_cmd();
    spi_tx(&cmd, 1);
    dc_dat();
    spi_rx(out, n);
    cs_high();
}

/* ---------- backlight (LEDC PWM, GPIO21) ---------- */

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

/* ---------- verificação por readback ---------- */

/* MADCTL adequado a cada modo de orientação */
static uint8_t madctl_for_xform(uint8_t xf)
{
    if (xf == 0) {
        return LCD_MADCTL;                     /* paisagem (MV|BGR) */
    }
    if (xf == 1) {
        return (uint8_t)(LCD_MADCTL ^ 0xC0);   /* paisagem 180° */
    }
    return (uint8_t)(LCD_MADCTL & 0x08);       /* retrato nativo + transposição */
}

static void apply_madctl(void)
{
    uint8_t m = madctl_for_xform(appcfg_get()->lcd_xform);
    lcd_cmd_p(LCD_CMD_MADCTL, &m, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_cmd_p(LCD_CMD_MADCTL, &m, 1);   /* 2º envio: segurança p/ clones */
}

void lcd_set_xform(uint8_t xf)
{
    appcfg_get()->lcd_xform = xf > 5 ? 0 : xf;
    apply_madctl();
}

/* Readback apenas INFORMATIVO: muitos clones do CYD não trazem o SDO do
 * painel conectado, e o MISO retorna crosstalk determinístico do clock
 * (ex.: "06 78 00"). Não use para decisão — só a imagem confirma. */
static void verify_madctl(uint8_t expected)
{
    uint8_t r[3] = {0};
    lcd_read(LCD_CMD_RDMADCTL, r, 3);
    ESP_LOGI(TAG, "readback MADCTL (informativo): %02X %02X %02X (escrito %02X)",
             r[0], r[1], r[2], expected);
}

static void lcd_diag(void)
{
    uint8_t id[4] = {0}, mad[3] = {0}, cm[2] = {0};
    lcd_read(LCD_CMD_RDID, id, 4);
    lcd_read(LCD_CMD_RDMADCTL, mad, 3);
    lcd_read(LCD_CMD_RDCOLMOD, cm, 2);
    ESP_LOGI(TAG, "DIAG RDDID: %02X %02X %02X %02X (ILI9341 ~00 93 41)",
             id[0], id[1], id[2], id[3]);
    ESP_LOGI(TAG, "DIAG RDDMADCTL: %02X %02X %02X (esperado %02X)",
             mad[0], mad[1], mad[2], LCD_MADCTL);
    ESP_LOGI(TAG, "DIAG RDCOLMOD: %02X %02X (esperado x5)", cm[0], cm[1]);
}

/* ---------- init ---------- */

esp_err_t lcd_init(void)
{
    /* CS e DC como GPIO comuns (controlados à mão) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_PIN_CS) | (1ULL << LCD_PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    cs_high();
    dc_cmd();

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        /* DMA é obrigatório: sem ele o host limita transações a 64 bytes
         * ("txdata transfer > host maximum") e os pixels nunca chegam. */
        .max_transfer_sz = FLUSH_CHUNK + 64,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_PCLK_HZ,
        .mode = 0,
        .spics_io_num = -1,          /* CS por GPIO */
        .queue_size = 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &devcfg, &s_dev),
                        TAG, "spi device");

    /* Sequência mínima e segura do ILI9341 (sem comandos de teste/vendor
     * exóticos, que podem travar clones em modos errados) */
    lcd_cmd_p(LCD_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd_p(LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd_p(LCD_CMD_COLMOD, (const uint8_t[]){0x55}, 1);      /* RGB565 */
    lcd_cmd_p(0xB1, (const uint8_t[]){0x00, 0x18}, 2);         /* frame rate */
    lcd_cmd_p(0xB6, (const uint8_t[]){0x08, 0x82, 0x27}, 3);   /* disp func */
    lcd_cmd_p(0xC0, (const uint8_t[]){0x23}, 1);               /* power 1 */
    lcd_cmd_p(0xC1, (const uint8_t[]){0x10}, 1);               /* power 2 */
    lcd_cmd_p(0xC5, (const uint8_t[]){0x3E, 0x28}, 2);         /* VCOM 1 */
    lcd_cmd_p(0xC7, (const uint8_t[]){0x86}, 1);               /* VCOM 2 */
    lcd_cmd_p(0x26, (const uint8_t[]){0x01}, 1);               /* gamma curve */
    lcd_cmd_p(0xF2, (const uint8_t[]){0x00}, 1);               /* 3Gamma off */
    static const uint8_t gamma_p[] = {0x1F, 0x36, 0x36, 0x3A, 0x0C, 0x05, 0x4F,
                                      0x87, 0x3C, 0x08, 0x11, 0x35, 0x19, 0x13, 0x00};
    static const uint8_t gamma_n[] = {0x00, 0x09, 0x09, 0x05, 0x13, 0x0A, 0x30,
                                      0x78, 0x43, 0x07, 0x0E, 0x0A, 0x26, 0x2C, 0x1F};
    lcd_cmd_p(0xE0, gamma_p, sizeof(gamma_p));
    lcd_cmd_p(0xE1, gamma_n, sizeof(gamma_n));
#if LCD_INVERT_COLORS
    lcd_cmd_p(LCD_CMD_INVON, NULL, 0);
#else
    lcd_cmd_p(LCD_CMD_INVOFF, NULL, 0);
#endif

    /* orientação salva na NVS (quiz da seta / Ajustes) */
    apply_madctl();
    verify_madctl(madctl_for_xform(appcfg_get()->lcd_xform));

    lcd_cmd_p(LCD_CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    lcd_diag();

    backlight_init();
    lcd_set_backlight(appcfg_get()->brightness);

    ESP_LOGI(TAG, "ILI9341 320x240 inicializado (SPI2 @ %d MHz, CS/DC manuais)",
             LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

/* ---------- flush de área ---------- */

/* Buffer de transposição (painéis que ignoram o bit MV): uma tira UI
 * 320x16 vira uma janela de 16 colunas x 320 linhas do painel retrato. */
static uint16_t s_tbuf[320 * LCD_STRIP_LINES];

static esp_err_t flush_transposed(int y, int h, const uint16_t *buf, uint8_t t)
{
    const bool cinv = (t >= 4);          /* c = 239 - y */
    const bool xinv = (t == 3 || t == 5);/* r = 319 - x */
    int c0 = cinv ? (LCD_V_RES - 1 - (y + h - 1)) : y;
    int c1 = cinv ? (LCD_V_RES - 1 - y) : (y + h - 1);

    int n = 0;
    for (int r = 0; r < LCD_H_RES; r++) {            /* linhas do painel */
        int ux = xinv ? (LCD_H_RES - 1 - r) : r;     /* x na UI */
        for (int c = c0; c <= c1; c++) {
            int uy = cinv ? (LCD_V_RES - 1 - c) : c; /* y na UI */
            s_tbuf[n++] = buf[(uy - y) * LCD_H_RES + ux];
        }
    }

    const uint8_t ca[4] = { (uint8_t)(c0 >> 8), (uint8_t)(c0 & 0xFF),
                            (uint8_t)(c1 >> 8), (uint8_t)(c1 & 0xFF) };
    const uint8_t ra[4] = { 0, 0, (uint8_t)((LCD_H_RES - 1) >> 8),
                            (uint8_t)((LCD_H_RES - 1) & 0xFF) };
    cs_low();
    dc_cmd(); spi_tx((uint8_t[]){LCD_CMD_CASET}, 1);
    dc_dat(); spi_tx(ca, 4);
    dc_cmd(); spi_tx((uint8_t[]){LCD_CMD_RASET}, 1);
    dc_dat(); spi_tx(ra, 4);
    dc_cmd(); spi_tx((uint8_t[]){LCD_CMD_RAMWR}, 1);
    dc_dat();
    int total = n * 2, off = 0;
    while (off < total) {
        int k = total - off > FLUSH_CHUNK ? FLUSH_CHUNK : total - off;
        spi_tx((const uint8_t *)s_tbuf + off, k);
        off += k;
    }
    cs_high();
    return ESP_OK;
}

esp_err_t lcd_flush_async(int x, int y, int w, int h, const uint16_t *buf)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t t = appcfg_get()->lcd_xform;
    if (t >= 2 && x == 0 && w == LCD_H_RES) {
        return flush_transposed(y, h, buf, t);
    }
    const uint8_t ca[4] = { (uint8_t)(x >> 8), (uint8_t)(x & 0xFF),
                            (uint8_t)((x + w - 1) >> 8), (uint8_t)((x + w - 1) & 0xFF) };
    const uint8_t ra[4] = { (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
                            (uint8_t)((y + h - 1) >> 8), (uint8_t)((y + h - 1) & 0xFF) };

    cs_low();
    dc_cmd(); spi_tx((uint8_t[]){LCD_CMD_CASET}, 1);
    dc_dat(); spi_tx(ca, 4);
    dc_cmd(); spi_tx((uint8_t[]){LCD_CMD_RASET}, 1);
    dc_dat(); spi_tx(ra, 4);
    dc_cmd(); spi_tx((uint8_t[]){LCD_CMD_RAMWR}, 1);
    dc_dat();
    int total = w * h * 2;
    int off = 0;
    while (off < total) {
        int n = total - off > FLUSH_CHUNK ? FLUSH_CHUNK : total - off;
        spi_tx((const uint8_t *)buf + off, n);
        off += n;
    }
    cs_high();
    return ESP_OK;
}

void lcd_wait_one(void) { }
void lcd_wait_all(void) { }

#else  /* ---------- build sem display (emulador) ---------- */

esp_err_t lcd_init(void)
{
    ESP_LOGW(TAG, "LCD_HW_PRESENT=0 — modo emulação, display desativado");
    return ESP_OK;
}
void lcd_set_backlight(uint8_t pct) { (void)pct; }
void lcd_set_xform(uint8_t xf) { (void)xf; }
esp_err_t lcd_flush_async(int x, int y, int w, int h, const uint16_t *buf)
{
    (void)x; (void)y; (void)w; (void)h; (void)buf;
    return ESP_OK;
}
void lcd_wait_one(void) { }
void lcd_wait_all(void) { }

#endif /* LCD_HW_PRESENT */
