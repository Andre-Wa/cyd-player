/*
 * wav_decoder.h — Leitor/decodificador de WAV (PCM) com conversão para o
 * formato do pipeline: estéreo, 16 bits, 44,1 kHz (exigência do SBC/A2DP).
 *
 * Suporta: PCM 16 bits, 1 ou 2 canais, qualquer taxa (44,1 kHz nativo;
 * 22,05/48 kHz etc. por reamostragem linear). Formatos ADPCM/IEEE float/
 * 24/32 bits são rejeitados na abertura (ver README: conversão com ffmpeg).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    FILE    *f;
    uint32_t sample_rate;     /* taxa do arquivo */
    uint16_t channels;        /* 1 ou 2 */
    uint32_t data_left;       /* bytes restantes no chunk "data" */
    uint32_t total_frames;    /* frames totais (fonte) */
    uint32_t frame_pos_q16;   /* posição de leitura fracionária (Q16, taxa fonte) */
    int16_t  hist[2];         /* último frame lido (interpolação) */
    bool     hist_valid;
    int16_t  scratch[2 * 320];  /* leitura em blocos (até 320 frames fonte) */
    int      scratch_frames;
    int      scratch_pos;
    bool     eof;
} wav_dec_t;

/* Abre e valida o arquivo. true se utilizável. */
bool wav_open(wav_dec_t *dec, const char *path);

/* Gera 'frames' frames estéreo 16 bits @44,1 kHz em 'out' (2*frames int16).
 * Aplica volume (0..100) e conversão de taxa/canais.
 * Retorna o número de frames efetivamente gerados (0 = fim de arquivo). */
int wav_read_stereo(wav_dec_t *dec, int16_t *out, int frames, int volume);

/* Duração total em segundos (estimada pelo chunk data). */
uint32_t wav_duration_s(const wav_dec_t *dec);

void wav_close(wav_dec_t *dec);
