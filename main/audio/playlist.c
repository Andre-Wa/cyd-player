#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "playlist.h"
#include "esp_log.h"

static const char *TAG = "playlist";

static pl_entry_t s_entries[PL_MAX_ENTRIES];
static int s_count = 0;
static char s_dir[PL_PATH_LEN] = "";

static int cmp_entry(const void *a, const void *b)
{
    const pl_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;   /* pastas primeiro */
    }
    return strcasecmp(ea->name, eb->name);
}

static bool has_wav_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".wav") == 0;
}

bool playlist_load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "não foi possível abrir %s", dir);
        s_count = 0;
        return false;
    }
    strlcpy(s_dir, dir, sizeof(s_dir));
    s_count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_count < PL_MAX_ENTRIES) {
        if (ent->d_name[0] == '.') {
            continue;                 /* ocultos / "." / ".." */
        }
        bool is_dir = (ent->d_type == DT_DIR);
        if (!is_dir && ent->d_type == DT_UNKNOWN) {
            /* alguns FATFS não preenchem d_type: stat de fallback */
            char p[PL_PATH_LEN];
            snprintf(p, sizeof(p), "%.126s/%.63s", dir, ent->d_name);
            struct stat st;
            is_dir = (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
        }
        if (!is_dir && !has_wav_ext(ent->d_name)) {
            continue;
        }
        pl_entry_t *e = &s_entries[s_count++];
        e->is_dir = is_dir;
        strlcpy(e->fname, ent->d_name, sizeof(e->fname));
        if (is_dir) {
            strlcpy(e->name, ent->d_name, sizeof(e->name));
        } else {
            strlcpy(e->name, ent->d_name, sizeof(e->name));
            e->name[strlen(e->name) - 4] = '\0';   /* remove ".wav" */
        }
    }
    closedir(d);

    qsort(s_entries, s_count, sizeof(pl_entry_t), cmp_entry);
    ESP_LOGI(TAG, "%s: %d entradas", s_dir, s_count);
    return true;
}

int playlist_count(void)
{
    return s_count;
}

const pl_entry_t *playlist_get(int i)
{
    return (i >= 0 && i < s_count) ? &s_entries[i] : NULL;
}

const char *playlist_dir(void)
{
    return s_dir;
}

bool playlist_path(int i, char *out, int out_len)
{
    const pl_entry_t *e = playlist_get(i);
    if (!e) {
        return false;
    }
    if (strcmp(s_dir, "/") == 0) {
        snprintf(out, out_len, "/%s", e->fname);
    } else {
        snprintf(out, out_len, "%s/%s", s_dir, e->fname);
    }
    return true;
}

bool playlist_parent(char *out, int out_len)
{
    if (s_dir[0] == '\0' || strcmp(s_dir, "/sdcard") == 0 || strcmp(s_dir, "/") == 0) {
        return false;
    }
    strlcpy(out, s_dir, out_len);
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        return false;
    }
    *slash = '\0';
    return true;
}

int playlist_next_wav(int cur, int dir_step)
{
    if (s_count == 0) {
        return -1;
    }
    for (int k = 1; k <= s_count; k++) {
        int i = (cur + dir_step * k) % s_count;
        if (i < 0) {
            i += s_count;
        }
        if (!s_entries[i].is_dir) {
            return i;
        }
    }
    return -1;
}
