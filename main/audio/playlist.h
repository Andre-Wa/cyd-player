/* playlist.h — Varredura de diretórios do SD: subpastas + arquivos .wav */
#pragma once

#include <stdbool.h>

#define PL_MAX_ENTRIES   64
#define PL_NAME_LEN      64
#define PL_PATH_LEN      192

typedef struct {
    char name[PL_NAME_LEN];   /* nome exibido (sem extensão p/ arquivos) */
    char fname[PL_NAME_LEN];  /* nome real no disco */
    bool is_dir;
} pl_entry_t;

/* Carrega a lista do diretório 'dir' (substitui a anterior).
 * Ordena: pastas primeiro, depois *.wav, ambos em ordem alfabética. */
bool playlist_load_dir(const char *dir);

int  playlist_count(void);
const pl_entry_t *playlist_get(int i);
const char *playlist_dir(void);

/* Caminho completo da entrada i em 'out'. */
bool playlist_path(int i, char *out, int out_len);

/* Diretório-pai do diretório atual (false se já estiver na raiz do SD). */
bool playlist_parent(char *out, int out_len);

/* Próximo/anterior arquivo .wav a partir de 'cur' (wrapping).
 * Retorna o índice, ou -1 se não houver músicas. */
int playlist_next_wav(int cur, int dir_step);
