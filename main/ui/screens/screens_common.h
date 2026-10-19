/* screens_common.h — Elementos compartilhados pelas telas. */
#pragma once

#include "gfx_primitives.h"
#include "ui_engine.h"

#define HEADER_H 26

/* Barra superior: título + estado do BT + volume + SD */
void ui_draw_header(gfx_canvas_t *c, const char *title);

/* Toast de erro na base da tela (se ativo) */
void ui_draw_toast(gfx_canvas_t *c);

/* Botão "Voltar" padrão no rodapé (x 8..88, y 206..236) */
void ui_draw_back_button(gfx_canvas_t *c);
bool ui_back_button_hit(int x, int y);
