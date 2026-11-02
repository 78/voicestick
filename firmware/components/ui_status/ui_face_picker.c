/*
 * Auswahl des Zifferblatt-Layouts (0.4.4, Paket I): langer Druck auf das
 * Zifferblatt (main.c, handle_touch) oeffnet sie.
 *
 * Eine grosse Vorschau statt drei kleiner nebeneinander: Auf 378 px Breite
 * waeren drei Vorschauen je gut 110 px breit, und darin ist "Info" von
 * "Gross" kaum zu unterscheiden. Links/rechts wischen blaettert, die Punkte
 * darunter sagen, wo man steht, der Name steht unter der Vorschau. Tippen
 * uebernimmt (die ganze Flaeche -- am Handgelenk trifft man sonst daneben),
 * ausser auf das Zurueck "‹" links oben (0.5.0), das abbricht.
 *
 * Gesten nach der einen Regel (Paket J, 0.5.0): rechts wischen bricht ab --
 * ueberall, bis 0.4.6 nur vom linken Rand, sonst blaetterte es zurueck.
 * Geblaettert wird mit links (naechstes, im Kreis) und mit oben/unten
 * (naechstes/voriges) -- "scrollen" durch die drei Layouts; nach unten brach
 * bis 0.4.6 ab. Zehn Sekunden ohne Eingabe brechen ebenfalls ab.
 *
 * Die Vorschau zeigt die echte Uhrzeit und das echte Datum, aber die Zeilen
 * nur als Balken: Es geht um die Anordnung, nicht um den Inhalt.
 */
#include <string.h>

#include "sdkconfig.h"
#include "ui_status.h"
#include "ui_status_internal.h"
#include "voice_board.h"

/*
 * Namen in `watch_settings.face` und `face_layout` (docs/protocol.md,
 * doku-vertrag:uhr-layouts), in der Reihenfolge von ui_face_layout_t.
 */
static const char *const FACE_LAYOUT_NAMES[UI_FACE_COUNT] = { "info", "minimal", "large" };

int ui_face_layout_from_name(const char *name)
{
    for (int i = 0; i < UI_FACE_COUNT; i++) {
        if (name && strcmp(name, FACE_LAYOUT_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *ui_face_layout_name(ui_face_layout_t layout)
{
    return (int)layout >= 0 && layout < UI_FACE_COUNT ? FACE_LAYOUT_NAMES[layout] : "info";
}

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_chrome.h"
#include "ui_layout_watch206.h"
#include "ui_status_icons.h"

/* Was unter der Vorschau steht. */
static const char *const FACE_LAYOUT_TITLES[UI_FACE_COUNT] = { "Info", "Minimal", "Groß" };

#define PICKER_TIMEOUT_MS 10000

#define PREVIEW_W 240
#define PREVIEW_H 290
#define PREVIEW_Y 46
#define PREVIEW_BARS 3
#define PREVIEW_TECKI 48
#define NAME_Y (PREVIEW_Y + PREVIEW_H + 12)
#define DOTS_Y (NAME_Y + 36 + 12)
#define HINT_Y (DOTS_Y + UI_DOTS_SIZE + 12)

static lv_obj_t *s_root;
static lv_obj_t *s_back;
static lv_obj_t *s_title;
static lv_obj_t *s_hint;
static lv_obj_t *s_frame;
static lv_obj_t *s_clock;
static lv_obj_t *s_large[2];
static lv_obj_t *s_bar[PREVIEW_BARS];
static lv_obj_t *s_date;
static lv_obj_t *s_dot;
static lv_obj_t *s_name;
static ui_status_icons_t s_tecki;
static ui_dots_t s_dots;
static lv_timer_t *s_timeout;
static bool s_visible;
static ui_face_layout_t s_page;

static lv_obj_t *create_rect(lv_obj_t *parent, int32_t w, int32_t h, uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    return o;
}

static lv_obj_t *create_text(lv_obj_t *parent, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, "");
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(colour), 0);
    return o;
}

static void create_locked(void)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_root, CONTENT_W, CONTENT_H);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_back = ui_back_create(s_root);
    s_title = create_text(s_root, UI_FONT_SMALL, 0x9aa3ad);
    lv_label_set_text(s_title, "Zifferblatt");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, UI_HEADER_LABEL_Y);

    /* Der Rahmen steht fuer das Glas der Uhr: abgerundet, schwarz, grau umrandet. */
    s_frame = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_frame);
    lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_frame, PREVIEW_W, PREVIEW_H);
    lv_obj_set_style_radius(s_frame, 36, 0);
    lv_obj_set_style_bg_opa(s_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_frame, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_frame, 3, 0);
    lv_obj_set_style_border_color(s_frame, lv_color_hex(0x3a4656), 0);
    lv_obj_align(s_frame, LV_ALIGN_TOP_MID, 0, PREVIEW_Y);

    s_clock = create_text(s_frame, UI_FONT_CLOCK_SMALL, 0xf2f2f2);
    for (int i = 0; i < 2; i++) {
        s_large[i] = create_text(s_frame, UI_FONT_CLOCK_FACE, 0xf2f2f2);
    }
    for (int i = 0; i < PREVIEW_BARS; i++) {
        s_bar[i] = create_rect(s_frame, 160, 12, i == 0 ? 0x5ec4ff : 0x3a4656);
    }
    s_date = create_text(s_frame, &todoteck_watch_font_20, 0x9aa3ad);
    s_dot = create_rect(s_frame, 10, 10, 0x2ecc71);
    ui_status_icons_create(&s_tecki, s_frame);
    ui_status_icons_show(&s_tecki, false);

    s_name = create_text(s_root, UI_FONT_STATUS, 0xf2f2f2);
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, NAME_Y);

    ui_dots_create(&s_dots, s_root);
    lv_obj_align(s_dots.box, LV_ALIGN_TOP_MID, 0, DOTS_Y);

    s_hint = create_text(s_root, UI_FONT_SMALL, 0x9aa3ad);
    lv_label_set_text(s_hint, "Tippen übernimmt");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, HINT_Y);
}

static void show_obj(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Die Vorschau der Seite `s_page` -- Anordnung wie auf dem Zifferblatt, verkleinert. */
static void render_locked(void)
{
    char clock[8];
    char date[24];
    ui_status_format_clock(clock, sizeof(clock));
    ui_status_format_date(date, sizeof(date));

    const bool info = s_page == UI_FACE_INFO;
    const bool minimal = s_page == UI_FACE_MINIMAL;
    const bool large = s_page == UI_FACE_LARGE;

    lv_label_set_text(s_clock, clock);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, info ? 34 : 92);
    show_obj(s_clock, !large);

    /* Gross: "HH" und "MM" untereinander in der 120er -- so gross wirkt die 180er auf dem Schirm. */
    const bool full = strlen(clock) == 5;
    for (int i = 0; i < 2; i++) {
        const size_t at = i == 0 ? 0 : 3;
        const char part[3] = { full ? clock[at] : '-', full ? clock[at + 1] : '-', '\0' };
        lv_label_set_text(s_large[i], part);
        lv_obj_align(s_large[i], LV_ALIGN_TOP_MID, 0, i == 0 ? 20 : 108);
        show_obj(s_large[i], large);
    }

    /* Info: drei Zeilen (der Termin blau); Gross: eine. */
    static const int32_t BAR_W[PREVIEW_BARS] = { 170, 140, 120 };
    for (int i = 0; i < PREVIEW_BARS; i++) {
        lv_obj_set_width(s_bar[i], BAR_W[i]);
        lv_obj_align(s_bar[i], LV_ALIGN_TOP_MID, 0, large ? 222 : 104 + i * 26);
        show_obj(s_bar[i], info || (large && i == 0));
    }

    lv_label_set_text(s_date, date[0] ? date : "So 1.11.");
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 152);
    show_obj(s_date, minimal);
    lv_obj_align(s_dot, LV_ALIGN_TOP_MID, 0, 194);
    show_obj(s_dot, minimal);

    if (info) {
        ui_status_icons_set_box(&s_tecki, PREVIEW_TECKI, (PREVIEW_W - PREVIEW_TECKI) / 2,
                                PREVIEW_H - PREVIEW_TECKI - 20);
        ui_status_icons_apply(&s_tecki, UI_STATUS_ICON_IDLE);
    }
    ui_status_icons_show(&s_tecki, info);

    /* Titel, Name und Hinweis in der Typo-Skala (die Vorschau selbst ist ein Bild, keine Schrift). */
    lv_obj_set_style_text_font(s_title, UI_TYPO(UI_TYPO_META), 0);
    lv_obj_set_style_text_font(s_hint, UI_TYPO(UI_TYPO_META), 0);
    lv_obj_set_style_text_font(s_name, UI_TYPO(UI_TYPO_TITLE), 0);
    lv_label_set_text(s_name, FACE_LAYOUT_TITLES[s_page]);
    ui_dots_set(&s_dots, UI_FACE_COUNT, (uint8_t)s_page);
}

static void hide_locked(void)
{
    s_visible = false;
    if (s_timeout) {
        lv_timer_delete(s_timeout);
        s_timeout = NULL;
    }
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Zehn Sekunden ohne Eingabe: abbrechen. Laeuft im LVGL-Strang, die Sperre ist gehalten. */
static void timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_timeout = NULL;
    hide_locked();
}

static void restart_timeout_locked(void)
{
    if (s_timeout) {
        lv_timer_reset(s_timeout);
        return;
    }
    s_timeout = lv_timer_create(timeout_cb, PICKER_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_timeout, 1);
}
#endif /* CONFIG_VOICE_BOARD_WATCH206 */

void ui_face_picker_show(ui_face_layout_t current)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    create_locked();
    s_page = (int)current >= 0 && current < UI_FACE_COUNT ? current : UI_FACE_INFO;
    render_locked();
    s_visible = true;
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    restart_timeout_locked();
    ui_status_unlock();
#else
    (void)current;
#endif
}

void ui_face_picker_hide(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    hide_locked();
    ui_status_unlock();
#endif
}

bool ui_face_picker_visible(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_visible;
#else
    return false;
#endif
}

ui_face_picker_action_t ui_face_picker_touch(int gesture, int16_t x, int16_t y,
                                             ui_face_layout_t *chosen)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_face_picker_action_t action = UI_FACE_PICKER_NONE;
    ui_status_lock();
    if (!s_visible) {
        ui_status_unlock();
        return UI_FACE_PICKER_CANCELLED;
    }
    switch (gesture) {
    case VOICE_BOARD_TAP:
    case VOICE_BOARD_LONG_PRESS:
        if (ui_back_hit(s_back, x, y)) {
            hide_locked();
            action = UI_FACE_PICKER_CANCELLED;
            break;
        }
        if (chosen) {
            *chosen = s_page;
        }
        hide_locked();
        action = UI_FACE_PICKER_CHOSEN;
        break;
    case VOICE_BOARD_SWIPE_LEFT:
    case VOICE_BOARD_SWIPE_UP:
        s_page = (ui_face_layout_t)((s_page + 1) % UI_FACE_COUNT);
        render_locked();
        restart_timeout_locked();
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        s_page = (ui_face_layout_t)((s_page + UI_FACE_COUNT - 1) % UI_FACE_COUNT);
        render_locked();
        restart_timeout_locked();
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        /* Rechts ist zurueck -- ueberall (0.5.0). */
        hide_locked();
        action = UI_FACE_PICKER_CANCELLED;
        break;
    default:
        restart_timeout_locked();
        break;
    }
    ui_status_unlock();
    return action;
#else
    (void)gesture;
    (void)x;
    (void)y;
    (void)chosen;
    return UI_FACE_PICKER_NONE;
#endif
}
