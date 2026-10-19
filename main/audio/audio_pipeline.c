/*
 * audio_pipeline.c — Tarefa de áudio do Core 0 (spec §2.1, prioridade 20).
 *
 *   SD (WAV) -> wav_decoder (conversão p/ 44,1 kHz est. 16 bits + volume)
 *            -> RingBuffer 16 KB -> callback A2DP Source -> SBC -> Bluetooth
 *            -> (ou) DAC local GPIO26 quando não há conexão BT
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#include "app_config.h"
#include "app_types.h"
#include "audio_pipeline.h"
#include "wav_decoder.h"
#include "local_dac.h"
#include "playlist.h"
#include "bt_a2dp_source.h"
#include "nvs_config.h"

static const char *TAG = "pipeline";

/* ------- comandos internos da tarefa ------- */
typedef enum {
    PIPE_OPEN_FILE = 1,
    PIPE_STOP,
    PIPE_PAUSE,
    PIPE_RESUME,
} pipe_cmd_id_t;

typedef struct {
    pipe_cmd_id_t id;
    char path[PL_PATH_LEN];
} pipe_cmd_t;

#define PCM_BLOCK_FRAMES 256                     /* 256 frames = 1024 B estéreo */

static QueueHandle_t s_cmd_q;
static RingbufHandle_t s_ring;
static TaskHandle_t s_task;

static volatile bool s_playing;
static volatile bool s_paused;
static volatile uint8_t s_volume = 65;
static volatile uint32_t s_elapsed_s;
static volatile uint32_t s_total_s;
static volatile bool s_bt_stream_started;

static bool hwm_logged;
static wav_dec_t s_dec;
static char s_cur_path[PL_PATH_LEN];
static audio_route_t s_route = ROUTE_NONE;
static void (*s_track_end_cb)(void);

void audio_pipeline_set_track_end_cb(void (*cb)(void))
{
    s_track_end_cb = cb;
}

/* ================= API pública ================= */

RingbufHandle_t audio_pipeline_get_ring(void)
{
    return s_ring;
}

bool audio_pipeline_is_playing(void)
{
    return s_playing && !s_paused;
}

bool audio_pipeline_is_active(void)
{
    return s_playing;
}

uint32_t audio_pipeline_elapsed_s(void)
{
    return s_elapsed_s;
}

uint32_t audio_pipeline_total_s(void)
{
    return s_total_s;
}

static void pipe_send(pipe_cmd_id_t id, const char *path)
{
    pipe_cmd_t c = { .id = id };
    if (path) {
        strlcpy(c.path, path, sizeof(c.path));
    }
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(300));
    }
}

void audio_pipeline_play_file(const char *path)
{
    pipe_send(PIPE_OPEN_FILE, path);
}

void audio_pipeline_stop(void)
{
    pipe_send(PIPE_STOP, NULL);
}

void audio_pipeline_set_paused(bool paused)
{
    pipe_send(paused ? PIPE_PAUSE : PIPE_RESUME, NULL);
}

void audio_pipeline_set_volume(uint8_t vol)
{
    s_volume = vol > 100 ? 100 : vol;
}

void audio_pipeline_notify_bt_stream(bool started)
{
    s_bt_stream_started = started;
}

/* ================= helpers ================= */

static void post_evt(evt_id_t id, int32_t val, const char *str)
{
    app_evt_t e = { .id = id, .val = val };
    if (str) {
        strlcpy(e.str, str, sizeof(e.str));
    }
    if (g_audio_to_ui_queue) {
        xQueueSend(g_audio_to_ui_queue, &e, pdMS_TO_TICKS(100));
    }
}

static void title_from_path(const char *path, char *out, size_t out_len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strlcpy(out, base, out_len);
    size_t n = strlen(out);
    if (n > 4 && strcasecmp(out + n - 4, ".wav") == 0) {
        out[n - 4] = '\0';
    }
}

static audio_route_t current_route(void)
{
    if (bt_is_connected() && s_bt_stream_started) {
        return ROUTE_BT;
    }
    if (appcfg_get()->local_out) {
        return ROUTE_LOCAL;
    }
    return ROUTE_NONE;
}

static void set_route(audio_route_t r)
{
    if (r != s_route) {
        s_route = r;
        post_evt(EVT_ROUTE, r, NULL);
        ESP_LOGI(TAG, "rota de áudio: %s",
                 r == ROUTE_BT ? "Bluetooth A2DP" :
                 r == ROUTE_LOCAL ? "alto-falante local" : "nenhuma");
    }
}

/* Abre arquivo e publica eventos de início de faixa */
static bool open_track(const char *path)
{
    wav_close(&s_dec);
    s_playing = false;

    if (!wav_open(&s_dec, path)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Falha ao abrir %.60s", path);
        post_evt(EVT_ERROR, 0, msg);
        post_evt(EVT_STATE, PST_STOPPED, NULL);
        return false;
    }
    strlcpy(s_cur_path, path, sizeof(s_cur_path));
    s_total_s = wav_duration_s(&s_dec);
    s_elapsed_s = 0;
    s_paused = false;
    s_playing = true;
    hwm_logged = false;

    /* ring buffer limpo p/ evitar resíduo da faixa anterior */
    size_t sz;
    void *item;
    while ((item = xRingbufferReceive(s_ring, &sz, 0)) != NULL) {
        vRingbufferReturnItem(s_ring, item);
    }

    char title[80];
    title_from_path(path, title, sizeof(title));
    post_evt(EVT_TRACK_INFO, (int32_t)s_total_s, title);
    post_evt(EVT_STATE, PST_PLAYING, NULL);
    set_route(current_route());

    app_config_t *cfg = appcfg_get();
    strlcpy(cfg->last_track, path, sizeof(cfg->last_track));
    appcfg_save();
    return true;
}

static void stop_track(void)
{
    s_playing = false;
    s_paused = false;
    wav_close(&s_dec);
    post_evt(EVT_STATE, PST_STOPPED, NULL);
    set_route(ROUTE_NONE);
}

/* ================= tarefa principal (Core 0) ================= */

static void pipeline_task(void *arg)
{
    /* MALLOC_CAP_8BIT: garante DRAM. Com INTERNAL puro o alocador pode
     * entregar IRAM, que NÃO aceita stores de 8/16 bits -> LoadStoreError
     * no primeiro store de amostra (crash reproduzível em toda reprodução). */
    int16_t *pcm = heap_caps_malloc(PCM_BLOCK_FRAMES * 2 * sizeof(int16_t),
                                    MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    uint8_t *mono8 = heap_caps_malloc(PCM_BLOCK_FRAMES,
                                      MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pcm || !mono8 || !esp_ptr_in_dram(pcm) || !esp_ptr_in_dram(mono8)) {
        ESP_LOGE(TAG, "sem memória DRAM para buffers do pipeline");
        vTaskDelete(NULL);
        return;
    }

    uint32_t frames_since_prog = 0;
    uint8_t silence[256];
    memset(silence, 0x80, sizeof(silence));
    bool dac_idle_silence = false;

    while (true) {
        /* ---- comandos ---- */
        pipe_cmd_t c;
        while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) {
            switch (c.id) {
            case PIPE_OPEN_FILE: open_track(c.path); frames_since_prog = 0; break;
            case PIPE_STOP:      stop_track(); break;
            case PIPE_PAUSE:
                if (s_playing && !s_paused) {
                    s_paused = true;
                    post_evt(EVT_STATE, PST_PAUSED, NULL);
                }
                break;
            case PIPE_RESUME:
                if (s_playing && s_paused) {
                    s_paused = false;
                    post_evt(EVT_STATE, PST_PLAYING, NULL);
                }
                break;
            default: break;
            }
        }

        /* ---- geração de áudio ---- */
        if (!s_playing || s_paused) {
            /* ocioso/pausado: empurra nível médio UMA vez no DAC (evita
             * estalos sem manter escritas contínuas) */
            if (appcfg_get()->local_out && !bt_is_connected() && !dac_idle_silence) {
                local_dac_write(silence, sizeof(silence), 100);
                dac_idle_silence = true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        dac_idle_silence = false;

        audio_route_t route = current_route();
        set_route(route);
        if (route == ROUTE_NONE) {
            /* nenhuma saída ativa: NÃO consome a faixa, apenas espera */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int frames = wav_read_stereo(&s_dec, pcm, PCM_BLOCK_FRAMES, s_volume);
        if (frames <= 0) {
            /* fim da faixa */
            ESP_LOGI(TAG, "fim de %s", s_cur_path);
            s_playing = false;
            post_evt(EVT_TRACK_END, 0, s_cur_path);
            if (s_track_end_cb) {
                s_track_end_cb();
            }
            continue;
        }

        if (route == ROUTE_BT) {
            size_t bytes = frames * 4;   /* estéreo 16 bits */
            if (xRingbufferSend(s_ring, pcm, bytes, pdMS_TO_TICKS(250)) != pdTRUE) {
                /* ring cheio e stream parado: descarta para não travar */
                ESP_LOGD(TAG, "ring cheio — bloco descartado");
            }
        } else {
            /* downmix estéreo 16 bits -> mono 8 bits para o DAC */
            for (int i = 0; i < frames; i++) {
                int32_t m = ((int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1]) / 2;
                mono8[i] = (uint8_t)((m >> 8) + 128);
            }
            local_dac_write(mono8, frames, 500);
        }

        /* ---- margem de pilha (diagnóstico, 1x por faixa) ---- */
        if (!hwm_logged) {
            hwm_logged = true;
            ESP_LOGI(TAG, "HWM audio_pipe: %u words livres no pico",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        /* ---- progresso (1 evento/s) ---- */
        frames_since_prog += frames;
        if (frames_since_prog >= AUDIO_SAMPLE_RATE) {
            frames_since_prog -= AUDIO_SAMPLE_RATE;
            if (s_elapsed_s < s_total_s) {
                s_elapsed_s++;
                post_evt(EVT_PROGRESS, (int32_t)s_elapsed_s, NULL);
            }
        }
    }
}

esp_err_t audio_pipeline_init(void)
{
    s_volume = appcfg_get()->volume;

    s_ring = xRingbufferCreate(AUDIO_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    s_cmd_q = xQueueCreate(4, sizeof(pipe_cmd_t));
    if (!s_cmd_q) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = local_dac_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DAC local indisponível (%s) — só Bluetooth", esp_err_to_name(err));
    } else {
        /* Bluetooth é a saída principal: o DAC só fica ativo se o usuário
         * habilitou o alto-falante on-board nos Ajustes. */
        local_dac_enable(appcfg_get()->local_out);
    }

    /* pilha generosa: a cadeia fread -> FATFS -> SDSPI -> log é profunda */
    BaseType_t ok = xTaskCreatePinnedToCore(pipeline_task, "audio_pipe", 8192,
                                            NULL, PRIO_AUDIO_PIPELINE, &s_task,
                                            CORE_AUDIO);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
