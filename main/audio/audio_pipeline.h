/*
 * audio_pipeline.h — Pipeline de áudio (§2.1/§4 da spec): tarefa no Core 0,
 * prioridade 20. Lê WAV do SD, converte para PCM estéreo 44,1 kHz/16 bits e
 * alimenta a saída ativa:
 *
 *   BT conectado  -> RingBuffer de 16 KB -> callback de dados A2DP -> SBC
 *   Sem BT        -> DAC local (SC8002D, mono 8 bits), se local_out ativo
 *
 * A troca de rota é automática e sem parar a reprodução.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "app_types.h"

#define AUDIO_RING_BYTES   (16 * 1024)   /* ~93 ms de PCM 44,1k estéreo */

/* Inicializa ring buffer, DAC local e cria a tarefa do pipeline (Core 0). */
esp_err_t audio_pipeline_init(void);

/* Comandos thread-safe (postados para a tarefa do pipeline). */
void audio_pipeline_play_file(const char *path);
void audio_pipeline_stop(void);
void audio_pipeline_set_paused(bool paused);
void audio_pipeline_set_volume(uint8_t vol);   /* 0..100 */

/* Consulta (voláteis, leitura direta). */
bool audio_pipeline_is_playing(void);
audio_route_t audio_pipeline_get_route(void);
bool audio_pipeline_is_active(void);   /* faixa carregada (tocando ou pausada) */
uint32_t audio_pipeline_elapsed_s(void);
uint32_t audio_pipeline_total_s(void);

/* Ring buffer consumido pelo callback de dados do A2DP Source. */
RingbufHandle_t audio_pipeline_get_ring(void);

/* Chamado pelo módulo BT quando o stream A2DP inicia/suspende. */
void audio_pipeline_notify_bt_stream(bool started);

/* Registra callback de fim de faixa (chamado no contexto da tarefa de áudio;
 * deve ser leve — ex.: postar CMD_NEXT na fila de comandos). */
void audio_pipeline_set_track_end_cb(void (*cb)(void));
