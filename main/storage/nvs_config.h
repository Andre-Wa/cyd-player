/*
 * nvs_config.h — Configurações persistentes na memória interna (NVS).
 *
 * A partição NVS tem 48 KB (partitions.csv), suficiente para as chaves de
 * pareamento do Bluedroid (namespace "bt") e para a configuração do app
 * (namespace "cfg"). Caso a NVS fique cheia, as configurações NÃO críticas
 * (última faixa, diretório raiz) podem ser espelhadas em /sdcard/cyd.cfg —
 * ver appcfg_mirror_to_sd().
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    /* áudio */
    uint8_t  volume;             /* 0..100 */
    uint8_t  local_out;          /* 1 = alto-falante on-board ativo sem BT */
    uint8_t  brightness;         /* backlight 0..100 */
    uint8_t  auto_next;          /* avançar automaticamente no fim da faixa */

    /* reprodução */
    char     root_dir[64];       /* pasta raiz das músicas */
    char     last_track[192];    /* último caminho reproduzido */
    uint8_t  last_index;         /* último índice na playlist */

    /* último alto-falante/fone Bluetooth pareado (reconexão automática) */
    bool     has_bt;
    uint8_t  bt_mac[6];
    char     bt_name[32];

    /* calibração do touch: xs = c0*rx + c1*ry + c2 ; ys = c3*rx + c4*ry + c5 */
    bool     has_calib;
    float    calib[6];
} app_config_t;

/* Carrega a configuração da NVS (valores padrão se ausente). */
esp_err_t appcfg_load(void);

/* Salva toda a configuração na NVS (commit imediato). */
esp_err_t appcfg_save(void);

/* Ponteiro para a instância global (sempre válida após appcfg_load). */
app_config_t *appcfg_get(void);

/* Espelha itens não críticos em /sdcard/cyd.cfg (fallback solicitado no
 * enunciado, caso a NVS não comporte). Retorna false se o SD não estiver
 * montado. */
bool appcfg_mirror_to_sd(void);
