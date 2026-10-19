/* Gerado automaticamente por tools/gen_font.py - NAO EDITAR */
#pragma once
#include <stdint.h>

#define FONT8X16_FIRST  32
#define FONT8X16_LAST   126
#define FONT8X16_NUM_CHARS (126 - 32 + 1)
#define FONT8X16_W      8
#define FONT8X16_H      16

/* 1 byte por linha; bit7 = pixel mais a esquerda */
extern const uint8_t font8x16_glyphs[FONT8X16_NUM_CHARS][FONT8X16_H];
