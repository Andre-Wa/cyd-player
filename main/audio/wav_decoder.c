#include <string.h>
#include <stdlib.h>
#include "wav_decoder.h"
#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "wav";

#define OUT_RATE AUDIO_SAMPLE_RATE

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Lê o próximo frame fonte para dec->hist/scratch; retorna false no EOF */
static bool fill_scratch(wav_dec_t *dec, int want_frames)
{
    if (dec->scratch_pos < dec->scratch_frames) {
        return true; /* ainda há dados */
    }
    if (dec->data_left == 0) {
        return false;
    }
    int src_frame_bytes = dec->channels * 2;
    int n = want_frames;
    if ((uint32_t)(n * src_frame_bytes) > dec->data_left) {
        n = dec->data_left / src_frame_bytes;
    }
    if (n <= 0) {
        return false;
    }
    size_t got = fread(dec->scratch, src_frame_bytes, n, dec->f);
    dec->scratch_frames = (int)got;
    dec->scratch_pos = 0;
    dec->data_left -= got * src_frame_bytes;
    return got > 0;
}

/* Copia o frame fonte i do scratch para dst[2] (mono -> duplicado) */
static void get_scratch_frame(wav_dec_t *dec, int i, int16_t dst[2])
{
    const int16_t *p = dec->scratch + (size_t)i * dec->channels;
    dst[0] = p[0];
    dst[1] = (dec->channels == 2) ? p[1] : p[0];
}

bool wav_open(wav_dec_t *dec, const char *path)
{
    memset(dec, 0, sizeof(*dec));
    dec->f = fopen(path, "rb");
    if (!dec->f) {
        ESP_LOGE(TAG, "não foi possível abrir %s", path);
        return false;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, dec->f) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "%s: não é RIFF/WAVE", path);
        goto fail;
    }

    bool have_fmt = false, have_data = false;
    uint16_t fmt_tag = 0, bits = 0;

    while (!have_data) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, dec->f) != 8) {
            break;
        }
        uint32_t size = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t n = size > sizeof(fmt) ? sizeof(fmt) : size;
            if (fread(fmt, 1, n, dec->f) != n) {
                goto fail;
            }
            if (size > n) {
                fseek(dec->f, size - n, SEEK_CUR);
            }
            fmt_tag        = rd16(fmt);
            dec->channels  = rd16(fmt + 2);
            dec->sample_rate = rd32(fmt + 4);
            bits           = rd16(fmt + 14);
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            dec->data_left = size;
            dec->total_frames = (dec->channels && dec->sample_rate)
                ? size / (dec->channels * 2) : 0;
            have_data = true;
        } else {
            /* chunk desconhecido (LIST/INFO etc.) — pula (com padding ímpar) */
            fseek(dec->f, size + (size & 1), SEEK_CUR);
        }
    }

    if (!have_fmt || !have_data) {
        ESP_LOGE(TAG, "%s: chunks fmt/data ausentes", path);
        goto fail;
    }
    if (fmt_tag != 1 || bits != 16 || (dec->channels != 1 && dec->channels != 2)) {
        ESP_LOGE(TAG, "%s: formato não suportado (fmt=%u bits=%u ch=%u) — use PCM 16 bits",
                 path, fmt_tag, bits, dec->channels);
        goto fail;
    }
    if (dec->sample_rate < 4000 || dec->sample_rate > 96000) {
        ESP_LOGE(TAG, "%s: taxa %lu Hz inválida", path, (unsigned long)dec->sample_rate);
        goto fail;
    }

    ESP_LOGI(TAG, "aberto %s: %lu Hz, %u ch, %lu s",
             path, (unsigned long)dec->sample_rate, dec->channels,
             (unsigned long)wav_duration_s(dec));
    return true;

fail:
    fclose(dec->f);
    dec->f = NULL;
    return false;
}

uint32_t wav_duration_s(const wav_dec_t *dec)
{
    if (!dec->sample_rate) {
        return 0;
    }
    return dec->total_frames / dec->sample_rate;
}

int wav_read_stereo(wav_dec_t *dec, int16_t *out, int frames, int volume)
{
    if (!dec->f || dec->eof) {
        return 0;
    }

    /* passo de reamostragem em Q16: quantos frames-fonte por frame de saída */
    uint32_t step_q16 = (uint32_t)(((uint64_t)dec->sample_rate << 16) / OUT_RATE);
    int vol_q8 = volume <= 0 ? 0 : (volume >= 100 ? 256 : (volume * 256) / 100);

    int produced = 0;
    int16_t f0[2] = { dec->hist[0], dec->hist[1] };
    bool have_f0 = dec->hist_valid;

    while (produced < frames) {
        /* avança a posição fracionária consumindo frames fonte */
        while (dec->frame_pos_q16 >= (1u << 16)) {
            dec->frame_pos_q16 -= (1u << 16);
            if (have_f0) {
                /* descarta f0; o próximo frame vira f0 quando lido abaixo */
            }
            if (!fill_scratch(dec, 288)) {
                dec->eof = true;
                break;
            }
            get_scratch_frame(dec, dec->scratch_pos++, f0);
            have_f0 = true;
        }
        if (dec->eof) {
            break;
        }
        /* garante f1 (frame seguinte a f0) */
        int16_t f1[2];
        if (dec->scratch_pos < dec->scratch_frames) {
            get_scratch_frame(dec, dec->scratch_pos, f1);
        } else if (fill_scratch(dec, 288)) {
            get_scratch_frame(dec, dec->scratch_pos, f1);
        } else {
            f1[0] = f0[0];
            f1[1] = f0[1];
        }
        if (!have_f0) { /* primeiro frame do arquivo */
            f0[0] = f1[0];
            f0[1] = f1[1];
            have_f0 = true;
        }

        uint32_t frac = dec->frame_pos_q16; /* 0..65535 */
        for (int c = 0; c < 2; c++) {
            int32_t s = (int32_t)f0[c] + (((int32_t)(f1[c] - f0[c]) * (int32_t)frac) >> 16);
            s = (s * vol_q8) >> 8;
            if (s > 32767)  s = 32767;
            if (s < -32768) s = -32768;
            out[produced * 2 + c] = (int16_t)s;
        }
        produced++;
        dec->frame_pos_q16 += step_q16;
    }

    /* guarda estado p/ próxima chamada */
    if (have_f0) {
        dec->hist[0] = f0[0];
        dec->hist[1] = f0[1];
        dec->hist_valid = true;
    }
    if (produced == 0) {
        dec->eof = true;
    }
    return produced;
}

void wav_close(wav_dec_t *dec)
{
    if (dec->f) {
        fclose(dec->f);
        dec->f = NULL;
    }
    dec->eof = true;
}
