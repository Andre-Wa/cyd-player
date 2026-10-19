#include <string.h>
#include <stdio.h>
#include "nvs_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "nvs_cfg";
#define NVS_NAMESPACE "cfg"

static app_config_t s_cfg;

app_config_t *appcfg_get(void)
{
    return &s_cfg;
}

static void config_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.volume     = 65;
    s_cfg.vol_bt     = 65;
    s_cfg.vol_local  = 10;   /* amp SC8002D tem ganho alto; começa contido */
    s_cfg.local_out  = 0;    /* v1.1: Bluetooth é a saída principal */
    s_cfg.brightness = 90;
    s_cfg.auto_next  = 1;
    strlcpy(s_cfg.root_dir, SD_MOUNT_POINT, sizeof(s_cfg.root_dir));
}

esp_err_t appcfg_load(void)
{
    config_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* primeiro boot: namespace ainda não existe — padrões + salva */
        ESP_LOGI(TAG, "primeiro boot — criando configuração padrão");
        appcfg_save();
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open falhou (%s) — usando padrões", esp_err_to_name(err));
        return ESP_OK;
    }

    size_t sz;
    uint8_t u8;
    uint8_t ver = 0;
    nvs_get_u8(h, "ver", &ver);
    if (nvs_get_u8(h, "volume", &u8) == ESP_OK)     s_cfg.volume = u8 > 100 ? 100 : u8;
    bool have_vols = true;
    if (nvs_get_u8(h, "volbt", &u8) == ESP_OK)      s_cfg.vol_bt = u8 > 100 ? 100 : u8;
    else                                            have_vols = false;
    if (nvs_get_u8(h, "volloc", &u8) == ESP_OK)     s_cfg.vol_local = u8 > 100 ? 100 : u8;
    else                                            have_vols = false;
    if (!have_vols) {   /* migração: volume antigo era o da rota BT */
        s_cfg.vol_bt = s_cfg.volume;
        if (s_cfg.vol_local > 10) {
            s_cfg.vol_local = 10;
        }
    }
    if (nvs_get_u8(h, "local", &u8) == ESP_OK)      s_cfg.local_out = !!u8;
    if (nvs_get_u8(h, "bright", &u8) == ESP_OK)     s_cfg.brightness = u8 > 100 ? 100 : u8;
    if (nvs_get_u8(h, "autonext", &u8) == ESP_OK)   s_cfg.auto_next = !!u8;
    if (nvs_get_u8(h, "lastidx", &u8) == ESP_OK)    s_cfg.last_index = u8;
    if (nvs_get_u8(h, "orientok", &u8) == ESP_OK)   s_cfg.orient_ok = !!u8;
    if (nvs_get_u8(h, "xform", &u8) == ESP_OK) {
        s_cfg.lcd_xform = u8 > LCD_MODE_MAX ? 0 : u8;
    } else {
        uint8_t r180 = 0;   /* migração da v1.2.2 */
        if (nvs_get_u8(h, "rot180", &r180) == ESP_OK && r180) {
            s_cfg.lcd_xform = 1;
        }
    }

    sz = sizeof(s_cfg.root_dir);
    nvs_get_str(h, "rootdir", s_cfg.root_dir, &sz);
    sz = sizeof(s_cfg.last_track);
    nvs_get_str(h, "lasttrack", s_cfg.last_track, &sz);
    sz = sizeof(s_cfg.bt_name);
    if (nvs_get_str(h, "btname", s_cfg.bt_name, &sz) == ESP_OK && s_cfg.bt_name[0]) {
        s_cfg.has_bt = true;
    }

    sz = sizeof(s_cfg.bt_mac);
    if (nvs_get_blob(h, "btmac", s_cfg.bt_mac, &sz) == ESP_OK && sz == 6) {
        s_cfg.has_bt = true;
    }

    sz = sizeof(s_cfg.calib);
    if (nvs_get_blob(h, "calib", s_cfg.calib, &sz) == ESP_OK && sz == sizeof(s_cfg.calib)) {
        s_cfg.has_calib = true;
    }

    nvs_close(h);

    /* Migração de versão: v2 torna o Bluetooth a saída principal */
    if (ver < 2) {
        s_cfg.local_out = 0;
        ESP_LOGI(TAG, "config migrada para v%d (BT = saída principal)", APP_CFG_VER);
    }
    if (ver != APP_CFG_VER) {
        appcfg_save();
    }

    ESP_LOGI(TAG, "config: vol=%u%% local=%d bright=%u%% root=%s bt=%s calib=%d",
             s_cfg.volume, s_cfg.local_out, s_cfg.brightness, s_cfg.root_dir,
             s_cfg.has_bt ? s_cfg.bt_name : "-", s_cfg.has_calib);
    return ESP_OK;
}

esp_err_t appcfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open RW falhou: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, "ver", APP_CFG_VER);
    nvs_set_u8(h, "volume", s_cfg.volume);
    nvs_set_u8(h, "volbt", s_cfg.vol_bt);
    nvs_set_u8(h, "volloc", s_cfg.vol_local);
    nvs_set_u8(h, "local", !!s_cfg.local_out);
    nvs_set_u8(h, "bright", s_cfg.brightness);
    nvs_set_u8(h, "autonext", !!s_cfg.auto_next);
    nvs_set_u8(h, "lastidx", s_cfg.last_index);
    nvs_set_u8(h, "orientok", !!s_cfg.orient_ok);
    nvs_set_u8(h, "xform", s_cfg.lcd_xform);
    nvs_set_str(h, "rootdir", s_cfg.root_dir);
    nvs_set_str(h, "lasttrack", s_cfg.last_track);

    if (s_cfg.has_bt) {
        nvs_set_str(h, "btname", s_cfg.bt_name);
        nvs_set_blob(h, "btmac", s_cfg.bt_mac, sizeof(s_cfg.bt_mac));
    }
    if (s_cfg.has_calib) {
        nvs_set_blob(h, "calib", s_cfg.calib, sizeof(s_cfg.calib));
    }

    err = nvs_commit(h);
    nvs_close(h);

    /* NVS cheia? Faz o fallback: espelha no SD e tenta liberar espaço. */
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "NVS cheia — espelhando configuração no cartão SD");
        appcfg_mirror_to_sd();
    }
    return err;
}

bool appcfg_mirror_to_sd(void)
{
    char path[80];
    snprintf(path, sizeof(path), "%s/cyd.cfg", SD_MOUNT_POINT);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    /* Formato simples legível: chave=valor */
    fprintf(f, "# cyd_audio_player config mirror v1\n");
    fprintf(f, "volume=%u\n", s_cfg.volume);
    fprintf(f, "local=%u\n", s_cfg.local_out);
    fprintf(f, "bright=%u\n", s_cfg.brightness);
    fprintf(f, "autonext=%u\n", s_cfg.auto_next);
    fprintf(f, "rootdir=%s\n", s_cfg.root_dir);
    fprintf(f, "lasttrack=%s\n", s_cfg.last_track);
    fprintf(f, "lastidx=%u\n", s_cfg.last_index);
    if (s_cfg.has_bt) {
        fprintf(f, "btname=%s\n", s_cfg.bt_name);
        fprintf(f, "btmac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                s_cfg.bt_mac[0], s_cfg.bt_mac[1], s_cfg.bt_mac[2],
                s_cfg.bt_mac[3], s_cfg.bt_mac[4], s_cfg.bt_mac[5]);
    }
    if (s_cfg.has_calib) {
        fprintf(f, "calib=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                s_cfg.calib[0], s_cfg.calib[1], s_cfg.calib[2],
                s_cfg.calib[3], s_cfg.calib[4], s_cfg.calib[5]);
    }
    fclose(f);
    ESP_LOGI(TAG, "configuração espelhada em %s", path);
    return true;
}
