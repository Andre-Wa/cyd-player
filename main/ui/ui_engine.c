#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "ui_engine.h"
#include "lcd_spi_driver.h"
#include "touch_xpt2046.h"
#include "nvs_config.h"

/* telas definidas em ui/screens */
extern const ui_screen_t player_screen;
extern const ui_screen_t browser_screen;
extern const ui_screen_t bt_screen;
extern const ui_screen_t settings_screen;
extern const ui_screen_t calib_screen;
extern const ui_screen_t orient_screen;

static const char *TAG = "ui";

/* ================== modelo compartilhado ================== */
ui_model_t g_ui;

/* ================== splash (interna da engine) ================== */
static void splash_draw(gfx_canvas_t *c)
{
    gfx_fill(c, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG);
    gfx_text(c, 96, 60, "CYD Audio", COL_ACCENT, 2);
    gfx_text(c, 116, 100, "Player v" APP_VERSION, COL_MUTED, 1);
    gfx_text(c, 88, 150, "Iniciando sistema...", COL_TEXT, 1);
    char st[64];
    snprintf(st, sizeof(st), "SD: %s", g_ui.sd_ok ? "ok" : "ausente");
    gfx_text(c, 128, 176, st, g_ui.sd_ok ? COL_OK : COL_ERR, 1);
    snprintf(st, sizeof(st), "BT: %s",
             g_ui.bt_state >= BT_READY ? "ok" : "inicializando");
    gfx_text(c, 128, 196, st, g_ui.bt_state >= BT_READY ? COL_OK : COL_MUTED, 1);
}

static const ui_screen_t splash_screen = {
    .name = "splash",
    .draw = splash_draw,
};

static const ui_screen_t *s_screens[SCREEN_COUNT];

/* ================== estado interno ================== */
#define N_STRIPS ((LCD_V_RES + LCD_STRIP_LINES - 1) / LCD_STRIP_LINES)

static uint16_t *s_buf[2];
static volatile uint32_t s_dirty;         /* bitmask de tiras sujas */
static QueueHandle_t s_touch_q;
static QueueHandle_t s_ctrl_q;
static screen_id_t s_cur = SCREEN_SPLASH;


typedef struct { screen_id_t id; } ctrl_msg_t;

int64_t ui_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void ui_invalidate_all(void)
{
    s_dirty = (1u << N_STRIPS) - 1;
}

void ui_show(screen_id_t id)
{
    if (id >= SCREEN_COUNT) {
        return;
    }
    ctrl_msg_t m = { .id = id };
    if (s_ctrl_q) {
        xQueueSend(s_ctrl_q, &m, pdMS_TO_TICKS(100));
    }
}

/* ================== renderização por tiras (ping-pong DMA) ================== */

static void render_strips(uint32_t mask)
{
    const ui_screen_t *scr = s_screens[s_cur];
    if (!scr || !scr->draw) {
        return;
    }
    int pending = 0;
    for (int n = 0; n < N_STRIPS; n++) {
        if (!(mask & (1u << n))) {
            continue;
        }
        int y = n * LCD_STRIP_LINES;
        int h = LCD_V_RES - y < LCD_STRIP_LINES ? LCD_V_RES - y : LCD_STRIP_LINES;

        /* renderiza a tira n ENQUANTO o DMA da tira n-1 ainda está em voo
         * (buffer n&1 foi liberado: o flush de n-1 só é emitido após a
         * conclusão de n-2) */
        gfx_canvas_t cv;
        gfx_canvas_init(&cv, s_buf[n & 1], LCD_H_RES, h, 0, y);
        scr->draw(&cv);

        if (pending) {
            lcd_wait_one();
            pending--;
        }
        lcd_flush_async(0, y, LCD_H_RES, h, s_buf[n & 1]);
        pending++;
    }
    while (pending > 0) {
        lcd_wait_one();
        pending--;
    }
}

/* ================== tratamento de eventos (modelo) ================== */

static void handle_event(const app_evt_t *e)
{
    switch (e->id) {
    case EVT_STATE:
        g_ui.pstate = (player_state_t)e->val;
        break;
    case EVT_TRACK_INFO:
        strlcpy(g_ui.title, e->str, sizeof(g_ui.title));
        g_ui.total_s = (uint32_t)e->val;
        g_ui.elapsed_s = 0;
        break;
    case EVT_PROGRESS:
        g_ui.elapsed_s = (uint32_t)e->val;
        break;
    case EVT_ROUTE:
        g_ui.route = (audio_route_t)e->val;
        break;
    case EVT_BT_STATE:
        g_ui.bt_state = (bt_state_t)e->val;
        if (e->str[0]) {
            strlcpy(g_ui.bt_peer, e->str, sizeof(g_ui.bt_peer));
        }
        if (g_ui.bt_state != BT_CONNECTED) {
            g_ui.bt_peer[0] = '\0';
        }
        if (g_ui.bt_state == BT_SCANNING) {
            g_ui.n_devices = 0;    /* nova varredura: limpa a lista */
        }
        break;
    case EVT_BT_DEVICE: {
        /* insere/atualiza mantendo a lista sem duplicatas */
        int slot = -1;
        for (int i = 0; i < g_ui.n_devices; i++) {
            if (memcmp(g_ui.devices[i].mac, e->mac, 6) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (g_ui.n_devices >= UI_MAX_DEVICES) {
                return;
            }
            slot = g_ui.n_devices++;
            memcpy(g_ui.devices[slot].mac, e->mac, 6);
        }
        if (e->str[0]) {
            strlcpy(g_ui.devices[slot].name, e->str,
                    sizeof(g_ui.devices[slot].name));
        }
        g_ui.devices[slot].rssi = (int8_t)e->val;
        break;
    }
    case EVT_BT_SCAN_DONE:
        break;
    case EVT_SD:
        g_ui.sd_ok = !!e->val;
        break;
    case EVT_VOLUME:
        break;   /* config já atualizada pelo módulo BT; slider lê config */
    case EVT_ERROR:
        strlcpy(g_ui.error, e->str, sizeof(g_ui.error));
        g_ui.error_until_ms = ui_now_ms() + 4000;
        break;
    default:
        break;
    }
    ui_invalidate_all();
}

/* ================== tarefa de UI (Core 1, prio 10) ================== */

static void ui_render_task(void *arg)
{
    bool toast_active = false;
    while (true) {
        /* 1) troca de tela */
        ctrl_msg_t m;
        while (xQueueReceive(s_ctrl_q, &m, 0) == pdTRUE) {
            if (m.id != s_cur) {
                if (s_screens[s_cur] && s_screens[s_cur]->on_exit) {
                    s_screens[s_cur]->on_exit();
                }
                s_cur = m.id;
                if (s_screens[s_cur] && s_screens[s_cur]->on_enter) {
                    s_screens[s_cur]->on_enter();
                }
                ESP_LOGI(TAG, "tela -> %s", s_screens[s_cur]->name);
                ui_invalidate_all();
            }
        }

        /* 2) eventos do áudio/BT */
        app_evt_t e;
        while (xQueueReceive(g_audio_to_ui_queue, &e, 0) == pdTRUE) {
            handle_event(&e);
        }

        /* 3) eventos de touch -> tela atual */
        touch_evt_t te;
        while (xQueueReceive(s_touch_q, &te, 0) == pdTRUE) {
            const ui_screen_t *scr = s_screens[s_cur];
            if (scr && scr->on_touch) {
                scr->on_touch(&te);
            }
        }

        /* 4) expiração do toast de erro */
        if (g_ui.error_until_ms && ui_now_ms() > g_ui.error_until_ms) {
            g_ui.error_until_ms = 0;
            ui_invalidate_all();
        }
        toast_active = g_ui.error_until_ms != 0;

        /* 5) renderiza tiras sujas */
        uint32_t mask = s_dirty;
        if (mask) {
            s_dirty = 0;
            render_strips(mask);
        }

        vTaskDelay(pdMS_TO_TICKS(toast_active ? 100 : 25));
    }
}

/* ================== tarefa de touch (Core 1, prio 5) ================== */

static void touch_task(void *arg)
{
    bool pressed = false, dragging = false, long_sent = false;
    int px = 0, py = 0, lx = 0, ly = 0;
    uint16_t prx = 0, pry = 0;
    int64_t t0 = 0;

    while (true) {
        uint16_t x, y, rx, ry;
        bool down = xpt2046_read_full(&x, &y, &rx, &ry);
        touch_evt_t ev = {0};

        if (down && !pressed) {
            pressed = true;
            dragging = false;
            long_sent = false;
            px = lx = x;
            py = ly = y;
            prx = rx;
            pry = ry;
            t0 = ui_now_ms();
            ev.type = TEV_PRESS;
            ev.x = x; ev.y = y; ev.rx = rx; ev.ry = ry;
            xQueueSend(s_touch_q, &ev, 0);
        } else if (down && pressed) {
            if (!long_sent && ui_now_ms() - t0 >= 1200) {
                long_sent = true;
                ev.type = TEV_LONG;
                ev.x = px; ev.y = py; ev.rx = prx; ev.ry = pry;
                xQueueSend(s_touch_q, &ev, 0);
            }
            int dx = x - lx, dy = y - ly;
            if (!dragging && (abs(x - px) > 10 || abs(y - py) > 10)) {
                dragging = true;
            }
            if (dragging && (abs(dx) >= 6 || abs(dy) >= 6)) {
                ev.type = TEV_DRAG;
                ev.x = x; ev.y = y; ev.dx = dx; ev.dy = dy;
                ev.rx = rx; ev.ry = ry;
                xQueueSend(s_touch_q, &ev, 0);
                lx = x;
                ly = y;
            }
        } else if (!down && pressed) {
            pressed = false;
            if (!dragging && !long_sent && ui_now_ms() - t0 < 800) {
                ev.type = TEV_TAP;
                ev.x = px; ev.y = py; ev.rx = prx; ev.ry = pry;
                xQueueSend(s_touch_q, &ev, 0);
            }
            ev.type = TEV_RELEASE;
            ev.x = px; ev.y = py;
            xQueueSend(s_touch_q, &ev, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(20));    /* spec §2.2: amostragem 20 ms */
    }
}

/* ================== init ================== */

void ui_init(void)
{
    memset(&g_ui, 0, sizeof(g_ui));

    s_screens[SCREEN_SPLASH]   = &splash_screen;
    s_screens[SCREEN_PLAYER]   = &player_screen;
    s_screens[SCREEN_BROWSER]  = &browser_screen;
    s_screens[SCREEN_BT]       = &bt_screen;
    s_screens[SCREEN_SETTINGS] = &settings_screen;
    s_screens[SCREEN_CALIB]    = &calib_screen;
    s_screens[SCREEN_ORIENT]   = &orient_screen;

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(LCD_STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "sem memória DMA para buffer de tira %d", i);
            return;
        }
        memset(s_buf[i], 0, LCD_STRIP_BYTES);
    }

    s_touch_q = xQueueCreate(8, sizeof(touch_evt_t));
    s_ctrl_q = xQueueCreate(4, sizeof(ctrl_msg_t));

    xTaskCreatePinnedToCore(ui_render_task, "ui_render", 4096, NULL,
                            PRIO_UI_RENDER, NULL, CORE_UI);
    xTaskCreatePinnedToCore(touch_task, "touch", 2560, NULL,
                            PRIO_TOUCH, NULL, CORE_UI);

    ui_invalidate_all();
    ESP_LOGI(TAG, "engine de UI pronta (%d tiras de %d px, buffers 2x%d B)",
             N_STRIPS, LCD_STRIP_LINES, LCD_STRIP_BYTES);
}
