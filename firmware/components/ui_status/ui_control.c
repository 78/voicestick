#include "ui_control.h"

#include <stdio.h>

#include <string.h>

#include "sdkconfig.h"
#include "ui_status_internal.h"

/* Namen der Kacheln in `watch_settings.tiles` (docs/protocol.md), in Reihenfolge der Aufzaehlung. */
static const char *const TILE_NAMES[UI_CONTROL_COUNT] = {
    "brightness", "silent", "cinema", "saver", "torch", "battery",
};

/*
 * Welche Kacheln in welcher Reihenfolge stehen -- eingestellt in der App.
 * Ohne Einstellung: Lautlos und Helligkeit oben (die zwei, die man am
 * haeufigsten braucht), dann "Dunkel & still" (`cinema`), Sparen, Lampe, Akku.
 */
static uint8_t s_order[UI_CONTROL_COUNT] = {
    UI_CONTROL_SILENT, UI_CONTROL_BRIGHTNESS, UI_CONTROL_CINEMA,
    UI_CONTROL_SAVER, UI_CONTROL_TORCH, UI_CONTROL_BATTERY,
};
static uint8_t s_order_count = UI_CONTROL_COUNT;

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"

#define TILE_GAP 12
#define TILE_COLS 2
#define TILE_ROWS 3
/*
 * Feste Kachelgroesse fuer drei Reihen, auch wenn weniger Kacheln sichtbar
 * sind: Das Raster fuellt sich von oben, statt zwei Kacheln zu Riesen
 * aufzublasen. 183 x 134 px (Tabelle in ui_layout_watch206.h).
 */
#define TILE_TOP (HEADER_H + 10)
#define TILE_W ((CONTENT_W - (TILE_COLS - 1) * TILE_GAP) / TILE_COLS)
#define TILE_H ((CONTENT_H - TILE_TOP - (TILE_ROWS - 1) * TILE_GAP) / TILE_ROWS)

static lv_obj_t *s_root;
static lv_obj_t *s_tile[UI_CONTROL_COUNT];
static lv_obj_t *s_tile_title[UI_CONTROL_COUNT];
static lv_obj_t *s_tile_value[UI_CONTROL_COUNT];
static lv_obj_t *s_torch;
static bool s_visible;
static bool s_torch_on;

/*
 * "Dunkel & still" hiess bis 0.4.6 "Kino" -- ein Bild, das erst erklaert
 * werden musste. Der neue Name sagt, was die Kachel tut; die Kennung in
 * `watch_settings.tiles` bleibt `cinema` (Paket J, 0.5.0).
 */
static const char *const TITLES[UI_CONTROL_COUNT] = {
    "Helligkeit", "Lautlos", "Dunkel & still", "Sparen", "Lampe", "Akku",
};

static void create_locked(void)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, CONTENT_W, CONTENT_H);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        s_tile[i] = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_tile[i]);
        lv_obj_remove_flag(s_tile[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_tile[i], TILE_W, TILE_H);
        lv_obj_add_flag(s_tile[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(s_tile[i], 28, 0);
        lv_obj_set_style_bg_opa(s_tile[i], LV_OPA_COVER, 0);

        s_tile_title[i] = lv_label_create(s_tile[i]);
        lv_obj_set_style_text_font(s_tile_title[i], UI_FONT_SMALL, 0);
        lv_label_set_text(s_tile_title[i], TITLES[i]);
        lv_obj_align(s_tile_title[i], LV_ALIGN_TOP_LEFT, 18, 16);

        s_tile_value[i] = lv_label_create(s_tile[i]);
        lv_obj_set_style_text_font(s_tile_value[i], &todoteck_watch_font_26, 0);
        lv_obj_align(s_tile_value[i], LV_ALIGN_BOTTOM_LEFT, 18, -16);
    }

    s_torch = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_torch);
    /* Ueber den Rand des Inhaltsbereichs hinaus: der ganze Schirm leuchtet. */
    lv_obj_set_size(s_torch, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_torch, LV_ALIGN_TOP_LEFT, -UI_SCREEN_PAD, -UI_SCREEN_PAD);
    lv_obj_set_style_bg_opa(s_torch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_torch, lv_color_hex(0xffffff), 0);
    lv_obj_add_flag(s_torch, LV_OBJ_FLAG_HIDDEN);
}

/* Kacheln in der eingestellten Reihenfolge ins Raster, die uebrigen weg. */
static void layout_locked(void)
{
    if (!s_root) {
        return;
    }
    bool shown[UI_CONTROL_COUNT] = { false };
    for (int pos = 0; pos < s_order_count; pos++) {
        const int tile = s_order[pos];
        shown[tile] = true;
        lv_obj_align(s_tile[tile], LV_ALIGN_TOP_LEFT, (pos % TILE_COLS) * (TILE_W + TILE_GAP),
                     TILE_TOP + (pos / TILE_COLS) * (TILE_H + TILE_GAP));
        lv_obj_remove_flag(s_tile[tile], LV_OBJ_FLAG_HIDDEN);
    }
    for (int tile = 0; tile < UI_CONTROL_COUNT; tile++) {
        if (!shown[tile]) {
            lv_obj_add_flag(s_tile[tile], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void tile_style(int i, bool on, const char *value)
{
    lv_obj_set_style_bg_color(s_tile[i], on ? lv_color_hex(0x2ecc71) : lv_color_hex(0x1b2430), 0);
    const lv_color_t text = on ? lv_color_hex(0x000000) : lv_color_hex(0xf2f2f2);
    lv_obj_set_style_text_color(s_tile_title[i], text, 0);
    lv_obj_set_style_text_color(s_tile_value[i], text, 0);
    lv_label_set_text(s_tile_value[i], value);
}

static void render_locked(const ui_control_state_t *st)
{
    if (!s_root || !st) {
        return;
    }
    tile_style(UI_CONTROL_BRIGHTNESS, false, st->brightness ? st->brightness : "");
    tile_style(UI_CONTROL_SILENT, st->silent, st->silent ? "An" : "Aus");
    tile_style(UI_CONTROL_CINEMA, st->cinema, st->cinema ? "An" : "Aus");
    tile_style(UI_CONTROL_SAVER, st->saver, st->saver ? "An" : "Aus");
    tile_style(UI_CONTROL_TORCH, false, "Tippen");
    /* "24 % · noch 9 h" -- die Restzeit kommt fertig aus main.c; rot erst ab 10 %. */
    char battery[40];
    const bool remaining = st->remaining && st->remaining[0];
    if (st->battery_percent < 0) {
        snprintf(battery, sizeof(battery), "--%%");
    } else {
        snprintf(battery, sizeof(battery), "%d %%%s%s%.14s", st->battery_percent % 1000,
                 st->charging ? " +" : "", remaining ? " · " : "", remaining ? st->remaining : "");
    }
    tile_style(UI_CONTROL_BATTERY, false, battery);
    if (st->battery_percent >= 0 && st->battery_percent <= 10 && !st->charging) {
        lv_obj_set_style_text_color(s_tile_value[UI_CONTROL_BATTERY], lv_color_hex(0xe74c3c), 0);
    }
    lv_obj_set_style_text_font(s_tile_value[UI_CONTROL_BATTERY],
                               remaining ? &todoteck_watch_font_20 : &todoteck_watch_font_26, 0);
}
#endif

void ui_control_show(const ui_control_state_t *state)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    create_locked();
    layout_locked();
    render_locked(state);
    s_visible = true;
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    ui_status_header_refresh_locked();
    ui_status_unlock();
#else
    (void)state;
#endif
}

void ui_control_update(const ui_control_state_t *state)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    if (s_visible) {
        render_locked(state);
    }
    ui_status_unlock();
#else
    (void)state;
#endif
}

void ui_control_hide(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    s_visible = false;
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_header_refresh_locked();
    ui_status_unlock();
#endif
}

bool ui_control_visible(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_visible;
#else
    return false;
#endif
}

int ui_control_tile_at(int16_t x, int16_t y)
{
#if CONFIG_VOICE_BOARD_WATCH206
    /*
     * Mit Toleranz (UI_HIT_TOLERANCE) ueberlappen sich die Flaechen im
     * Zwischenraum; es zaehlt die Kachel, deren Mitte naeher liegt.
     */
    int hit = -1;
    int32_t best = 0;
    ui_status_lock();
    for (int i = 0; s_visible && i < UI_CONTROL_COUNT; i++) {
        if (lv_obj_has_flag(s_tile[i], LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(s_tile[i], &area);
        if (x < area.x1 - UI_HIT_TOLERANCE || x > area.x2 + UI_HIT_TOLERANCE ||
            y < area.y1 - UI_HIT_TOLERANCE || y > area.y2 + UI_HIT_TOLERANCE) {
            continue;
        }
        const int32_t dx = x - (area.x1 + area.x2) / 2;
        const int32_t dy = y - (area.y1 + area.y2) / 2;
        const int32_t distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (hit < 0 || distance < best) {
            hit = i;
            best = distance;
        }
    }
    ui_status_unlock();
    return hit;
#else
    (void)x;
    (void)y;
    return -1;
#endif
}

void ui_torch_set(bool on)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    create_locked();
    s_torch_on = on;
    if (on) {
        lv_obj_remove_flag(s_torch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_torch);
    } else {
        lv_obj_add_flag(s_torch, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_header_refresh_locked();
    ui_status_unlock();
#else
    (void)on;
#endif
}

bool ui_torch_on(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_torch_on;
#else
    return false;
#endif
}

int ui_control_tile_from_name(const char *name)
{
    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        if (name && strcmp(name, TILE_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

void ui_control_set_tiles(const uint8_t *tiles, uint8_t count)
{
    uint8_t order[UI_CONTROL_COUNT];
    uint8_t n = 0;
    bool seen[UI_CONTROL_COUNT] = { false };
    for (uint8_t i = 0; tiles && i < count && n < UI_CONTROL_COUNT; i++) {
        if (tiles[i] < UI_CONTROL_COUNT && !seen[tiles[i]]) {
            seen[tiles[i]] = true;
            order[n++] = tiles[i];
        }
    }
    if (n == 0) {
        /* Mindestens eine Kachel -- sonst bliebe die Voreinstellung stehen. */
        return;
    }
    ui_status_lock();
    memcpy(s_order, order, n);
    s_order_count = n;
#if CONFIG_VOICE_BOARD_WATCH206
    if (s_visible) {
        layout_locked();
    }
#endif
    ui_status_unlock();
}
