/* sd_card.h — Montagem do cartão SD via SDSPI (SPI3, pinos nativos VSPI). */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Inicializa o barramento SPI3 e monta o FATFS em SD_MOUNT_POINT.
 * Não é fatal: retorna erro se não houver cartão (a UI mostra aviso). */
esp_err_t sd_card_init(void);

bool sd_card_is_mounted(void);

/* Tamanho total/livre do cartão em bytes (0 se não montado). */
void sd_card_get_usage(uint64_t *total, uint64_t *free_b);
