/*
 * app_types.h — Tipos compartilhados e definição das filas de IPC (§5 da spec).
 *
 *   ui_to_audio_queue   : UI/controle -> pipeline de áudio / Bluetooth
 *   audio_to_ui_queue   : áudio/BT/armazenamento -> UI
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ============================ Comandos ==================================== */
typedef enum {
    CMD_NONE = 0,
    CMD_PLAY,               /* retomar */
    CMD_PAUSE,
    CMD_TOGGLE,             /* play/pause */
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_PLAY_FILE,          /* str = caminho completo */
    CMD_SET_VOLUME,         /* val = 0..100 */
    CMD_SET_BRIGHTNESS,     /* val = 0..100 */
    CMD_SET_LOCAL_OUT,      /* val = 0/1 */
    CMD_BT_SCAN,
    CMD_BT_CONNECT,         /* mac[6] */
    CMD_BT_DISCONNECT,
    CMD_CALIB_DONE,         /* calib[6] concluída (enviada pela tela) */
} cmd_id_t;

typedef struct {
    cmd_id_t id;
    int32_t  val;
    char     str[176];      /* caminho de arquivo */
    uint8_t  mac[6];
} app_cmd_t;

/* ============================ Eventos ===================================== */
typedef enum {
    EVT_NONE = 0,
    /* player */
    EVT_STATE,              /* val = player_state_t */
    EVT_TRACK_INFO,         /* str = título, val = duração (s) */
    EVT_PROGRESS,           /* val = segundos decorridos */
    EVT_TRACK_END,
    EVT_ROUTE,              /* val = audio_route_t (BT ou alto-falante) */
    /* bluetooth */
    EVT_BT_STATE,           /* val = bt_state_t */
    EVT_BT_DEVICE,          /* mac + str = nome + val = rssi */
    EVT_BT_SCAN_DONE,
    /* sistema */
    EVT_SD,                 /* val = 1 montado / 0 ausente */
    EVT_VOLUME,             /* val = 0..100 (mudou remotamente via AVRCP) */
    EVT_ERROR,              /* str = mensagem */
} evt_id_t;

typedef enum {
    PST_STOPPED = 0,
    PST_PLAYING,
    PST_PAUSED,
} player_state_t;

typedef enum {
    BT_OFF = 0,
    BT_READY,               /* pilha ativa, sem conexão */
    BT_SCANNING,
    BT_CONNECTING,
    BT_CONNECTED,
} bt_state_t;

typedef enum {
    ROUTE_NONE = 0,         /* sem saída ativa */
    ROUTE_BT,               /* streaming A2DP */
    ROUTE_LOCAL,            /* alto-falante on-board (DAC IO26) */
} audio_route_t;

typedef struct {
    evt_id_t id;
    int32_t  val;
    char     str[80];       /* título / nome de dispositivo / mensagem */
    uint8_t  mac[6];
} app_evt_t;

/* Filas globais (criadas em main.c) */
extern QueueHandle_t g_ui_to_audio_queue;
extern QueueHandle_t g_audio_to_ui_queue;

/* utilitário: enviar comando sem bloquear */
static inline void app_send_cmd(const app_cmd_t *cmd)
{
    if (g_ui_to_audio_queue) {
        xQueueSend(g_ui_to_audio_queue, cmd, pdMS_TO_TICKS(100));
    }
}
