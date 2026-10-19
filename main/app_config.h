/*
 * app_config.h — Mapeamento de pinos do ESP32-2432S028R (Cheap Yellow Display)
 * e constantes globais do projeto.
 *
 * Distribuição dos periféricos SPI (o ESP32 possui apenas 2 hosts SPI de uso
 * geral, mas a placa expõe 3 conjuntos de pinos SPI):
 *
 *   SPI2_HOST (HSPI, pinos nativos) -> LCD ILI9341  (áudio/UI: DMA)
 *   SPI3_HOST (VSPI, pinos nativos) -> Cartão SD     (throughput de áudio)
 *   Software SPI (bit-bang)         -> Touch XPT2046 (baixa taxa, 50 Hz)
 */
#pragma once

#include "driver/spi_common.h"
#include "driver/gpio.h"

/* ===================== Display ILI9341 320x240 (SPI2) ===================== */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIN_MOSI        GPIO_NUM_13
#define LCD_PIN_MISO        GPIO_NUM_12
#define LCD_PIN_SCLK        GPIO_NUM_14
#define LCD_PIN_CS          GPIO_NUM_15
#define LCD_PIN_DC          GPIO_NUM_2
#define LCD_PIN_BL          GPIO_NUM_21     /* backlight via LEDC (PWM) */
#define LCD_PIN_RST         GPIO_NUM_NC     /* sem reset por hardware */

#define LCD_H_RES           320
#define LCD_V_RES           240
/* 40 MHz com CS/DC manuais (modelo TFT_eSPI/Adafruit, comprovado no CYD).
 * Se houver instabilidade de escrita em cabos/unidades marginais, reduza
 * para 20 MHz. */
#define LCD_PCLK_HZ         (40 * 1000 * 1000)

/* MADCTL: paisagem 320x240 + ordem de cor.
 *   0x28 = MV|BGR  (padrão CYD: paisagem, USB à esquerda, cores BGR)
 *   0xE8 = MX|MY|MV|BGR (paisagem girada 180°)
 *   0x20 = MV|RGB (se vermelho/azul ficarem trocados, remova o bit BGR)
 * Confira no log de boot: o driver relê o MADCTL gravado no painel. */
#define LCD_MADCTL          0x28

/* Inversão de cores do painel. ILI9341 do CYD: 0 (INVOFF).
 * Se as cores parecerem "negativo fotográfico", mude para 1. */
#define LCD_INVERT_COLORS   0

/* Renderização parcial por tiras: 320 x 16 x RGB565 = 10240 bytes por buffer.
 * Dois buffers (ping-pong) = 20,5 KB de SRAM interna com capacidade DMA. */
#define LCD_STRIP_LINES     16
#define LCD_STRIP_BYTES     (LCD_H_RES * LCD_STRIP_LINES * 2)

/* 0 = sem hardware de display (build para emulador/testes de lógica de UI) */
#ifndef LCD_HW_PRESENT
#define LCD_HW_PRESENT      1
#endif

/* ===================== Touch XPT2046 (SPI software) ======================= */
#define TOUCH_PIN_CLK       GPIO_NUM_25
#define TOUCH_PIN_MOSI      GPIO_NUM_32
#define TOUCH_PIN_MISO      GPIO_NUM_39     /* input-only */
#define TOUCH_PIN_CS        GPIO_NUM_33
#define TOUCH_PIN_IRQ       GPIO_NUM_36     /* input-only; usamos polling */

#define TOUCH_ADC_MAX       4096            /* 12 bits */
#define TOUCH_Z_THRESHOLD   600             /* pressão mínima (placa CYD) */
#define TOUCH_SAMPLES_AVG   3

/* Mapeamento padrão bruto -> tela (paisagem, MADCTL 0x28).
 * A calibração de 4 pontos (tela "Calibrar") refina/sobrepõe estes valores. */
#define TOUCH_SWAP_XY       1               /* eixo Y bruto vira X da tela */
#define TOUCH_MIRROR_X      1
#define TOUCH_MIRROR_Y      1

/* ===================== Cartão SD (SPI3) =================================== */
#define SD_SPI_HOST         SPI3_HOST
#define SD_PIN_CS           GPIO_NUM_5
#define SD_PIN_MOSI         GPIO_NUM_23
#define SD_PIN_MISO         GPIO_NUM_19
#define SD_PIN_SCLK         GPIO_NUM_18
#define SD_FREQ_KHZ         SDMMC_FREQ_DEFAULT   /* 20 MHz — confiável no CYD */
#define SD_MOUNT_POINT      "/sdcard"

/* ===================== Áudio local (SC8002D em IO26) ====================== */
/* GPIO26 = canal 1 do DAC de 8 bits do ESP32 (GPIO25, canal 0, é o CLK do
 * touch). O amplificador SC8002D do CYD não tem pino de enable — fica sempre
 * ativo; quando ocioso o DAC mantém nível médio (0x80) para evitar "pop". */
#define AMP_DAC_CHANNEL     DAC_CHANNEL_1   /* GPIO26 */
#define AUDIO_SAMPLE_RATE   44100           /* SBC/A2DP: 44,1 kHz fixo */

/* ===================== LED RGB de status (ânodo comum) ==================== */
#define LED_PIN_R           GPIO_NUM_4
#define LED_PIN_G           GPIO_NUM_16
#define LED_PIN_B           GPIO_NUM_17
#define LED_ACTIVE_LOW      1               /* nível 0 = aceso */

/* ===================== Aplicação ========================================== */
#define APP_BT_DEVICE_NAME  "CYD Audio Player"
#define APP_VERSION         "1.3.4"

/* RODADA DE TESTES (debug): força o fluxo seta de orientação + calibração
 * de touch em TODO boot, independente de orient_ok/has_calib. */
#define DEBUG_CALIB_EVERY_BOOT 0

/* Compile com -DAPP_BT_ENABLE=0 para builds sem Bluetooth (ex.: emulador
 * QEMU, que não possui rádio). Em hardware real, mantenha 1. */
#ifndef APP_BT_ENABLE
#define APP_BT_ENABLE       1
#endif

/* Prioridades de tarefas (ver especificação §2.2) */
#define PRIO_AUDIO_PIPELINE 20
#define PRIO_UI_RENDER      10
#define PRIO_TOUCH          5
#define PRIO_APP_CTRL       2

/* Núcleos */
#define CORE_AUDIO          0
#define CORE_UI             1
