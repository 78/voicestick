/*
 * Auswahl fuer `input_select` (0.4.5): Tippen auf eine Zeile mit Flag `s`
 * auf der Karte Zuhause oeffnet sie (main.c, handle_touch). Was die Szene
 * kann und wann sie schliesst, steht in include/ui_options.h; das Muster ist
 * die Layout-Auswahl (ui_face_picker.c): eigene Wurzel ueber allem, alle
 * Gesten gehen an sie, ein einmaliger LVGL-Zeitgeber bricht ab.
 *
 * Ein Zeitgeber fuer drei Fristen, je nach Zustand: drei Sekunden auf die
 * Antwort der Bruecke ("Laedt ..."), anderthalb Sekunden "Keine Antwort",
 * zehn Sekunden ohne Eingabe bei stehenden Optionen. Er laeuft im
 * LVGL-Strang mit gehaltener Sperre und schreibt nichts in den NVS.
 */
#include "ui_options.h"

#include <string.h>

#include "sdkconfig.h"
#include "ui_status_internal.h"
#include "voice_board.h"

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"

#define OPT_IDLE_MS 10000
#define OPT_ANSWER_MS 3000
#define OPT_NO_ANSWER_MS 1500
/* Bis hierher zaehlt ein Wischen nach rechts als "vom linken Rand": zurueck (wie main.c). */
#define OPT_BACK_EDGE_PX 30

/* Titel unter der Kopfzeile (dort steht die kleine Uhrzeit), Liste darunter. */
#define OPT_TITLE_Y (HEADER_H + 6)
#define OPT_LIST_TOP (HEADER_H + 54)
#define OPT_LIST_BOTTOM (CONTENT_H - 16)
#define OPT_ROW_GAP 8
/* Trefferflaeche >= 48 px (ui_layout_watch206.h); 56 wie die Chips. */
#define OPT_ROW_MIN_H 56
#define OPT_ROW_PAD 14
#define OPT_MARK 14

typedef enum {
    OPT_HIDDEN,
    OPT_LOADING,
    OPT_NO_ANSWER,
    OPT_SHOWN,
} opt_state_t;

/* Dieselben Stufen wie die Kartenzeilen: klein 20, normal 23, gross 26. */
static const lv_font_t *const LEVEL_FONT[3] = {
    &todoteck_watch_font_20, &todoteck_watch_font_23, &todoteck_watch_font_26,
};

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_message;
static lv_obj_t *s_list;
static lv_obj_t *s_row[UI_OPTIONS_MAX];
static lv_obj_t *s_row_text[UI_OPTIONS_MAX];
static lv_obj_t *s_row_mark[UI_OPTIONS_MAX];
static lv_timer_t *s_timer;
static opt_state_t s_state;

/* Die Zeile, fuer die die Auswahl offen ist. */
static ui_card_kind_t s_kind;
static uint16_t s_rev;
static uint8_t s_index;
static char s_title_text[UI_CARD_TEXT_LEN];

static char s_items[UI_OPTIONS_MAX][UI_OPTIONS_TEXT_LEN];
static uint8_t s_count;
static int s_current;
static int32_t s_pitch;
static int32_t s_content_h;

static int32_t list_h(void)
{
    return OPT_LIST_BOTTOM - OPT_LIST_TOP;
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

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, &todoteck_watch_font_26, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x2ecc71), 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_title, CONTENT_W - 2 * UI_HEADER_INSET_X,
                    lv_font_get_line_height(&todoteck_watch_font_26));
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, OPT_TITLE_Y);
    lv_label_set_text(s_title, "");

    s_message = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_message, UI_FONT_QUESTION, 0);
    lv_obj_set_style_text_color(s_message, lv_color_hex(0x9aa3ad), 0);
    lv_obj_align(s_message, LV_ALIGN_TOP_MID, 0, OPT_LIST_TOP + list_h() / 3);
    lv_label_set_text(s_message, "");

    /* Scrollender Ausschnitt wie bei den Karten: senkrecht, Rastung an den Zeilen, ohne Balken. */
    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, CONTENT_W, list_h());
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, OPT_LIST_TOP);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < UI_OPTIONS_MAX; i++) {
        s_row[i] = lv_obj_create(s_list);
        lv_obj_remove_style_all(s_row[i]);
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_set_style_radius(s_row[i], 16, 0);
        lv_obj_set_style_bg_opa(s_row[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_row[i], lv_color_hex(0x2ecc71), 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

        s_row_text[i] = lv_label_create(s_row[i]);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(s_row_text[i], "");

        /* Die aktive Option: gruener Punkt rechts (die Schriften haben keinen Haken). */
        s_row_mark[i] = lv_obj_create(s_row[i]);
        lv_obj_remove_style_all(s_row_mark[i]);
        lv_obj_set_size(s_row_mark[i], OPT_MARK, OPT_MARK);
        lv_obj_set_style_radius(s_row_mark[i], OPT_MARK / 2, 0);
        lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row_mark[i], lv_color_hex(0x2ecc71), 0);
        lv_obj_align(s_row_mark[i], LV_ALIGN_RIGHT_MID, -18, 0);
        lv_obj_add_flag(s_row_mark[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_obj(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_message_locked(const char *text)
{
    char clean[32];
    ui_status_copy_display_text(clean, text, sizeof(clean));
    lv_label_set_text(s_message, clean);
    lv_obj_align(s_message, LV_ALIGN_TOP_MID, 0, OPT_LIST_TOP + list_h() / 3);
}

static void render_rows_locked(void)
{
    const lv_font_t *font = LEVEL_FONT[ui_status_font_level() % 3];
    const int32_t line_h = lv_font_get_line_height(font);
    int32_t h = line_h + 2 * OPT_ROW_PAD;
    if (h < OPT_ROW_MIN_H) {
        h = OPT_ROW_MIN_H;
    }
    s_pitch = h + OPT_ROW_GAP;
    int32_t y = 0;
    for (int i = 0; i < UI_OPTIONS_MAX; i++) {
        if (i >= s_count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const bool current = i == s_current;
        lv_obj_set_size(s_row[i], CONTENT_W, h);
        lv_obj_align(s_row[i], LV_ALIGN_TOP_LEFT, 0, y);
        /* Aktiv: hellere Flaeche und gruener Rand, wie die markierte Kartenzeile. */
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(current ? 0x2a3b4f : 0x1b2430), 0);
        lv_obj_set_style_border_width(s_row[i], current ? 2 : 0, 0);
        lv_obj_set_style_text_font(s_row_text[i], font, 0);
        lv_obj_set_style_text_color(s_row_text[i],
                                    current ? lv_color_hex(0x2ecc71) : lv_color_hex(0xf2f2f2), 0);
        lv_obj_set_size(s_row_text[i], CONTENT_W - 16 - 2 * OPT_MARK - 24, line_h);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, 16, 0);
        lv_label_set_text(s_row_text[i], s_items[i]);
        show_obj(s_row_mark[i], current);
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        y += s_pitch;
    }
    s_content_h = y > OPT_ROW_GAP ? y - OPT_ROW_GAP : 0;
    lv_obj_update_layout(s_list);
}

static void hide_locked(void)
{
    s_state = OPT_HIDDEN;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void timer_cb(lv_timer_t *timer);

/* Den einen Zeitgeber neu stellen (alle drei Fristen sind einmalig). */
static void arm_timer_locked(uint32_t ms)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
    }
    s_timer = lv_timer_create(timer_cb, ms, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_timer = NULL;
    if (s_state == OPT_LOADING) {
        /* Keine Antwort der Bruecke: kurz sagen, dann zu. */
        s_state = OPT_NO_ANSWER;
        set_message_locked("Keine Antwort");
        arm_timer_locked(OPT_NO_ANSWER_MS);
        return;
    }
    hide_locked();
}

/* Einen Ausschnitt weiter (+1) oder zurueck (-1); `false`, wenn es dort nichts mehr gibt. */
static bool scroll_page_locked(int dir)
{
    const int32_t view = list_h();
    const int32_t max_y = s_content_h > view ? s_content_h - view : 0;
    const int32_t y = lv_obj_get_scroll_y(s_list);
    if ((dir > 0 && y >= max_y) || (dir < 0 && y <= 0)) {
        return false;
    }
    /* Ganze Zeilen weiter, damit die Rastung an einer Oberkante sitzt. */
    int32_t step = s_pitch > 0 ? (view / s_pitch) * s_pitch : view;
    if (step <= 0) {
        step = s_pitch > 0 ? s_pitch : view;
    }
    int32_t target = y + (dir > 0 ? step : -step);
    if (target > max_y) {
        target = max_y;
    }
    if (target < 0) {
        target = 0;
    }
    lv_obj_scroll_to_y(s_list, target, LV_ANIM_ON);
    return true;
}

/* Die Option unter dem Finger; -1 daneben. Zeilen ausserhalb des Ausschnitts zaehlen nicht. */
static int option_at_locked(int16_t x, int16_t y)
{
    lv_area_t list_area;
    lv_obj_get_coords(s_list, &list_area);
    if (y < list_area.y1 || y > list_area.y2) {
        return -1;
    }
    int best = -1;
    int32_t best_distance = 0;
    for (int i = 0; i < s_count; i++) {
        lv_area_t area;
        lv_obj_get_coords(s_row[i], &area);
        if (x < area.x1 - UI_HIT_TOLERANCE || x > area.x2 + UI_HIT_TOLERANCE ||
            y < area.y1 - UI_HIT_TOLERANCE || y > area.y2 + UI_HIT_TOLERANCE) {
            continue;
        }
        const int32_t cy = (area.y1 + area.y2) / 2;
        const int32_t distance = y > cy ? y - cy : cy - y;
        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}
#endif /* CONFIG_VOICE_BOARD_WATCH206 */

void ui_options_open(ui_card_kind_t kind, uint16_t rev, uint8_t index, const char *title)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    create_locked();
    s_kind = kind;
    s_rev = rev;
    s_index = index;
    ui_status_copy_display_text(s_title_text, title ? title : "", sizeof(s_title_text));
    s_count = 0;
    s_current = -1;
    s_content_h = 0;
    lv_label_set_text(s_title, s_title_text);
    set_message_locked("Lädt …");
    lv_obj_remove_flag(s_message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    s_state = OPT_LOADING;
    arm_timer_locked(OPT_ANSWER_MS);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    ui_status_unlock();
#else
    (void)kind;
    (void)rev;
    (void)index;
    (void)title;
#endif
}

ui_options_fill_t ui_options_fill(uint16_t rev, uint8_t index, int current,
                                  const char *const *items, uint8_t count)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_options_fill_t result = UI_OPTIONS_FILL_IGNORED;
    ui_status_lock();
    if (s_state != OPT_HIDDEN && s_rev == rev && s_index == index) {
        if (count == 0 || !items) {
            /* Die Karte ist inzwischen neu: zu, die neue Karte kommt von der Bruecke. */
            hide_locked();
            result = UI_OPTIONS_FILL_EMPTY;
        } else {
            if (count > UI_OPTIONS_MAX) {
                count = UI_OPTIONS_MAX;
            }
            for (uint8_t i = 0; i < count; i++) {
                ui_status_copy_display_text(s_items[i], items[i] ? items[i] : "",
                                            sizeof(s_items[i]));
            }
            s_count = count;
            s_current = current >= 0 && current < count ? current : -1;
            s_state = OPT_SHOWN;
            lv_obj_add_flag(s_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
            render_rows_locked();
            lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
            /* Die aktive Option zu sehen, falls sie unter dem ersten Ausschnitt liegt. */
            if (s_current >= 0 && s_pitch > 0) {
                const int32_t view = list_h();
                const int32_t max_y = s_content_h > view ? s_content_h - view : 0;
                int32_t want = (int32_t)s_current * s_pitch;
                if (want + s_pitch > view) {
                    want = want > max_y ? max_y : want;
                    lv_obj_scroll_to_y(s_list, want, LV_ANIM_OFF);
                }
            }
            arm_timer_locked(OPT_IDLE_MS);
            result = UI_OPTIONS_FILL_SHOWN;
        }
    }
    ui_status_unlock();
    return result;
#else
    (void)rev;
    (void)index;
    (void)current;
    (void)items;
    (void)count;
    return UI_OPTIONS_FILL_IGNORED;
#endif
}

void ui_options_hide(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    hide_locked();
    ui_status_unlock();
#endif
}

bool ui_options_visible(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_state != OPT_HIDDEN;
#else
    return false;
#endif
}

ui_options_action_t ui_options_touch(int gesture, int16_t x, int16_t y,
                                     ui_options_choice_t *choice)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_options_action_t action = UI_OPTIONS_NONE;
    ui_status_lock();
    if (s_state == OPT_HIDDEN) {
        ui_status_unlock();
        return UI_OPTIONS_CLOSED;
    }
    const bool shown = s_state == OPT_SHOWN;
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_TAP:
    case VOICE_BOARD_LONG_PRESS: {
        if (!shown) {
            /* Noch keine Optionen: Tippen tut nichts, die Frist laeuft weiter. */
            break;
        }
        const int option = option_at_locked(x, y);
        if (option < 0) {
            arm_timer_locked(OPT_IDLE_MS);
            break;
        }
        if (option == s_current) {
            /* Die aktive Option: nichts zu senden, nur zu. */
            hide_locked();
            action = UI_OPTIONS_CLOSED;
            break;
        }
        if (choice) {
            choice->kind = s_kind;
            choice->rev = s_rev;
            choice->index = s_index;
            choice->choice = (uint8_t)option;
            strlcpy(choice->title, s_title_text, sizeof(choice->title));
            strlcpy(choice->option, s_items[option], sizeof(choice->option));
        }
        hide_locked();
        action = UI_OPTIONS_CHOSEN;
        break;
    }
    case VOICE_BOARD_SWIPE_UP:
        if (shown) {
            (void)scroll_page_locked(1);
            arm_timer_locked(OPT_IDLE_MS);
        }
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        /* Wie bei den Karten: erst zurueckscrollen, am Anfang eine Ebene zurueck. */
        if (shown && scroll_page_locked(-1)) {
            arm_timer_locked(OPT_IDLE_MS);
            break;
        }
        hide_locked();
        action = UI_OPTIONS_CLOSED;
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        if (x < OPT_BACK_EDGE_PX) {
            hide_locked();
            action = UI_OPTIONS_CLOSED;
            break;
        }
        if (shown) {
            arm_timer_locked(OPT_IDLE_MS);
        }
        break;
    default:
        if (shown) {
            arm_timer_locked(OPT_IDLE_MS);
        }
        break;
    }
    ui_status_unlock();
    return action;
#else
    (void)gesture;
    (void)x;
    (void)y;
    (void)choice;
    return UI_OPTIONS_NONE;
#endif
}
