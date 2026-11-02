/*
 * Blaetter der Uhr (Paket J, Teil 2, 0.5.0): Frage, Details, PIN, Diagnose.
 * Was jedes ist und wofuer, steht in include/ui_sheets.h. Nur die Uhr --
 * CMakeLists.txt baut die Datei nur dort.
 *
 * Alle vier liegen als eigene Wurzel auf ui_status_screen() und kommen beim
 * Zeigen nach vorn: ueber Karten, Zifferblatt und Verlauf. Sie teilen das
 * Zurueck "‹" links oben (ui_back_create) und die Typo-Skala; der Titel
 * steht unter der Kopfzeile ueber die volle Breite, weil Aufgaben- und
 * Erinnerungstitel neben dem Pfeil keinen Platz haetten.
 */
#include "ui_sheets.h"

#include <stdio.h>
#include <string.h>

#include "ui_chrome.h"
#include "ui_layout_watch206.h"
#include "ui_status_internal.h"
#include "voice_board.h"

/* Gemeinsame Masse (im Inhalt, Rand schon abgezogen). */
#define SHEET_TITLE_Y (UI_BACK_Y + UI_BACK_H + 4)
#define SHEET_SIDE 16
#define SHEET_W (CONTENT_W - 2 * SHEET_SIDE)
/* Knoepfe: >= 48 px hoch, schmaler als der Schirm -- unten runden die Ecken. */
#define BTN_H 52
#define BTN_GAP 8
#define BTN_W (CONTENT_W - 48)
#define BTN_BOTTOM (CONTENT_H - 20)
/* Unten frei fuer die Seitenpunkte bzw. die Eckrundung. */
#define SCROLL_BOTTOM (CONTENT_H - 16)

static void show_obj(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_root(void)
{
    lv_obj_t *root = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, CONTENT_W, CONTENT_H);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, "");
    return label;
}

/* Senkrecht scrollender Ausschnitt ohne Leiste und ohne Schwung (Gesten kommen erst beim Abheben). */
static lv_obj_t *make_scroller(lv_obj_t *parent, int32_t y, int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, CONTENT_W, h);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    return box;
}

/* Einen Ausschnitt (abzueglich einer Zeile Ueberlapp) weiter oder zurueck; `false` am Ende. */
static bool scroll_box_locked(lv_obj_t *box, int32_t content_h, int dir)
{
    const int32_t view = lv_obj_get_height(box);
    const int32_t max_y = content_h > view ? content_h - view : 0;
    const int32_t y = lv_obj_get_scroll_y(box);
    if ((dir > 0 && y >= max_y) || (dir < 0 && y <= 0)) {
        return false;
    }
    const int32_t step = view > 60 ? view - 40 : view;
    int32_t target = y + (dir > 0 ? step : -step);
    if (target > max_y) {
        target = max_y;
    }
    if (target < 0) {
        target = 0;
    }
    lv_obj_scroll_to_y(box, target, LV_ANIM_ON);
    return true;
}

static bool hit_obj(lv_obj_t *obj, int16_t x, int16_t y)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return x >= area.x1 - UI_HIT_TOLERANCE && x <= area.x2 + UI_HIT_TOLERANCE &&
           y >= area.y1 - UI_HIT_TOLERANCE && y <= area.y2 + UI_HIT_TOLERANCE;
}

/* Hoehe eines Labels mit fester Breite nach dem Setzen des Texts. */
static int32_t label_height(lv_obj_t *label)
{
    lv_obj_update_layout(label);
    return lv_obj_get_height(label);
}

/* ── Frage ─────────────────────────────────────────────────────────────── */

/*
 * Titel (Titel-Stufe, hoechstens zwei Zeilen), Text darunter (Inhalt-Stufe,
 * so viele Zeilen, wie bis zu den Knoepfen passen) und die Knoepfe von unten
 * nach oben gestapelt: Der erste ist gruen gefuellt -- das, was man meistens
 * will --, die uebrigen dunkel. Untereinander statt nebeneinander, weil
 * "Heute Abend" und "+15 min" in der Inhalt-Stufe nebeneinander nicht passen.
 */
#define PROMPT_TITLE_LEN 96
#define PROMPT_BODY_LEN 160
#define PROMPT_BUTTON_LEN 24

static lv_obj_t *s_p_root;
static lv_obj_t *s_p_back;
static lv_obj_t *s_p_title;
static lv_obj_t *s_p_body;
static lv_obj_t *s_p_btn[UI_PROMPT_BUTTONS_MAX];
static lv_obj_t *s_p_btn_label[UI_PROMPT_BUTTONS_MAX];
static lv_timer_t *s_p_timer;
static ui_prompt_kind_t s_p_kind;
static uint8_t s_p_count;
static uint32_t s_p_idle_ms;

static void prompt_create_locked(void)
{
    if (s_p_root) {
        return;
    }
    s_p_root = make_root();
    s_p_back = ui_back_create(s_p_root);
    s_p_title = make_label(s_p_root, 0x2ecc71);
    lv_label_set_long_mode(s_p_title, LV_LABEL_LONG_DOT);
    s_p_body = make_label(s_p_root, 0xf2f2f2);
    lv_label_set_long_mode(s_p_body, LV_LABEL_LONG_DOT);
    for (int i = 0; i < UI_PROMPT_BUTTONS_MAX; i++) {
        s_p_btn[i] = lv_obj_create(s_p_root);
        lv_obj_remove_style_all(s_p_btn[i]);
        lv_obj_remove_flag(s_p_btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_p_btn[i], BTN_W, BTN_H);
        lv_obj_set_style_radius(s_p_btn[i], BTN_H / 2, 0);
        lv_obj_set_style_bg_opa(s_p_btn[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_p_btn[i], LV_OBJ_FLAG_HIDDEN);
        s_p_btn_label[i] = lv_label_create(s_p_btn[i]);
        lv_label_set_text(s_p_btn_label[i], "");
        lv_obj_center(s_p_btn_label[i]);
    }
}

static void prompt_hide_locked(void)
{
    s_p_kind = UI_PROMPT_NONE;
    if (s_p_timer) {
        lv_timer_delete(s_p_timer);
        s_p_timer = NULL;
    }
    if (s_p_root) {
        lv_obj_add_flag(s_p_root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void prompt_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_p_timer = NULL;
    prompt_hide_locked();
}

static void prompt_arm_locked(void)
{
    if (s_p_timer) {
        lv_timer_delete(s_p_timer);
        s_p_timer = NULL;
    }
    if (s_p_idle_ms > 0) {
        s_p_timer = lv_timer_create(prompt_timer_cb, s_p_idle_ms, NULL);
        lv_timer_set_repeat_count(s_p_timer, 1);
    }
}

void ui_prompt_show(ui_prompt_kind_t kind, const char *title, const char *body,
                    const char *const *buttons, uint8_t count, uint32_t idle_ms)
{
    static char title_text[PROMPT_TITLE_LEN]; /* unter der Sperre */
    static char body_text[PROMPT_BODY_LEN];
    if (count > UI_PROMPT_BUTTONS_MAX) {
        count = UI_PROMPT_BUTTONS_MAX;
    }
    ui_status_lock();
    prompt_create_locked();
    s_p_kind = kind;
    s_p_count = count;
    s_p_idle_ms = idle_ms;

    const lv_font_t *title_font = UI_TYPO(UI_TYPO_TITLE);
    const lv_font_t *body_font = UI_TYPO(UI_TYPO_BODY);
    ui_status_copy_display_text(title_text, title ? title : "", sizeof(title_text));
    lv_obj_set_style_text_font(s_p_title, title_font, 0);
    lv_obj_set_width(s_p_title, SHEET_W);
    lv_label_set_text(s_p_title, title_text);
    /* Hoechstens zwei Zeilen: LONG_DOT kuerzt erst mit fester Hoehe. */
    const int32_t title_line = lv_font_get_line_height(title_font);
    const bool title_two = lv_text_get_width(title_text, (uint32_t)strlen(title_text), title_font,
                                             0) > SHEET_W;
    const int32_t title_h = title_two ? 2 * title_line : title_line;
    lv_obj_set_height(s_p_title, title_h);
    lv_obj_align(s_p_title, LV_ALIGN_TOP_LEFT, SHEET_SIDE, SHEET_TITLE_Y);

    const int32_t buttons_top = BTN_BOTTOM - (count * BTN_H + (count > 0 ? count - 1 : 0) * BTN_GAP);
    for (int i = 0; i < UI_PROMPT_BUTTONS_MAX; i++) {
        if (i >= count) {
            lv_obj_add_flag(s_p_btn[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char label[PROMPT_BUTTON_LEN];
        ui_status_copy_display_text(label, buttons[i] ? buttons[i] : "", sizeof(label));
        const bool primary = i == 0;
        lv_obj_set_style_bg_color(s_p_btn[i], lv_color_hex(primary ? 0x2ecc71 : 0x1b2430), 0);
        lv_obj_set_style_text_font(s_p_btn_label[i], body_font, 0);
        lv_obj_set_style_text_color(s_p_btn_label[i], lv_color_hex(primary ? 0x000000 : 0xf2f2f2),
                                    0);
        lv_label_set_text(s_p_btn_label[i], label);
        lv_obj_center(s_p_btn_label[i]);
        lv_obj_align(s_p_btn[i], LV_ALIGN_TOP_MID, 0, buttons_top + i * (BTN_H + BTN_GAP));
        lv_obj_remove_flag(s_p_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    ui_status_copy_display_text(body_text, body ? body : "", sizeof(body_text));
    const int32_t body_y = SHEET_TITLE_Y + title_h + 10;
    const int32_t body_line = lv_font_get_line_height(body_font);
    int32_t room = buttons_top - 12 - body_y;
    room = room > body_line ? (room / body_line) * body_line : body_line;
    lv_obj_set_style_text_font(s_p_body, body_font, 0);
    lv_obj_set_width(s_p_body, SHEET_W);
    lv_label_set_text(s_p_body, body_text);
    lv_obj_set_height(s_p_body, LV_SIZE_CONTENT);
    if (label_height(s_p_body) > room) {
        lv_obj_set_height(s_p_body, room);
    }
    lv_obj_align(s_p_body, LV_ALIGN_TOP_LEFT, SHEET_SIDE, body_y);
    show_obj(s_p_body, body_text[0] != '\0');

    prompt_arm_locked();
    lv_obj_remove_flag(s_p_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_p_root);
    ui_status_unlock();
}

void ui_prompt_hide(void)
{
    ui_status_lock();
    prompt_hide_locked();
    ui_status_unlock();
}

bool ui_prompt_visible(void)
{
    return s_p_kind != UI_PROMPT_NONE;
}

ui_prompt_kind_t ui_prompt_kind(void)
{
    return s_p_kind;
}

int ui_prompt_touch(int gesture, int16_t x, int16_t y)
{
    int result = UI_PROMPT_TOUCH_NONE;
    ui_status_lock();
    if (s_p_kind == UI_PROMPT_NONE) {
        ui_status_unlock();
        return UI_PROMPT_TOUCH_CLOSED;
    }
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_TAP:
    case VOICE_BOARD_LONG_PRESS:
        if (ui_back_hit(s_p_back, x, y)) {
            result = UI_PROMPT_TOUCH_CLOSED;
            break;
        }
        for (int i = 0; i < s_p_count; i++) {
            if (hit_obj(s_p_btn[i], x, y)) {
                result = i;
                break;
            }
        }
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        result = UI_PROMPT_TOUCH_CLOSED;
        break;
    default:
        break;
    }
    if (result == UI_PROMPT_TOUCH_NONE) {
        prompt_arm_locked();
    } else {
        prompt_hide_locked();
    }
    ui_status_unlock();
    return result;
}

/* ── Details ───────────────────────────────────────────────────────────── */

/*
 * Eine Aufgabe zum Lesen: Titel, darunter "Projekt · Termin · Prio", die
 * Beschreibung (Klartext, hoechstens 400 Zeichen) und bis zu acht
 * Unteraufgaben -- erledigte grau und durchgestrichen. Alles in einem
 * scrollenden Ausschnitt; oben/unten wischen blaettert. Die Bruecke schickt
 * die Teile nacheinander, jeder zeichnet neu. Kommt binnen drei Sekunden
 * nichts, steht "Keine Antwort".
 */
#define DETAIL_TEXT_LEN 900
#define DETAIL_SUBTASKS 8
#define DETAIL_SUB_LEN 72
#define DETAIL_ANSWER_MS 3000
#define DETAIL_GAP 10

static lv_obj_t *s_d_root;
static lv_obj_t *s_d_back;
static lv_obj_t *s_d_box;
static lv_obj_t *s_d_title;
static lv_obj_t *s_d_meta;
static lv_obj_t *s_d_text;
static lv_obj_t *s_d_sub[DETAIL_SUBTASKS];
static lv_obj_t *s_d_message;
static lv_timer_t *s_d_timer;
static bool s_d_visible;
static bool s_d_loading;
static uint16_t s_d_rev;
static uint8_t s_d_index;
static int32_t s_d_content_h;

static struct {
    char title[PROMPT_TITLE_LEN + 32];
    char meta[96];
    char text[DETAIL_TEXT_LEN];
    char sub[DETAIL_SUBTASKS][DETAIL_SUB_LEN];
    bool sub_done[DETAIL_SUBTASKS];
    uint8_t sub_count;
} s_d;

static void detail_create_locked(void)
{
    if (s_d_root) {
        return;
    }
    s_d_root = make_root();
    s_d_back = ui_back_create(s_d_root);
    s_d_box = make_scroller(s_d_root, SHEET_TITLE_Y, SCROLL_BOTTOM - SHEET_TITLE_Y);
    s_d_title = make_label(s_d_box, 0x2ecc71);
    s_d_meta = make_label(s_d_box, 0x9aa3ad);
    s_d_text = make_label(s_d_box, 0xf2f2f2);
    for (int i = 0; i < DETAIL_SUBTASKS; i++) {
        s_d_sub[i] = make_label(s_d_box, 0xf2f2f2);
    }
    s_d_message = make_label(s_d_root, 0x9aa3ad);
    lv_label_set_long_mode(s_d_message, LV_LABEL_LONG_CLIP);
}

/* Ein Label in den Ausschnitt legen; leerer Text = versteckt. Gibt die neue Unterkante. */
static int32_t place_label(lv_obj_t *label, const lv_font_t *font, const char *text, int32_t y)
{
    if (!text[0]) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return y;
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_width(label, SHEET_W);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, SHEET_SIDE, y);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    return y + label_height(label) + DETAIL_GAP;
}

static void detail_render_locked(void)
{
    const lv_font_t *body = UI_TYPO(UI_TYPO_BODY);
    int32_t y = 0;
    y = place_label(s_d_title, UI_TYPO(UI_TYPO_TITLE), s_d.title, y);
    y = place_label(s_d_meta, UI_TYPO(UI_TYPO_META), s_d.meta, y);
    y = place_label(s_d_text, body, s_d.text, y);
    for (int i = 0; i < DETAIL_SUBTASKS; i++) {
        if (i >= s_d.sub_count) {
            lv_obj_add_flag(s_d_sub[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_style_text_color(s_d_sub[i],
                                    lv_color_hex(s_d.sub_done[i] ? 0x6f7f95 : 0xf2f2f2), 0);
        lv_obj_set_style_text_decor(s_d_sub[i], s_d.sub_done[i] ? LV_TEXT_DECOR_STRIKETHROUGH :
                                                                  LV_TEXT_DECOR_NONE, 0);
        y = place_label(s_d_sub[i], body, s_d.sub[i], y);
    }
    s_d_content_h = y > DETAIL_GAP ? y - DETAIL_GAP : 0;
    show_obj(s_d_message, s_d_loading);
}

static void detail_message_locked(const char *text)
{
    char clean[32];
    ui_status_copy_display_text(clean, text, sizeof(clean));
    lv_obj_set_style_text_font(s_d_message, UI_TYPO(UI_TYPO_BODY), 0);
    lv_label_set_text(s_d_message, clean);
    lv_obj_set_width(s_d_message, LV_SIZE_CONTENT);
    lv_obj_align(s_d_message, LV_ALIGN_TOP_MID, 0, CONTENT_H / 2);
}

static void detail_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_d_timer = NULL;
    if (s_d_visible && s_d_loading) {
        detail_message_locked("Keine Antwort");
    }
}

static void detail_hide_locked(void)
{
    s_d_visible = false;
    s_d_loading = false;
    if (s_d_timer) {
        lv_timer_delete(s_d_timer);
        s_d_timer = NULL;
    }
    if (s_d_root) {
        lv_obj_add_flag(s_d_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_detail_open(uint16_t rev, uint8_t index, const char *title)
{
    ui_status_lock();
    detail_create_locked();
    memset(&s_d, 0, sizeof(s_d));
    ui_status_copy_display_text(s_d.title, title ? title : "", sizeof(s_d.title));
    s_d_rev = rev;
    s_d_index = index;
    s_d_visible = true;
    s_d_loading = true;
    detail_message_locked("Lädt …");
    detail_render_locked();
    lv_obj_scroll_to_y(s_d_box, 0, LV_ANIM_OFF);
    if (s_d_timer) {
        lv_timer_delete(s_d_timer);
    }
    s_d_timer = lv_timer_create(detail_timer_cb, DETAIL_ANSWER_MS, NULL);
    lv_timer_set_repeat_count(s_d_timer, 1);
    lv_obj_remove_flag(s_d_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_d_root);
    ui_status_unlock();
}

/* `src` an `dst` haengen (Anzeige-Kopie), ohne den Puffer zu sprengen. */
static void append_display(char *dst, size_t size, const char *src)
{
    const size_t used = strlen(dst);
    if (used + 1 >= size) {
        return;
    }
    ui_status_copy_display_text(dst + used, src, size - used);
}

void ui_detail_part(uint16_t rev, uint8_t index, uint8_t part, uint8_t total,
                    const char *title, const char *project, const char *due, int prio,
                    const char *text, const char *const *subtasks, const bool *subtask_done,
                    uint8_t subtask_count)
{
    (void)total;
    ui_status_lock();
    if (!s_d_visible || rev != s_d_rev || index != s_d_index) {
        ui_status_unlock();
        return;
    }
    if (part == 0) {
        memset(&s_d, 0, sizeof(s_d));
        ui_status_copy_display_text(s_d.title, title ? title : "", sizeof(s_d.title));
        char meta[96];
        meta[0] = '\0';
        char clean[48];
        if (project && project[0]) {
            ui_status_copy_display_text(clean, project, sizeof(clean));
            strlcat(meta, clean, sizeof(meta));
        }
        if (due && due[0]) {
            ui_status_copy_display_text(clean, due, sizeof(clean));
            if (meta[0]) {
                strlcat(meta, " · ", sizeof(meta));
            }
            strlcat(meta, clean, sizeof(meta));
        }
        /* Prioritaet nur, wenn sie etwas sagt: 3 ist "normal". */
        if (prio >= 1 && prio <= 4 && prio != 3) {
            char p[32];
            snprintf(p, sizeof(p), "%sPrio %d", meta[0] ? " · " : "", prio);
            strlcat(meta, p, sizeof(meta));
        }
        ui_status_copy_display_text(s_d.meta, meta, sizeof(s_d.meta));
        s_d_loading = false;
        if (s_d_timer) {
            lv_timer_delete(s_d_timer);
            s_d_timer = NULL;
        }
    }
    if (text && text[0]) {
        append_display(s_d.text, sizeof(s_d.text), text);
    }
    for (uint8_t i = 0; subtasks && i < subtask_count && s_d.sub_count < DETAIL_SUBTASKS; i++) {
        char line[DETAIL_SUB_LEN];
        snprintf(line, sizeof(line), "- %s", subtasks[i] ? subtasks[i] : "");
        ui_status_copy_display_text(s_d.sub[s_d.sub_count], line, DETAIL_SUB_LEN);
        s_d.sub_done[s_d.sub_count] = subtask_done && subtask_done[i];
        s_d.sub_count++;
    }
    detail_render_locked();
    ui_status_unlock();
}

void ui_detail_hide(void)
{
    ui_status_lock();
    detail_hide_locked();
    ui_status_unlock();
}

bool ui_detail_visible(void)
{
    return s_d_visible;
}

bool ui_detail_touch(int gesture, int16_t x, int16_t y)
{
    bool closed = false;
    ui_status_lock();
    if (!s_d_visible) {
        ui_status_unlock();
        return true;
    }
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_TAP:
    case VOICE_BOARD_LONG_PRESS:
        closed = ui_back_hit(s_d_back, x, y);
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        closed = true;
        break;
    case VOICE_BOARD_SWIPE_UP:
        (void)scroll_box_locked(s_d_box, s_d_content_h, 1);
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        (void)scroll_box_locked(s_d_box, s_d_content_h, -1);
        break;
    default:
        break;
    }
    if (closed) {
        detail_hide_locked();
    }
    ui_status_unlock();
    return closed;
}

/* ── PIN ───────────────────────────────────────────────────────────────── */

/*
 * Vier grosse Felder 1-4 im Quadrat (2 x 2, je 150 x 104 px), darueber vier
 * Punkte fuer die Ziffern, die schon getippt sind, und eine Zeile fuer
 * "Falsch" oder die Sperre. Die PIN selbst kennt die Uhr nicht -- main.c
 * vergleicht den Hash.
 */
#define PIN_CELL_W 150
#define PIN_CELL_H 104
#define PIN_CELL_GAP 14
#define PIN_DOT 16
#define PIN_DOT_GAP 18
#define PIN_DOTS_Y (SHEET_TITLE_Y + 46)
#define PIN_MSG_Y (PIN_DOTS_Y + PIN_DOT + 10)
#define PIN_GRID_Y (PIN_MSG_Y + 36)

static lv_obj_t *s_n_root;
static lv_obj_t *s_n_back;
static lv_obj_t *s_n_title;
static lv_obj_t *s_n_msg;
static lv_obj_t *s_n_dot[UI_PIN_DIGITS];
static lv_obj_t *s_n_cell[4];
static bool s_n_visible;
static char s_n_entry[UI_PIN_DIGITS + 1];

static void pin_create_locked(void)
{
    if (s_n_root) {
        return;
    }
    s_n_root = make_root();
    s_n_back = ui_back_create(s_n_root);
    s_n_title = make_label(s_n_root, 0x2ecc71);
    lv_label_set_long_mode(s_n_title, LV_LABEL_LONG_DOT);
    s_n_msg = make_label(s_n_root, 0x9aa3ad);
    lv_label_set_long_mode(s_n_msg, LV_LABEL_LONG_CLIP);
    const int32_t dots_w = UI_PIN_DIGITS * PIN_DOT + (UI_PIN_DIGITS - 1) * PIN_DOT_GAP;
    for (int i = 0; i < UI_PIN_DIGITS; i++) {
        s_n_dot[i] = lv_obj_create(s_n_root);
        lv_obj_remove_style_all(s_n_dot[i]);
        lv_obj_set_size(s_n_dot[i], PIN_DOT, PIN_DOT);
        lv_obj_set_style_radius(s_n_dot[i], PIN_DOT / 2, 0);
        lv_obj_set_style_border_width(s_n_dot[i], 2, 0);
        lv_obj_set_style_border_color(s_n_dot[i], lv_color_hex(0x9aa3ad), 0);
        lv_obj_set_style_bg_color(s_n_dot[i], lv_color_hex(0x2ecc71), 0);
        lv_obj_align(s_n_dot[i], LV_ALIGN_TOP_LEFT,
                     (CONTENT_W - dots_w) / 2 + i * (PIN_DOT + PIN_DOT_GAP), PIN_DOTS_Y);
    }
    const int32_t grid_w = 2 * PIN_CELL_W + PIN_CELL_GAP;
    for (int i = 0; i < 4; i++) {
        s_n_cell[i] = lv_obj_create(s_n_root);
        lv_obj_remove_style_all(s_n_cell[i]);
        lv_obj_remove_flag(s_n_cell[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_n_cell[i], PIN_CELL_W, PIN_CELL_H);
        lv_obj_set_style_radius(s_n_cell[i], 20, 0);
        lv_obj_set_style_bg_opa(s_n_cell[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_n_cell[i], lv_color_hex(0x1b2430), 0);
        lv_obj_align(s_n_cell[i], LV_ALIGN_TOP_LEFT,
                     (CONTENT_W - grid_w) / 2 + (i % 2) * (PIN_CELL_W + PIN_CELL_GAP),
                     PIN_GRID_Y + (i / 2) * (PIN_CELL_H + PIN_CELL_GAP));
        lv_obj_t *digit = lv_label_create(s_n_cell[i]);
        const char text[2] = { (char)('1' + i), '\0' };
        lv_label_set_text(digit, text);
        lv_obj_set_style_text_font(digit, UI_FONT_STATUS, 0);
        lv_obj_set_style_text_color(digit, lv_color_hex(0xf2f2f2), 0);
        lv_obj_center(digit);
    }
}

static void pin_render_locked(void)
{
    const size_t n = strlen(s_n_entry);
    for (int i = 0; i < UI_PIN_DIGITS; i++) {
        lv_obj_set_style_bg_opa(s_n_dot[i], (size_t)i < n ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void ui_pin_show(const char *title)
{
    char clean[48];
    ui_status_lock();
    pin_create_locked();
    s_n_entry[0] = '\0';
    ui_status_copy_display_text(clean, title ? title : "", sizeof(clean));
    lv_obj_set_style_text_font(s_n_title, UI_TYPO(UI_TYPO_TITLE), 0);
    lv_obj_set_width(s_n_title, SHEET_W);
    lv_obj_set_height(s_n_title, lv_font_get_line_height(UI_TYPO(UI_TYPO_TITLE)));
    lv_label_set_text(s_n_title, clean);
    lv_obj_align(s_n_title, LV_ALIGN_TOP_LEFT, SHEET_SIDE, SHEET_TITLE_Y);
    lv_label_set_text(s_n_msg, "");
    s_n_visible = true;
    pin_render_locked();
    lv_obj_remove_flag(s_n_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_n_root);
    ui_status_unlock();
}

void ui_pin_hide(void)
{
    ui_status_lock();
    s_n_visible = false;
    s_n_entry[0] = '\0';
    if (s_n_root) {
        lv_obj_add_flag(s_n_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_unlock();
}

bool ui_pin_visible(void)
{
    return s_n_visible;
}

void ui_pin_reset(const char *message, bool error)
{
    char clean[48];
    ui_status_lock();
    if (s_n_root) {
        s_n_entry[0] = '\0';
        ui_status_copy_display_text(clean, message ? message : "", sizeof(clean));
        lv_obj_set_style_text_font(s_n_msg, UI_TYPO(UI_TYPO_META), 0);
        lv_obj_set_style_text_color(s_n_msg, lv_color_hex(error ? 0xe74c3c : 0x9aa3ad), 0);
        lv_label_set_text(s_n_msg, clean);
        lv_obj_set_width(s_n_msg, LV_SIZE_CONTENT);
        lv_obj_align(s_n_msg, LV_ALIGN_TOP_MID, 0, PIN_MSG_Y);
        pin_render_locked();
    }
    ui_status_unlock();
}

ui_pin_action_t ui_pin_touch(int gesture, int16_t x, int16_t y, char *out)
{
    ui_pin_action_t action = UI_PIN_NONE;
    ui_status_lock();
    if (!s_n_visible) {
        ui_status_unlock();
        return UI_PIN_CLOSED;
    }
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_TAP:
    case VOICE_BOARD_LONG_PRESS: {
        if (ui_back_hit(s_n_back, x, y)) {
            action = UI_PIN_CLOSED;
            break;
        }
        for (int i = 0; i < 4; i++) {
            if (!hit_obj(s_n_cell[i], x, y)) {
                continue;
            }
            const size_t n = strlen(s_n_entry);
            if (n < UI_PIN_DIGITS) {
                s_n_entry[n] = (char)('1' + i);
                s_n_entry[n + 1] = '\0';
            }
            pin_render_locked();
            if (strlen(s_n_entry) == UI_PIN_DIGITS) {
                if (out) {
                    strlcpy(out, s_n_entry, UI_PIN_DIGITS + 1);
                }
                action = UI_PIN_DONE;
            }
            break;
        }
        break;
    }
    case VOICE_BOARD_SWIPE_RIGHT:
        action = UI_PIN_CLOSED;
        break;
    default:
        break;
    }
    if (action == UI_PIN_CLOSED) {
        s_n_visible = false;
        s_n_entry[0] = '\0';
        lv_obj_add_flag(s_n_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_unlock();
    return action;
}

/* ── Diagnose ──────────────────────────────────────────────────────────── */

#define DIAG_TEXT_LEN 512

static lv_obj_t *s_g_root;
static lv_obj_t *s_g_back;
static lv_obj_t *s_g_title;
static lv_obj_t *s_g_box;
static lv_obj_t *s_g_text;
static bool s_g_visible;
static int32_t s_g_content_h;

static void diag_create_locked(void)
{
    if (s_g_root) {
        return;
    }
    s_g_root = make_root();
    s_g_back = ui_back_create(s_g_root);
    /* Kurzer Titel: er passt neben das Zurueck, mittig mit Abstand auf beiden Seiten. */
    s_g_title = make_label(s_g_root, 0x2ecc71);
    lv_label_set_text(s_g_title, "Diagnose");
    s_g_box = make_scroller(s_g_root, UI_BACK_H + 8, SCROLL_BOTTOM - UI_BACK_H - 8);
    s_g_text = make_label(s_g_box, 0xc9d1d9);
}

static void diag_set_locked(const char *text)
{
    static char clean[DIAG_TEXT_LEN]; /* unter der Sperre */
    ui_status_copy_display_text(clean, text ? text : "", sizeof(clean));
    lv_obj_set_style_text_font(s_g_title, UI_TYPO(UI_TYPO_TITLE), 0);
    lv_obj_set_width(s_g_title, LV_SIZE_CONTENT);
    lv_obj_align(s_g_title, LV_ALIGN_TOP_MID, 0,
                 (UI_BACK_H - lv_font_get_line_height(UI_TYPO(UI_TYPO_TITLE))) / 2);
    lv_obj_set_style_text_font(s_g_text, UI_TYPO(UI_TYPO_META), 0);
    lv_obj_set_style_text_line_space(s_g_text, 4, 0);
    lv_obj_set_width(s_g_text, SHEET_W);
    lv_label_set_text(s_g_text, clean);
    lv_obj_align(s_g_text, LV_ALIGN_TOP_LEFT, SHEET_SIDE, 0);
    s_g_content_h = label_height(s_g_text);
}

void ui_diag_show(const char *text)
{
    ui_status_lock();
    diag_create_locked();
    diag_set_locked(text);
    lv_obj_scroll_to_y(s_g_box, 0, LV_ANIM_OFF);
    s_g_visible = true;
    lv_obj_remove_flag(s_g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_g_root);
    ui_status_unlock();
}

void ui_diag_update(const char *text)
{
    ui_status_lock();
    if (s_g_visible) {
        diag_set_locked(text);
    }
    ui_status_unlock();
}

void ui_diag_hide(void)
{
    ui_status_lock();
    s_g_visible = false;
    if (s_g_root) {
        lv_obj_add_flag(s_g_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_unlock();
}

bool ui_diag_visible(void)
{
    return s_g_visible;
}

bool ui_diag_touch(int gesture, int16_t x, int16_t y)
{
    bool closed = false;
    ui_status_lock();
    if (!s_g_visible) {
        ui_status_unlock();
        return true;
    }
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_TAP:
    case VOICE_BOARD_LONG_PRESS:
        closed = ui_back_hit(s_g_back, x, y);
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        closed = true;
        break;
    case VOICE_BOARD_SWIPE_UP:
        (void)scroll_box_locked(s_g_box, s_g_content_h, 1);
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        (void)scroll_box_locked(s_g_box, s_g_content_h, -1);
        break;
    default:
        break;
    }
    if (closed) {
        s_g_visible = false;
        lv_obj_add_flag(s_g_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_unlock();
    return closed;
}
