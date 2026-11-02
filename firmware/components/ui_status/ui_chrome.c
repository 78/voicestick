/*
 * Kopfzeilen-Helfer (Paket J, 0.5.0): das Zurueck der Unterszenen, die
 * Zeichen fuer Lautlos und "Dunkel & still", der rote Mikrofon-Punkt und die
 * Messung der Renderzeit. Was davon wofuer da ist, steht in ui_chrome.h und
 * include/ui_status.h. Auf dem StickS3 tun die oeffentlichen Funktionen
 * nichts -- dort gibt es weder Kopfzeilen-Zeichen noch Diagnose-Seite.
 */
#include "ui_status.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_VOICE_BOARD_WATCH206
#include "esp_log.h"
#include "esp_timer.h"
#include "ui_chrome.h"
#include "ui_control.h"
#include "ui_glyph.h"
#include "ui_layout_watch206.h"
#include "ui_status_internal.h"

static const char *TAG = "ui_chrome";

/* ── Zurueck ───────────────────────────────────────────────────────────── */

lv_obj_t *ui_back_create(lv_obj_t *parent)
{
    lv_obj_t *back = lv_obj_create(parent);
    lv_obj_remove_style_all(back);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(back, UI_BACK_W, UI_BACK_H);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, UI_BACK_X, UI_BACK_Y);
    lv_obj_t *glyph = ui_glyph_create(back, 28);
    lv_obj_center(glyph);
    ui_glyph_set(glyph, UI_GLYPH_BACK, true, 0xc9d1d9);
    return back;
}

bool ui_back_hit(lv_obj_t *back, int16_t x, int16_t y)
{
    if (!back || lv_obj_has_flag(back, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_area_t area;
    lv_obj_get_coords(back, &area);
    return x >= area.x1 - UI_HIT_TOLERANCE && x <= area.x2 + UI_HIT_TOLERANCE &&
           y >= area.y1 - UI_HIT_TOLERANCE && y <= area.y2 + UI_HIT_TOLERANCE;
}

/* ── Zeichen in der Kopfzeile ──────────────────────────────────────────── */

/*
 * Lautlos und "Dunkel & still" (Punkt 29): zwei kleine Zeichen rechts neben
 * der kleinen Uhrzeit, auf lv_layer_top() und damit ueber jeder Szene, auch
 * auf dem Zifferblatt. Die Uhrzeit misst in der 18er hoechstens 52 px und
 * steht mittig (x 179..231 absolut); die Zeichen beginnen bei x 240 und
 * enden vor x 290 -- weit vor der Akkuzahl (ab x 306) und dem "›" der
 * Karten (ab x 344).
 *
 * Der Mikrofon-Punkt (Punkt 34): 8 px rot oben mittig, ueber der Uhrzeit,
 * solange das Mikrofon offen ist -- in jeder Szene, auch wenn eine Karte
 * oder das Kontrollzentrum darueber liegt.
 */
#define MARK_SIZE 20
#define MARK_X 240
#define MARK_GAP 6
#define MIC_DOT 8
#define MIC_Y 6

static lv_obj_t *s_mark_silent;
static lv_obj_t *s_mark_still;
static lv_obj_t *s_mic;
static int32_t s_mark_y;
static int32_t s_still_x;
static bool s_silent;
static bool s_still;
static bool s_mic_open;

/* ── Renderzeit ────────────────────────────────────────────────────────── */

/*
 * Punkt 47: Wie lange LVGL fuer ein Bild braucht und wie viele Bilder je
 * Sekunde es zeichnet. Gemessen von LV_EVENT_REFR_START (Anfang des
 * Durchgangs: Layout, Zeichnen, Uebertragen) bis LV_EVENT_RENDER_READY --
 * das kommt nur, wenn wirklich etwas neu gezeichnet wurde; Durchgaenge ohne
 * ungueltige Flaechen zaehlen also nicht als Bild. Alle zehn Sekunden (mit
 * dem naechsten Bild danach) wird zusammengefasst: ins Log (Stufe Debug) und
 * als Wert fuer die Diagnose-Seite (ui_status_render_stats). Kein eigener
 * Zeitgeber, keine eigene Task: Beides laeuft in den Ereignissen der Anzeige.
 */
#define RENDER_WINDOW_US (10 * 1000 * 1000LL)

static int64_t s_refr_start_us;
static int64_t s_window_start_us;
static uint32_t s_window_frames;
static int64_t s_window_render_us;
static uint32_t s_window_max_us;
static ui_render_stats_t s_stats;
static bool s_stats_valid;

static void render_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const int64_t now = esp_timer_get_time();
    if (code == LV_EVENT_REFR_START) {
        s_refr_start_us = now;
        if (s_window_start_us == 0) {
            s_window_start_us = now;
        }
        return;
    }
    if (code != LV_EVENT_RENDER_READY || s_refr_start_us == 0) {
        return;
    }
    const int64_t took = now - s_refr_start_us;
    s_refr_start_us = 0;
    s_window_frames++;
    s_window_render_us += took;
    if (took > (int64_t)s_window_max_us) {
        s_window_max_us = took > UINT32_MAX ? UINT32_MAX : (uint32_t)took;
    }
    const int64_t span = now - s_window_start_us;
    if (span < RENDER_WINDOW_US) {
        return;
    }
    s_stats.frames = s_window_frames;
    s_stats.window_s = (uint16_t)(span / 1000000);
    s_stats.fps_x10 = (uint16_t)((int64_t)s_window_frames * 10000000LL / span);
    s_stats.avg_ms_x10 = (uint16_t)(s_window_render_us / 100 / s_window_frames);
    s_stats.max_ms = (uint16_t)(s_window_max_us / 1000);
    s_stats_valid = true;
    ESP_LOGD(TAG, "Rendern: %u Bilder in %u s, %u.%u Bilder/s, Mittel %u.%u ms, max %u ms",
             (unsigned)s_stats.frames, (unsigned)s_stats.window_s,
             (unsigned)(s_stats.fps_x10 / 10), (unsigned)(s_stats.fps_x10 % 10),
             (unsigned)(s_stats.avg_ms_x10 / 10), (unsigned)(s_stats.avg_ms_x10 % 10),
             (unsigned)s_stats.max_ms);
    s_window_start_us = now;
    s_window_frames = 0;
    s_window_render_us = 0;
    s_window_max_us = 0;
}

void ui_chrome_create_locked(lv_display_t *display)
{
    if (s_mic) {
        return;
    }
    s_mark_y = UI_SCREEN_PAD + UI_HEADER_LABEL_Y + lv_font_get_line_height(UI_FONT_SMALL) / 2 -
               MARK_SIZE / 2;
    s_mark_silent = ui_glyph_create(lv_layer_top(), MARK_SIZE);
    lv_obj_set_pos(s_mark_silent, MARK_X, s_mark_y);
    s_mark_still = ui_glyph_create(lv_layer_top(), MARK_SIZE);
    s_still_x = MARK_X + MARK_SIZE + MARK_GAP;
    lv_obj_set_pos(s_mark_still, s_still_x, s_mark_y);

    s_mic = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_mic);
    lv_obj_remove_flag(s_mic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_mic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_mic, MIC_DOT, MIC_DOT);
    lv_obj_set_style_radius(s_mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_mic, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_mic, lv_color_hex(0xe74c3c), 0);
    lv_obj_align(s_mic, LV_ALIGN_TOP_MID, 0, MIC_Y);
    lv_obj_add_flag(s_mic, LV_OBJ_FLAG_HIDDEN);

    if (display) {
        lv_display_add_event_cb(display, render_event_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(display, render_event_cb, LV_EVENT_RENDER_READY, NULL);
    }
}

void ui_chrome_refresh_locked(void)
{
    if (!s_mic) {
        return;
    }
    /*
     * Die Taschenlampe braucht den ganzen Schirm weiss, der Nachtlade-Modus
     * zeigt nur die Ziffern -- dort keine Zeichen. Der Mikrofon-Punkt steht
     * immer, wenn das Mikrofon offen ist: Das zu wissen ist wichtiger als
     * ein ruhiger Schirm.
     */
    const bool quiet_ok = !ui_torch_on() && !ui_status_night_mode();
    if (s_silent && quiet_ok) {
        ui_glyph_set(s_mark_silent, UI_GLYPH_SILENT, true, 0x9aa3ad);
    } else {
        ui_glyph_set(s_mark_silent, UI_GLYPH_NONE, false, 0);
    }
    if (s_still && quiet_ok) {
        ui_glyph_set(s_mark_still, UI_GLYPH_STILL, true, 0x9aa3ad);
    } else {
        ui_glyph_set(s_mark_still, UI_GLYPH_NONE, false, 0);
    }
    /* Ohne Lautlos rueckt "Dunkel & still" an die erste Stelle. */
    const int32_t still_x = s_silent ? MARK_X + MARK_SIZE + MARK_GAP : MARK_X;
    if (still_x != s_still_x) {
        s_still_x = still_x;
        lv_obj_set_pos(s_mark_still, still_x, s_mark_y);
    }
    if (s_mic_open == !lv_obj_has_flag(s_mic, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (s_mic_open) {
        lv_obj_remove_flag(s_mic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_mic);
    } else {
        lv_obj_add_flag(s_mic, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif /* CONFIG_VOICE_BOARD_WATCH206 */

void ui_status_set_quiet_marks(bool silent, bool still)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    s_silent = silent;
    s_still = still;
    ui_chrome_refresh_locked();
    ui_status_unlock();
#else
    (void)silent;
    (void)still;
#endif
}

void ui_status_set_mic_open(bool open)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    s_mic_open = open;
    ui_chrome_refresh_locked();
    ui_status_unlock();
#else
    (void)open;
#endif
}

bool ui_status_render_stats(ui_render_stats_t *out)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (!out) {
        return false;
    }
    ui_status_lock();
    const bool valid = s_stats_valid;
    *out = s_stats;
    ui_status_unlock();
    return valid;
#else
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return false;
#endif
}
