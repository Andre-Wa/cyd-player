/*
 * ui_engine.h — Engine de UI por tiras com dirty regions (spec §3).
 *
 *  - Dois buffers parciais DMA (320x16 RGB565, 10,24 KB cada) em ping-pong;
 *  - Redesenho restrito às tiras marcadas como sujas (bitmask);
 *  - Tarefa ui_render_task (Core 1, prio 10): consome eventos de
 *    audio_to_ui_queue, atualiza o modelo compartilhado e renderiza;
 *  - Tarefa touch_task (Core 1, prio 5): amostra o XPT2046 a cada 20 ms,
 *    filtra e gera eventos TAP/PRESS/RELEASE/DRAG.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "gfx_primitives.h"
#include "app_types.h"

typedef enum {
    SCREEN_SPLASH = 0,
    SCREEN_PLAYER,
    SCREEN_BROWSER,
    SCREEN_BT,
    SCREEN_SETTINGS,
    SCREEN_CALIB,
    SCREEN_COUNT,
} screen_id_t;

typedef enum {
    TEV_PRESS = 0,
    TEV_RELEASE,
    TEV_TAP,
    TEV_DRAG,
} touch_evt_type_t;

typedef struct {
    touch_evt_type_t type;
    int x, y;         /* coords de tela */
    int dx, dy;       /* incremento (DRAG) */
    uint16_t rx, ry;  /* coords brutas do ADC (calibração) */
} touch_evt_t;

typedef struct {
    const char *name;
    void (*on_enter)(void);
    void (*on_exit)(void);
    void (*draw)(gfx_canvas_t *c);              /* coords de tela; clip por tira */
    void (*on_touch)(const touch_evt_t *ev);    /* pode ser NULL */
} ui_screen_t;

/* -------- modelo compartilhado (atualizado pela engine) -------- */
#define UI_MAX_DEVICES 12

typedef struct {
    /* player */
    player_state_t pstate;
    audio_route_t  route;
    char     title[80];
    uint32_t elapsed_s;
    uint32_t total_s;

    /* bluetooth */
    bt_state_t bt_state;
    char bt_peer[32];
    struct {
        uint8_t mac[6];
        char name[32];
        int8_t rssi;
        bool rendering;
    } devices[UI_MAX_DEVICES];
    int n_devices;

    /* sistema */
    bool sd_ok;
    char error[72];
    int64_t error_until_ms;   /* toast visível até este instante (0 = nenhum) */
} ui_model_t;

extern ui_model_t g_ui;

/* -------- API -------- */

/* Cria buffers/tarefas e mostra o splash. Requer lcd/touch já iniciados e
 * g_audio_to_ui_queue criada. */
void ui_init(void);

/* Troca de tela (thread-safe). */
void ui_show(screen_id_t id);

/* Solicita redesenho completo (thread-safe). */
void ui_invalidate_all(void);

/* Timestamp em ms (para toasts) */
int64_t ui_now_ms(void);
