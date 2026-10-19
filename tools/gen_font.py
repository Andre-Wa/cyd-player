#!/usr/bin/env python3
"""
Gera a tabela de fonte 8x16 (1bpp) para o display ILI9341 do CYD.
Usa DejaVu Sans Mono Bold via Pillow e emite main/display/font8x16.c/.h

Uso: python3 tools/gen_font.py [caminho_fonte_ttf]
Cada glifo: 16 bytes, 1 byte por linha; bit 7 = pixel mais à esquerda.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H = 8, 16
FIRST_CHAR, LAST_CHAR = 32, 126  # ASCII imprimível
FONT_PATH = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
FONT_SIZE = 13  # cabe na célula 8x16 com a métrica mono do DejaVu

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
asc, desc = font.getmetrics()          # ascent / descent em px
baseline = min(CELL_H - 3, asc + 1)    # linha de base dentro da célula

glyphs = {}
for ch in range(FIRST_CHAR, LAST_CHAR + 1):
    img = Image.new("1", (CELL_W, CELL_H), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), chr(ch), font=font)
    # centraliza horizontalmente na célula
    x = (CELL_W - (bbox[2] - bbox[0])) // 2 - bbox[0]
    d.text((x, baseline - asc), chr(ch), font=font, fill=1)
    rows = []
    for y in range(CELL_H):
        b = 0
        for x in range(CELL_W):
            if img.getpixel((x, y)):
                b |= 0x80 >> x
        rows.append(b)
    glyphs[ch] = rows

out = []
out.append("/* Gerado automaticamente por tools/gen_font.py - NAO EDITAR */")
out.append('#include "font8x16.h"')
out.append("")
out.append("const uint8_t font8x16_glyphs[FONT8X16_NUM_CHARS][FONT8X16_H] = {")
for ch in range(FIRST_CHAR, LAST_CHAR + 1):
    rows = glyphs[ch]
    name = repr(chr(ch)) if ch != 32 else "' '"
    out.append("    { /* %s (0x%02X) */ %s }," % (
        name, ch, ", ".join("0x%02X" % r for r in rows)))
out.append("};")
out.append("")

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "../main/display/font8x16.c"), "w") as f:
    f.write("\n".join(out) + "\n")

header = f"""/* Gerado automaticamente por tools/gen_font.py - NAO EDITAR */
#pragma once
#include <stdint.h>

#define FONT8X16_FIRST  {FIRST_CHAR}
#define FONT8X16_LAST   {LAST_CHAR}
#define FONT8X16_NUM_CHARS ({LAST_CHAR} - {FIRST_CHAR} + 1)
#define FONT8X16_W      {CELL_W}
#define FONT8X16_H      {CELL_H}

/* 1 byte por linha; bit7 = pixel mais a esquerda */
extern const uint8_t font8x16_glyphs[FONT8X16_NUM_CHARS][FONT8X16_H];
"""
with open(os.path.join(base, "../main/display/font8x16.h"), "w") as f:
    f.write(header)

print("OK: %d glifos, %d bytes de tabela" % (LAST_CHAR - FIRST_CHAR + 1,
      (LAST_CHAR - FIRST_CHAR + 1) * CELL_H))
