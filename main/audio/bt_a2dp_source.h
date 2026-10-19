/*
 * bt_a2dp_source.h — Bluetooth Classic: descoberta GAP, A2DP Source (SBC via
 * Bluedroid) e AVRCP (volume absoluto + passthrough). Spec §2.1/§4.
 *
 * Os callbacks do Bluedroid rodam na tarefa BTC (Core 0). Eventos de interesse
 * da UI são publicados em g_audio_to_ui_queue; o áudio é puxado do ring buffer
 * do pipeline pelo callback de dados.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"

/* Inicializa controlador BR/EDR, Bluedroid, GAP, A2DP Source e AVRCP.
 * Deve ser chamada após nvs_flash_init(). */
esp_err_t bt_audio_init(void);

void bt_start_discovery(void);              /* ~10 s de varredura */
void bt_stop_discovery(void);
void bt_connect(const uint8_t mac[6]);
void bt_disconnect(void);

bool bt_is_connected(void);
bt_state_t bt_get_state(void);

/* Dispositivo conectado (vazio se não conectado) */
void bt_get_peer(char *name, int name_len, uint8_t mac[6]);

/* AVRCP: volume 0..100 -> SetAbsoluteVolume no sink (melhor esforço) */
void bt_avrcp_set_volume(uint8_t vol100);

/* AVRCP passthrough (PLAY/PAUSE/STOP no dispositivo remoto) */
void bt_avrcp_passthrough(uint8_t cmd, bool pressed);
