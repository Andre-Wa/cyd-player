/*
 * main.c — Ponto de entrada: NVS, filas de IPC, inicialização dos
 * subsistemas (SD, display, touch, áudio, Bluetooth) e a tarefa controladora
 * da máquina de estados da aplicação (spec §2.2, §5).
 *
 *   Core 0: pilha Bluedroid + bt_work + audio_pipeline (prio 20)
 *   Core 1: ui_render (prio 10) + touch (prio 5) + app_ctrl (prio 2)
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_avrc_api.h"

#include "app_config.h"
#include "app_types.h"
#include "storage/nvs_config.h"
#include "storage/sd_card.h"
#include "audio/audio_pipeline.h"
#include "audio/bt_a2dp_source.h"
#include "audio/playlist.h"
#include "audio/local_dac.h"
#include "display/lcd_spi_driver.h"
#include "display/touch_xpt2046.h"
#include "ui/ui_engine.h"
#include "ui/screens/screens_common.h"

static const char *TAG = "app_main";

/* filas globais de IPC (spec §5) */
QueueHandle_t g_ui_to_audio_queue;
QueueHandle_t g_audio_to_ui_queue;

/* ================== LED RGB de status ================== */

static void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_PIN_R) | (1ULL << LED_PIN_G) |
                        (1ULL << LED_PIN_B),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
#if LED_ACTIVE_LOW
    gpio_set_level(LED_PIN_R, 1);
    gpio_set_level(LED_PIN_G, 1);
    gpio_set_level(LED_PIN_B, 1);
#else
    gpio_set_level(LED_PIN_R, 0);
    gpio_set_level(LED_PIN_G, 0);
    gpio_set_level(LED_PIN_B, 0);
#endif
}

static void led_set(bool r, bool g, bool b)
{
#if LED_ACTIVE_LOW
    gpio_set_level(LED_PIN_R, !r);
    gpio_set_level(LED_PIN_G, !g);
    gpio_set_level(LED_PIN_B, !b);
#else
    gpio_set_level(LED_PIN_R, r);
    gpio_set_level(LED_PIN_G, g);
    gpio_set_level(LED_PIN_B, b);
#endif
}

/* ================== estado do controlador ================== */

static char s_cur_path[192];
static int  s_cur_index = -1;
static bool s_cfg_save_pending;
static int64_t s_cfg_last_change_ms;
static bool s_boot_ui_done;
static bool s_autoreconnect_done;

static void post_evt(evt_id_t id, int32_t val, const char *str)
{
    app_evt_t e = { .id = id, .val = val };
    if (str) {
        strlcpy(e.str, str, sizeof(e.str));
    }
    xQueueSend(g_audio_to_ui_queue, &e, pdMS_TO_TICKS(100));
}

static void request_cfg_save(void)
{
    s_cfg_save_pending = true;
    s_cfg_last_change_ms = ui_now_ms();
}

/* Garante que a playlist carregada contém a faixa atual (para NEXT/PREV). */
static void ensure_playlist(void)
{
    if (playlist_count() > 0 && s_cur_path[0]) {
        /* a faixa atual pertence à pasta carregada? */
        size_t dlen = strlen(playlist_dir());
        if (dlen && strncmp(playlist_dir(), s_cur_path, dlen) == 0) {
            return;
        }
    }
    /* recarrega a pasta da faixa atual (ou a raiz configurada) */
    char dir[192];
    strlcpy(dir, s_cur_path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir && s_cur_path[0]) {
        *slash = '\0';
    } else {
        strlcpy(dir, appcfg_get()->root_dir, sizeof(dir));
    }
    playlist_load_dir(dir);

    /* localiza o índice da faixa atual */
    s_cur_index = -1;
    const char *base = strrchr(s_cur_path, '/');
    base = base ? base + 1 : s_cur_path;
    for (int i = 0; i < playlist_count(); i++) {
        const pl_entry_t *e = playlist_get(i);
        if (e && !e->is_dir && strcmp(e->fname, base) == 0) {
            s_cur_index = i;
            appcfg_get()->last_index = (uint8_t)i;
            break;
        }
    }
}

static void play_index(int i)
{
    char path[PL_PATH_LEN];
    if (playlist_path(i, path, sizeof(path))) {
        s_cur_index = i;
        strlcpy(s_cur_path, path, sizeof(s_cur_path));
        appcfg_get()->last_index = (uint8_t)i;
        audio_pipeline_play_file(path);
    }
}

static void play_default_track(void)
{
    app_config_t *cfg = appcfg_get();
    if (cfg->last_track[0] && access(cfg->last_track, F_OK) == 0) {
        strlcpy(s_cur_path, cfg->last_track, sizeof(s_cur_path));
        audio_pipeline_play_file(s_cur_path);
        ensure_playlist();
        return;
    }
    ensure_playlist();
    int i = playlist_next_wav(-1, 1);
    if (i >= 0) {
        play_index(i);
    } else {
        post_evt(EVT_ERROR, 0, "Nenhum arquivo .wav no cartao");
    }
}

/* callback do pipeline (contexto da tarefa de áudio, Core 0) */
static void on_track_end(void)
{
    if (appcfg_get()->auto_next) {
        app_cmd_t c = { .id = CMD_NEXT };
        xQueueSend(g_ui_to_audio_queue, &c, 0);
    } else {
        audio_pipeline_stop();
    }
}

/* ================== controlador (Core 1) ================== */

static void handle_cmd(const app_cmd_t *c)
{
    app_config_t *cfg = appcfg_get();
    switch (c->id) {
    case CMD_PLAY_FILE:
        strlcpy(s_cur_path, c->str, sizeof(s_cur_path));
        s_cur_index = c->val;
        audio_pipeline_play_file(c->str);
        break;
    case CMD_PLAY:
        audio_pipeline_set_paused(false);
        break;
    case CMD_PAUSE:
        audio_pipeline_set_paused(true);
        break;
    case CMD_TOGGLE:
        if (audio_pipeline_is_playing()) {
            audio_pipeline_set_paused(true);
            bt_avrcp_passthrough(ESP_AVRC_PT_CMD_PAUSE, true);
            bt_avrcp_passthrough(ESP_AVRC_PT_CMD_PAUSE, false);
        } else if (audio_pipeline_is_active()) {   /* pausada: retomar */
            audio_pipeline_set_paused(false);
            bt_avrcp_passthrough(ESP_AVRC_PT_CMD_PLAY, true);
            bt_avrcp_passthrough(ESP_AVRC_PT_CMD_PLAY, false);
        } else {                                   /* parada: iniciar */
            play_default_track();
        }
        break;
    case CMD_STOP:
        audio_pipeline_stop();
        break;
    case CMD_NEXT:
        ensure_playlist();
        {
            int i = playlist_next_wav(s_cur_index, +1);
            if (i >= 0) {
                play_index(i);
            }
        }
        break;
    case CMD_PREV:
        if (audio_pipeline_elapsed_s() > 3) {
            audio_pipeline_play_file(s_cur_path);   /* reinicia a faixa */
        } else {
            ensure_playlist();
            int i = playlist_next_wav(s_cur_index, -1);
            if (i >= 0) {
                play_index(i);
            }
        }
        break;
    case CMD_SET_VOLUME:
        cfg->volume = (uint8_t)(c->val < 0 ? 0 : (c->val > 100 ? 100 : c->val));
        audio_pipeline_set_volume(cfg->volume);
        bt_avrcp_set_volume(cfg->volume);
        request_cfg_save();
        post_evt(EVT_VOLUME, cfg->volume, NULL);
        break;
    case CMD_SET_BRIGHTNESS:
        cfg->brightness = (uint8_t)(c->val < 0 ? 0 : (c->val > 100 ? 100 : c->val));
        lcd_set_backlight(cfg->brightness);
        request_cfg_save();
        post_evt(EVT_NONE, 0, NULL);       /* só invalida a UI */
        break;
    case CMD_SET_LOCAL_OUT:
        cfg->local_out = !!c->val;
        local_dac_enable(cfg->local_out);
        appcfg_save();
        post_evt(EVT_NONE, 0, NULL);
        break;
    case CMD_BT_SCAN:
        if (c->val < 0) {
            bt_stop_discovery();
        } else {
            bt_start_discovery();
        }
        break;
    case CMD_BT_CONNECT:
        bt_connect(c->mac);
        break;
    case CMD_BT_DISCONNECT:
        bt_disconnect();
        break;
    default:
        break;
    }
}

static void app_ctrl_task(void *arg)
{
    app_cmd_t cmd;
    int64_t boot_ms = ui_now_ms();
    led_set(false, false, true);          /* azul: iniciando */

    while (true) {
        if (xQueueReceive(g_ui_to_audio_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_cmd(&cmd);
        }

        int64_t now = ui_now_ms();

        /* gravação debounced da configuração (sliders geram muitos eventos) */
        if (s_cfg_save_pending && now - s_cfg_last_change_ms > 1200) {
            s_cfg_save_pending = false;
            appcfg_save();
        }

        /* LED de status + reconexão automática + UI inicial */
        bt_state_t bst = bt_get_state();
        switch (bst) {
        case BT_CONNECTED:  led_set(false, true, false); break;
        case BT_SCANNING:
        case BT_CONNECTING: led_set(false, false, true); break;
        default:            led_set(false, false, false); break;
        }

        if (!s_autoreconnect_done && bst == BT_READY && now - boot_ms > 4000) {
            s_autoreconnect_done = true;
            app_config_t *cfg = appcfg_get();
            if (cfg->has_bt) {
                ESP_LOGI(TAG, "reconectando ao último dispositivo: %s", cfg->bt_name);
                bt_connect(cfg->bt_mac);
            } else {
                /* Bluetooth é a saída principal: já inicia a varredura para
                 * deixar a lista de dispositivos pronta na tela BT. */
                ESP_LOGI(TAG, "nenhum dispositivo salvo — iniciando varredura BT");
                bt_start_discovery();
            }
        }

        if (!s_boot_ui_done && now - boot_ms > 2500) {
            s_boot_ui_done = true;
            app_config_t *cfg = appcfg_get();
            if (DEBUG_CALIB_EVERY_BOOT) {
                /* rodada de testes: seta de orientação em todo boot;
                 * confirmar (segurar 2 s) leva à calibração de touch */
                ui_show(SCREEN_ORIENT);
            } else if (!cfg->orient_ok) {
                ui_show(SCREEN_ORIENT);     /* quiz da seta (1ª vez) */
            } else if (!cfg->has_calib) {
                ui_show(SCREEN_CALIB);      /* primeira vez: calibrar o touch */
            } else {
                ui_show(SCREEN_PLAYER);
            }
        }
    }
}

/* ================== app_main ================== */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " CYD Audio Player v" APP_VERSION);
    ESP_LOGI(TAG, " ESP32-2432S028R | A2DP Source + SD");
    ESP_LOGI(TAG, " heap inicial: %u B", (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");

    /* NVS (configurações + chaves de pareamento do Bluedroid) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(appcfg_load());

    /* filas de IPC (spec §5) */
    g_ui_to_audio_queue = xQueueCreate(8, sizeof(app_cmd_t));
    g_audio_to_ui_queue = xQueueCreate(24, sizeof(app_evt_t));
    configASSERT(g_ui_to_audio_queue && g_audio_to_ui_queue);

    led_init();

    /* display + touch + engine de UI (mostra o splash imediatamente) */
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(xpt2046_init());
    if (appcfg_get()->has_calib) {
        xpt2046_set_calibration(appcfg_get()->calib);
    }
    lcd_set_xform(appcfg_get()->lcd_xform);
    ui_init();

    /* cartão SD (não fatal) */
    ret = sd_card_init();
    post_evt(EVT_SD, ret == ESP_OK ? 1 : 0, NULL);
    playlist_load_dir(appcfg_get()->root_dir);

    /* áudio (ring buffer, DAC local, tarefa Core 0) */
    ESP_ERROR_CHECK(audio_pipeline_init());
    audio_pipeline_set_volume(appcfg_get()->volume);
    audio_pipeline_set_track_end_cb(on_track_end);

    /* Bluetooth Classic A2DP Source (~130 KB de SRAM, spec §6) */
    ret = bt_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth indisponível (%s) — só alto-falante local",
                 esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "heap após BT: %u B livres", (unsigned)esp_get_free_heap_size());

    /* controlador da aplicação */
    xTaskCreatePinnedToCore(app_ctrl_task, "app_ctrl", 6144, NULL,
                            PRIO_APP_CTRL, NULL, CORE_UI);
}
