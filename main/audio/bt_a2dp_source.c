/*
 * bt_a2dp_source.c — Bluetooth Classic A2DP Source para o CYD.
 *
 * Arquitetura (spec §2.1):
 *   - Callbacks Bluedroid (tarefa BTC, Core 0) apenas extraem dados e
 *     enfileiram itens leves em s_work_q — nunca chamam APIs da pilha
 *     diretamente (risco de deadlock), padrão do exemplo oficial a2dp_source.
 *   - A tarefa bt_work (Core 0) processa os itens: publica eventos na
 *     audio_to_ui_queue e chama APIs A2DP/GAP/AVRCP com segurança.
 *   - O callback de dados A2DP puxa PCM do ring buffer do pipeline e o
 *     Bluedroid codifica em SBC (spec §4).
 */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "app_config.h"
#if APP_BT_ENABLE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "app_types.h"
#include "bt_a2dp_source.h"
#include "audio_pipeline.h"
#include "nvs_config.h"

static const char *TAG = "bt_src";

/* ================== sinais internos da work-queue ================== */
enum {
    SIG_GAP_DISC_RESULT = 1,   /* dispositivo descoberto */
    SIG_GAP_DISC_STOPPED,
    SIG_GAP_PIN_REQ,
    SIG_GAP_CFM_REQ,
    SIG_GAP_AUTH_CMPL,
    SIG_A2DP_CONNECTED,
    SIG_A2DP_DISCONNECTED,
    SIG_AUDIO_STARTED,
    SIG_AUDIO_STOPPED,
    SIG_AVRC_CONNECTED,
    SIG_AVRC_CAPS,
    SIG_AVRC_VOL_CHANGED,      /* sink alterou o volume (0..127) */
};

typedef struct {
    uint16_t sig;
    uint8_t  mac[6];
    int32_t  val;              /* rssi / volume / status */
    char     name[40];
} bt_work_t;

static QueueHandle_t s_work_q;
static volatile bt_state_t s_state = BT_OFF;
static volatile bool s_connected;
static volatile bool s_stream_started;
static TimerHandle_t s_media_tmr;
static int s_media_retries;
static char s_peer_name[32];
static uint8_t s_peer_mac[6];
static esp_avrc_rn_evt_cap_mask_t s_peer_rn_cap;

/* Alguns sinks não aceitam o primeiro AVDTP START logo após a conexão;
 * este timer repete o START a cada 2 s até o stream subir (máx. 5x). */
static void media_retry_cb(TimerHandle_t t)
{
    if (s_connected && !s_stream_started && s_media_retries < 5) {
        s_media_retries++;
        ESP_LOGW(TAG, "stream A2DP ainda não iniciou — START novamente (%d/5)",
                 s_media_retries);
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    }
}

#define TL_GET_CAPS   0
#define TL_RN_VOLUME  1
#define TL_SET_VOLUME 2
#define TL_PASSTHRU   3

/* ---------------- cache de descoberta (para nomes na conexão) -------------- */
#define DISC_CACHE_MAX 12
typedef struct {
    uint8_t mac[6];
    char name[32];
    int8_t rssi;
    bool rendering;
    bool used;
} disc_dev_t;
static disc_dev_t s_disc[DISC_CACHE_MAX];

/* ================== utilidades ================== */

static void post_ui(evt_id_t id, int32_t val, const char *str, const uint8_t mac[6])
{
    app_evt_t e = { .id = id, .val = val };
    if (str) {
        strlcpy(e.str, str, sizeof(e.str));
    }
    if (mac) {
        memcpy(e.mac, mac, 6);
    }
    if (g_audio_to_ui_queue) {
        xQueueSend(g_audio_to_ui_queue, &e, pdMS_TO_TICKS(50));
    }
}

static void set_state(bt_state_t st)
{
    s_state = st;
    post_ui(EVT_BT_STATE, st, NULL, NULL);
}

static void mac2str(const uint8_t mac[6], char *out)
{
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static disc_dev_t *disc_find_or_add(const uint8_t mac[6])
{
    disc_dev_t *free_slot = NULL;
    for (int i = 0; i < DISC_CACHE_MAX; i++) {
        if (s_disc[i].used && memcmp(s_disc[i].mac, mac, 6) == 0) {
            return &s_disc[i];
        }
        if (!s_disc[i].used && !free_slot) {
            free_slot = &s_disc[i];
        }
    }
    return free_slot;   /* NULL = cache cheio */
}

static bool name_from_eir(const uint8_t *eir, char *out, size_t out_len)
{
    uint8_t len = 0;
    const uint8_t *n = esp_bt_gap_resolve_eir_data((uint8_t *)eir,
                        ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!n) {
        n = esp_bt_gap_resolve_eir_data((uint8_t *)eir,
                    ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    }
    if (!n || len == 0) {
        return false;
    }
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, n, len);
    out[len] = '\0';
    return true;
}

static void enqueue_work(uint16_t sig, const uint8_t mac[6], int32_t val, const char *name)
{
    bt_work_t w = { .sig = sig, .val = val };
    if (mac) {
        memcpy(w.mac, mac, 6);
    }
    if (name) {
        strlcpy(w.name, name, sizeof(w.name));
    }
    if (s_work_q) {
        xQueueSend(s_work_q, &w, 0);
    }
}

/* ================== callbacks Bluedroid (contexto BTC) ================== */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        uint32_t cod = 0;
        int8_t rssi = 0;
        const uint8_t *eir = NULL;
        char name[40] = "";
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = (uint8_t *)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                strlcpy(name, (const char *)p->val,
                        p->len < sizeof(name) ? p->len + 1 : sizeof(name));
                break;
            default:
                break;
            }
        }
        if (!name[0] && eir) {
            name_from_eir(eir, name, sizeof(name));
        }
        bool rendering = esp_bt_gap_is_valid_cod(cod) &&
            ((esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING) ||
             esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV);
        if (!name[0] && !rendering) {
            break;    /* ignora dispositivos mudos que não renderizam áudio */
        }
        if (!name[0]) {
            mac2str(param->disc_res.bda, name);
        }
        enqueue_work(SIG_GAP_DISC_RESULT, param->disc_res.bda, rssi | ((int32_t)rendering << 8), name);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            enqueue_work(SIG_GAP_DISC_STOPPED, NULL, 0, NULL);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        enqueue_work(SIG_GAP_PIN_REQ, param->pin_req.bda,
                     param->pin_req.min_16_digit, NULL);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        enqueue_work(SIG_GAP_CFM_REQ, param->cfm_req.bda,
                     (int32_t)param->cfm_req.num_val, NULL);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey do par: %" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        enqueue_work(SIG_GAP_AUTH_CMPL, param->auth_cmpl.bda,
                     param->auth_cmpl.stat, (const char *)param->auth_cmpl.device_name);
        break;
    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            enqueue_work(SIG_A2DP_CONNECTED, param->conn_stat.remote_bda, 0, NULL);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            enqueue_work(SIG_A2DP_DISCONNECTED, param->conn_stat.remote_bda, 0, NULL);
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            enqueue_work(SIG_AUDIO_STARTED, NULL, 0, NULL);
        } else {
            enqueue_work(SIG_AUDIO_STOPPED, NULL, 0, NULL);
        }
        break;
    default:
        break;
    }
}

/* Callback de dados: chamado pela pilha enquanto houver stream ativo.
 * Puxa PCM 44,1k estéreo do ring buffer do pipeline; completa com silêncio
 * se houver underrun. O Bluedroid codifica em SBC (spec §4). */
static int32_t a2d_data_cb(uint8_t *buf, int32_t len)
{
    if (!buf || len <= 0) {
        return 0;
    }
    RingbufHandle_t rb = audio_pipeline_get_ring();
    size_t filled = 0;
    if (rb) {
        while (filled < (size_t)len) {
            size_t sz = 0;
            uint8_t *item = xRingbufferReceiveUpTo(rb, &sz, 0, (size_t)len - filled);
            if (!item) {
                break;
            }
            memcpy(buf + filled, item, sz);
            filled += sz;
            vRingbufferReturnItem(rb, item);
        }
    }
    if (filled < (size_t)len) {
        memset(buf + filled, 0, (size_t)len - filled);   /* silêncio */
    }
    return len;
}

/* AVRCP Target: o FONE é o controle remoto (botões de mídia e volume).
 * Os callbacks rodam na tarefa BTC: apenas enfileiram comandos p/ o
 * controlador da aplicação (nunca chamam a pilha daqui). */
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
        if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
            break;
        }
        app_cmd_t c = {0};
        switch (param->psth_cmd.key_code) {
        case ESP_AVRC_PT_CMD_PLAY:     c.id = CMD_PLAY;  break;
        case ESP_AVRC_PT_CMD_PAUSE:    c.id = CMD_PAUSE; break;
        case ESP_AVRC_PT_CMD_STOP:     c.id = CMD_STOP;  break;
        case ESP_AVRC_PT_CMD_FORWARD:  c.id = CMD_NEXT;  break;
        case ESP_AVRC_PT_CMD_BACKWARD: c.id = CMD_PREV;  break;
        default:
            ESP_LOGD(TAG, "AVRCP TG passthrough ignorado: 0x%02x",
                     param->psth_cmd.key_code);
            return;
        }
        ESP_LOGI(TAG, "AVRCP TG: tecla de midia do fone -> cmd %d", c.id);
        if (g_ui_to_audio_queue) {
            xQueueSend(g_ui_to_audio_queue, &c, 0);
        }
        break;
    }
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        /* usuário mexeu no volume do fone: sincroniza o volume do app */
        uint8_t v100 = (uint8_t)((param->set_abs_vol.volume * 100) / 127);
        ESP_LOGI(TAG, "AVRCP TG: volume do fone %u/127 -> %u%%",
                 param->set_abs_vol.volume, v100);
        app_cmd_t c = { .id = CMD_SET_VOLUME, .val = v100 };
        if (g_ui_to_audio_queue) {
            xQueueSend(g_ui_to_audio_queue, &c, 0);
        }
        break;
    }
    default:
        break;
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            enqueue_work(SIG_AVRC_CONNECTED, param->conn_stat.remote_bda, 0, NULL);
        } else {
            s_peer_rn_cap.bits = 0;
        }
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        enqueue_work(SIG_AVRC_CAPS, NULL, param->get_rn_caps_rsp.evt_set.bits, NULL);
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            enqueue_work(SIG_AVRC_VOL_CHANGED, NULL,
                         param->change_ntf.event_parameter.volume, NULL);
        }
        break;
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        ESP_LOGD(TAG, "AVRCP SetAbsoluteVolume rsp: %u", param->set_volume_rsp.volume);
        break;
    default:
        break;
    }
}

/* ================== tarefa de trabalho BT (Core 0) ================== */

static void bt_work_task(void *arg)
{
    bt_work_t w;
    while (true) {
        if (xQueueReceive(s_work_q, &w, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (w.sig) {
        case SIG_GAP_DISC_RESULT: {
            disc_dev_t *d = disc_find_or_add(w.mac);
            if (!d) {
                break;
            }
            bool is_new = !d->used;
            d->used = true;
            memcpy(d->mac, w.mac, 6);
            d->rssi = (int8_t)w.val;
            d->rendering = (w.val >> 8) & 1;
            if (w.name[0]) {
                strlcpy(d->name, w.name, sizeof(d->name));
            }
            if (is_new || w.name[0]) {
                post_ui(EVT_BT_DEVICE, d->rssi, d->name, d->mac);
            }
            break;
        }
        case SIG_GAP_DISC_STOPPED:
            if (s_state == BT_SCANNING) {
                set_state(s_connected ? BT_CONNECTED : BT_READY);
            }
            post_ui(EVT_BT_SCAN_DONE, 0, NULL, NULL);
            break;
        case SIG_GAP_PIN_REQ: {
            esp_bt_pin_code_t pin = { '1', '2', '3', '4' };
            esp_bt_gap_pin_reply(w.mac, true, w.val ? 16 : 4,
                                   w.val ? (esp_bt_pin_code_t){0} : pin);
            break;
        }
        case SIG_GAP_CFM_REQ:
            /* pareamento SSP: confirma automaticamente (alto-falantes típicos) */
            esp_bt_gap_ssp_confirm_reply(w.mac, true);
            break;
        case SIG_GAP_AUTH_CMPL:
            if (w.val == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "pareamento ok com %s", w.name);
            } else {
                ESP_LOGW(TAG, "pareamento falhou (%d) com %s", (int)w.val, w.name);
                post_ui(EVT_ERROR, 0, "Falha no pareamento BT", NULL);
            }
            break;
        case SIG_A2DP_CONNECTED: {
            s_connected = true;
            memcpy(s_peer_mac, w.mac, 6);
            /* nome: procura no cache de descoberta */
            s_peer_name[0] = '\0';
            for (int i = 0; i < DISC_CACHE_MAX; i++) {
                if (s_disc[i].used && memcmp(s_disc[i].mac, w.mac, 6) == 0) {
                    strlcpy(s_peer_name, s_disc[i].name, sizeof(s_peer_name));
                    break;
                }
            }
            if (!s_peer_name[0]) {
                mac2str(w.mac, s_peer_name);
            }
            audio_pipeline_notify_bt_stream(false);   /* aguarda AUDIO_STARTED */
            s_stream_started = false;
            s_media_retries = 0;
            if (!s_media_tmr) {
                s_media_tmr = xTimerCreate("a2dp_start", pdMS_TO_TICKS(2000),
                                           pdTRUE, NULL, media_retry_cb);
            }
            if (s_media_tmr) {
                xTimerStart(s_media_tmr, 0);
            }
            set_state(BT_CONNECTED);
            post_ui(EVT_BT_STATE, BT_CONNECTED, s_peer_name, s_peer_mac);
            ESP_LOGI(TAG, "A2DP conectado a %s", s_peer_name);

            /* salva como dispositivo preferido (reconexão automática) */
            app_config_t *cfg = appcfg_get();
            cfg->has_bt = true;
            memcpy(cfg->bt_mac, w.mac, 6);
            strlcpy(cfg->bt_name, s_peer_name, sizeof(cfg->bt_name));
            appcfg_save();

            /* inicia o stream AVDTP */
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            break;
        }
        case SIG_A2DP_DISCONNECTED:
            s_connected = false;
            s_stream_started = false;
            if (s_media_tmr) {
                xTimerStop(s_media_tmr, 0);
            }
            audio_pipeline_notify_bt_stream(false);
            set_state(BT_READY);
            post_ui(EVT_BT_STATE, BT_READY, NULL, NULL);
            ESP_LOGI(TAG, "A2DP desconectado");
            break;
        case SIG_AUDIO_STARTED:
            s_stream_started = true;
            if (s_media_tmr) {
                xTimerStop(s_media_tmr, 0);
            }
            audio_pipeline_notify_bt_stream(true);
            ESP_LOGI(TAG, "stream A2DP iniciado (SBC)");
            break;
        case SIG_AUDIO_STOPPED:
            audio_pipeline_notify_bt_stream(false);
            break;
        case SIG_AVRC_CONNECTED:
            esp_avrc_ct_send_get_rn_capabilities_cmd(TL_GET_CAPS);
            break;
        case SIG_AVRC_CAPS:
            s_peer_rn_cap.bits = (uint16_t)w.val;
            if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                    &s_peer_rn_cap, ESP_AVRC_RN_VOLUME_CHANGE)) {
                esp_avrc_ct_send_register_notification_cmd(TL_RN_VOLUME,
                        ESP_AVRC_RN_VOLUME_CHANGE, 0);
                ESP_LOGI(TAG, "AVRCP: volume absoluto suportado pelo sink");
            }
            break;
        case SIG_AVRC_VOL_CHANGED: {
            /* usuário mexeu no volume do alto-falante: sincroniza o app */
            uint8_t v100 = (uint8_t)((w.val * 100) / 127);
            app_config_t *cfg = appcfg_get();
            cfg->volume = v100;
            audio_pipeline_set_volume(v100);
            appcfg_save();
            post_ui(EVT_VOLUME, v100, NULL, NULL);
            /* rearma a notificação */
            esp_avrc_ct_send_register_notification_cmd(TL_RN_VOLUME,
                    ESP_AVRC_RN_VOLUME_CHANGE, 0);
            break;
        }
        default:
            break;
        }
    }
}

/* ================== API pública ================== */

bool bt_is_connected(void)
{
    return s_connected;
}

bt_state_t bt_get_state(void)
{
    return s_state;
}

void bt_get_peer(char *name, int name_len, uint8_t mac[6])
{
    if (s_connected) {
        if (name) {
            strlcpy(name, s_peer_name, name_len);
        }
        if (mac) {
            memcpy(mac, s_peer_mac, 6);
        }
    } else {
        if (name) {
            name[0] = '\0';
        }
        if (mac) {
            memset(mac, 0, 6);
        }
    }
}

void bt_start_discovery(void)
{
    memset(s_disc, 0, sizeof(s_disc));
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0) == ESP_OK) {
        set_state(BT_SCANNING);
        ESP_LOGI(TAG, "descoberta iniciada (~10 s)");
    } else {
        ESP_LOGE(TAG, "falha ao iniciar descoberta");
        post_ui(EVT_ERROR, 0, "Falha ao escanear BT", NULL);
    }
}

void bt_stop_discovery(void)
{
    esp_bt_gap_cancel_discovery();
}

void bt_connect(const uint8_t mac[6])
{
    esp_bt_gap_cancel_discovery();
    set_state(BT_CONNECTING);
    ESP_LOGI(TAG, "conectando a %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (esp_a2d_source_connect((uint8_t *)mac) != ESP_OK) {
        set_state(BT_READY);
        post_ui(EVT_ERROR, 0, "Falha ao conectar BT", NULL);
    }
}

void bt_disconnect(void)
{
    if (s_connected) {
        esp_a2d_source_disconnect(s_peer_mac);
    }
}

void bt_avrcp_set_volume(uint8_t vol100)
{
    if (!s_connected) {
        return;
    }
    uint8_t v127 = (uint8_t)((vol100 > 100 ? 100 : vol100) * 127 / 100);
    esp_avrc_ct_send_set_absolute_volume_cmd(TL_SET_VOLUME, v127);
}

void bt_avrcp_passthrough(uint8_t cmd, bool pressed)
{
    if (s_connected) {
        esp_avrc_ct_send_passthrough_cmd(TL_PASSTHRU, cmd,
            pressed ? ESP_AVRC_PT_CMD_STATE_PRESSED : ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
}

esp_err_t bt_audio_init(void)
{
    s_work_q = xQueueCreate(24, sizeof(bt_work_t));
    if (!s_work_q) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(bt_work_task, "bt_work", 3072, NULL,
                            PRIO_AUDIO_PIPELINE - 1, NULL, CORE_AUDIO);

    /* libera memória do controller BLE (não usado) */
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mem_release BLE: %s", esp_err_to_name(err));
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(err));
        goto fail;
    }

    /* GAP (descoberta/pareamento) */
    esp_bt_gap_set_device_name(APP_BT_DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_register_callback(gap_cb);

    /* AVRCP CT + TG (volume absoluto) */
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(avrc_ct_cb);
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(avrc_tg_cb);
    /* aceita play/pause/stop/forward/backward vindos do fone */
    esp_avrc_psth_bit_mask_t psth = {0};
    const uint8_t keys[] = { ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE,
                             ESP_AVRC_PT_CMD_STOP, ESP_AVRC_PT_CMD_FORWARD,
                             ESP_AVRC_PT_CMD_BACKWARD };
    for (size_t i = 0; i < sizeof(keys); i++) {
        psth.bits[keys[i] >> 4] |= (uint16_t)(1u << (keys[i] & 0x0F));
    }
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &psth);
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set,
                                       ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);

    /* A2DP Source */
    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2dp source init: %s", esp_err_to_name(err));
        goto fail;
    }
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);

    set_state(BT_READY);
    ESP_LOGI(TAG, "A2DP Source pronto como \"%s\" (heap livre: %u B)",
             APP_BT_DEVICE_NAME, (unsigned)esp_get_free_heap_size());
    return ESP_OK;

fail:
    s_state = BT_OFF;
    post_ui(EVT_BT_STATE, BT_OFF, NULL, NULL);
    post_ui(EVT_ERROR, 0, "Bluetooth indisponivel", NULL);
    return err;
}

#else  /* =================== build sem Bluetooth (APP_BT_ENABLE=0) ========== */

#include "esp_log.h"
#include "app_types.h"
#include "bt_a2dp_source.h"

static const char *TAG = "bt_src";

esp_err_t bt_audio_init(void)
{
    ESP_LOGW(TAG, "build sem Bluetooth (APP_BT_ENABLE=0)");
    return ESP_ERR_NOT_SUPPORTED;
}
bool bt_is_connected(void) { return false; }
bt_state_t bt_get_state(void) { return BT_OFF; }
void bt_get_peer(char *name, int name_len, uint8_t mac[6])
{
    if (name && name_len) { name[0] = '\0'; }
    if (mac) { memset(mac, 0, 6); }
}
void bt_start_discovery(void) {}
void bt_stop_discovery(void) {}
void bt_connect(const uint8_t mac[6]) { (void)mac; }
void bt_disconnect(void) {}
void bt_avrcp_set_volume(uint8_t vol100) { (void)vol100; }
void bt_avrcp_passthrough(uint8_t cmd, bool pressed) { (void)cmd; (void)pressed; }

#endif /* APP_BT_ENABLE */
