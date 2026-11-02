#include "ui_cards.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "ui_status_internal.h"
#include "voice_board.h"

static const char *TAG = "ui_cards";

/* Farbe einer Zeile: 0xRRGGBB plus dieses Bit, wenn eine mitkam. */
#define CARD_COLOUR_SET 0x01000000u

/*
 * Gruppen einer Liste ("Termine", "Angepinnt", "Ueberfaellig", "Heute",
 * "Ausserdem wichtig"): hoechstens fuenf Namen je Liste (bis 0.4.3 vier --
 * "Termine" kam mit Paket I dazu), jede Zeile traegt nur den Index
 * (0 = keine). So kostet die Ueberschrift je Liste 120 Byte statt 24 je Zeile.
 */
#define CARD_GROUPS_MAX 5
/* Termine (Flag `e`, 0.4.4) tragen statt der Projektfarbe einen Streifen in Kalender-Blau. */
#define CARD_EVENT_COLOUR 0x3b82f6u
#define CARD_GROUP_LEN 24

typedef struct {
    char text[UI_CARD_TEXT_LEN];
    char aux[UI_CARD_AUX_LEN];
    uint32_t colour;  /* Projektfarbe (nur `today`), CARD_COLOUR_SET wenn gesetzt */
    uint8_t group;    /* 1..CARD_GROUPS_MAX in `groups`, 0 = keine */
    bool confirm;     /* Schalter: erst markieren, dann "OK - schalten" (Flag `c`) */
    bool event;       /* Termin (Flag `e`): nicht antippbar, nur zum Lesen */
    bool select;      /* input_select (Flag `s`, 0.4.5): Tippen oeffnet die Auswahl */
    bool done;        /* angetippt; die Bruecke hat es noch nicht bestaetigt */
} card_item_t;

typedef struct {
    uint16_t rev;
    uint8_t count;
    uint8_t group_count;
    /*
     * Kam diese Liste je von der Bruecke? Ohne die Unterscheidung zeigte
     * eine Uhr ohne Daten -- Bruecke nicht verbunden, Glance gescheitert --
     * "Heute ist alles erledigt.", obwohl Aufgaben offen waren (11/2026).
     */
    bool received;
    char groups[CARD_GROUPS_MAX][CARD_GROUP_LEN];
    card_item_t items[UI_CARD_ITEMS_MAX];
} card_list_t;

static const char *const KIND_NAMES[UI_CARD_COUNT] = { "today", "shopping", "switches", "weather" };

/*
 * Die Listen liegen im PSRAM-faehigen Heap nicht -- es sind gut 2,6 KB je
 * Liste, statisch im internen RAM. Bei drei Listen vertretbar; mehr
 * Eintraege zeigt eine Uhr ohnehin nicht sinnvoll an. Die Wetterkarte
 * haelt ihren Stand selbst (ui_weather.c).
 */
static card_list_t s_lists[UI_CARD_LIST_COUNT];

/*
 * "#RRGGBB" -> Farbe mit CARD_COLOUR_SET; alles andere (leer, "#abc",
 * fehlend, kaputt) -> 0, also kein Streifen. Aeltere Bruecken schicken das
 * dritte Element gar nicht.
 */
static uint32_t parse_colour(const char *text)
{
    if (!text || text[0] != '#' || strlen(text) != 7) {
        return 0;
    }
    uint32_t value = 0;
    for (int i = 1; i < 7; i++) {
        const char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    return value | CARD_COLOUR_SET;
}

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"
#include "ui_status_icons.h"
#include "ui_weather.h"

/*
 * Ueberfaellig: `aux` ist dann das Datum "31.10." statt einer Uhrzeit (so
 * schickt es der Server seit Paket G). Die Zeile faerbt es rot.
 */
static bool is_date_aux(const char *aux)
{
    if (!aux || strlen(aux) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        const char c = aux[i];
        const bool dot = i == 2 || i == 5;
        if (dot ? c != '.' : (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

static const char *const TITLES[UI_CARD_COUNT] = { "Heute", "Einkauf", "Zuhause", "Wetter" };
/*
 * Leere Karten sagen etwas Freundliches, und Tecki sagt es mit: Heute
 * erledigt -- er freut sich; leere Einkaufsliste -- neutral; keine Schalter
 * -- er schaut zur Seite, wo man sie einrichtet.
 */
static const char *const EMPTY_HINTS[UI_CARD_COUNT] = {
    "Heute ist alles erledigt.",
    "Nichts auf der Einkaufsliste.",
    "Noch keine Schalter. In den Einstellungen unter Sprache einrichten.",
    "Noch kein Wetter vom Handy.",
};
/* Noch nie eine Liste bekommen: ehrlich sagen, dass Daten fehlen. */
static const char *const NO_DATA_HINT = "Noch keine Daten vom Handy. Ist die Todoteck-App verbunden?";

static const ui_status_icon_scene_t EMPTY_POSES[UI_CARD_COUNT] = {
    UI_STATUS_ICON_HAPPY,
    UI_STATUS_ICON_IDLE,
    UI_STATUS_ICON_LOOK_RIGHT,
    UI_STATUS_ICON_LOOK_LEFT,
};

/*
 * Zeilen und Schrift nach der Schriftgroesse aus `watch_settings`
 * (ui_status_font_level): klein 20 px, normal 23, gross 26. Jede Zeile ist
 * auf **zwei Textzeilen** ausgelegt: Ein langer Titel bricht einmal um und
 * endet dann mit "..." -- bis 0.3.5 hatte das Label keine feste Hoehe, der
 * Umbruch lief ueber die Zeile hinaus und wurde oben und unten abgeschnitten
 * ("Mini-Haus Dachboden: Box und Deco mon"). Die Hoehe einer Zeile ist also
 * 2 x line_height der Schrift plus ROW_PAD Innenabstand; wie viele davon in
 * den sichtbaren Ausschnitt passen, rechnet rows_per_view() aus den echten
 * Schriftmassen: klein 6, normal 5, gross 5 (Tabelle in
 * ui_layout_watch206.h). Was uebrig bleibt, verteilt sich auf die Zeilen.
 *
 * Seit Paket G (11/2026) stehen **alle** Zeilen einer Liste untereinander in
 * einem scrollenden Ausschnitt (LVGL-Scroll mit Schwung und Rastung an den
 * Zeilen) statt auf Seiten: Nach oben wischen zieht den naechsten Ausschnitt
 * herein, nach unten den vorigen -- am oberen Ende ist nach unten weiterhin
 * "zurueck". Die Seitenpunkte unten sind ein schmaler Balken geworden, der
 * die Lage im Ganzen zeigt. Der Finger steuert den Scroll nicht selbst: Die
 * Touch-Schicht meldet Gesten erst beim Abheben, LVGL kennt kein
 * Eingabegeraet; der Schwung ist die Animation des Sprungs.
 *
 * Zwischen zwei Gruppen (`group` je Eintrag, nur Heute) steht eine
 * Trennzeile: duenner Text in UI_FONT_SMALL, 28 px hoch, kein Trefferziel.
 */
#define ROWS_MAX UI_CARD_ITEMS_MAX
#define ROWS_TOP (HEADER_H + 22)
#define ROWS_BOTTOM (CONTENT_H - 24)
#define ROW_GAP 6
#define ROW_PAD 8
#define ROW_TEXT_LINES 2
#define STRIPE_W 6
#define SEP_H 28
#define SEPS_MAX CARD_GROUPS_MAX
/* Der Balken unten, wo bis 0.3.6 die Seitenpunkte standen. */
#define BAR_W 120
#define BAR_H 4
static const lv_font_t *const LEVEL_ROW_FONT[3] = {
    &todoteck_watch_font_20, &todoteck_watch_font_23, &todoteck_watch_font_26,
};

/* Rueckgaengig: 5 s nach einem Tippen auf eine Zeile, unten in der Karte. */
#define UNDO_W 240
#define UNDO_H 56
#define UNDO_MS 5000
/*
 * Bestaetigung auf der Karte Heute (und bei Schaltern mit Flag `c`): Das
 * erste Tippen markiert die Zeile, erst "OK - erledigt" / "OK - schalten"
 * fuehrt aus. Der Chip sitzt rechts **in** der markierten Zeile, nicht unten
 * wie "Rueckgaengig": Unten laege er ueber der letzten Zeile, und wer nach
 * der Markierung eine untere Zeile antippen wollte, bestaetigte stattdessen
 * die markierte -- genau der Fehlgriff, den die Bestaetigung verhindern
 * soll. In der Zeile verdeckt er nur die eigene Zusatzangabe (Uhrzeit,
 * An/Aus), ist so hoch wie die Zeile (also >= 48 px), und seine Mitte liegt
 * bei x 319 weit weg von den Eckrundungen. 150 px fassen "OK - erledigt" in
 * der 20er (rund 116 px) mit Rand.
 */
#define OK_W 150
#define OK_MS 5000
/* Teckis Flaeche auf der leeren Karte. */
#define EMPTY_TECKI_BOX 96
#define EMPTY_TECKI_Y 110

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static ui_dots_t s_card_dots;
static lv_obj_t *s_bar_track;
static lv_obj_t *s_bar_thumb;
/* Seiten der Wetterkarte: Punkte unten (wie bis 0.3.6 bei allen Karten) und die gezeigte Seite. */
static ui_dots_t s_page_dots;
static uint8_t s_wpage;
static lv_obj_t *s_empty;
static ui_status_icons_t s_tecki;
static lv_obj_t *s_list;
static lv_obj_t *s_row[ROWS_MAX];
static lv_obj_t *s_row_stripe[ROWS_MAX];
static lv_obj_t *s_row_text[ROWS_MAX];
static lv_obj_t *s_row_aux[ROWS_MAX];
static lv_obj_t *s_sep[SEPS_MAX];
static lv_obj_t *s_undo;
static lv_timer_t *s_undo_timer;
static lv_obj_t *s_ok;
static lv_obj_t *s_ok_label;
static lv_timer_t *s_sel_timer;
static bool s_visible;
static ui_card_kind_t s_kind;
/* Oberkanten der Zeilen im Ausschnitt (fuer das zeilenweise Blaettern) und die Gesamthoehe. */
static int32_t s_row_top[ROWS_MAX];
static int32_t s_content_h;

/* Was der Chip rueckgaengig machen wuerde. */
static struct {
    bool armed;
    ui_card_kind_t kind;
    uint16_t rev;
    uint8_t index;
    bool prev_done;
} s_last;

/* Markierte Zeile, die auf "OK" wartet (Index in der Liste). */
static struct {
    bool active;
    uint16_t rev;
    uint8_t index;
} s_sel;

static const lv_font_t *row_font(void)
{
    return LEVEL_ROW_FONT[ui_status_font_level() % 3];
}

static int32_t view_h(void)
{
    return ROWS_BOTTOM - ROWS_TOP;
}

static uint8_t rows_per_view(void)
{
    const int32_t min_pitch = ROW_TEXT_LINES * lv_font_get_line_height(row_font()) + ROW_PAD +
                              ROW_GAP;
    int32_t rows = view_h() / min_pitch;
    if (rows < 1) {
        rows = 1;
    }
    return (uint8_t)(rows > 6 ? 6 : rows);
}

/* Hoehe einer Zeile ohne Luecke: der Ausschnitt geteilt durch die Zeilen, die hineinpassen. */
static int32_t row_h(void)
{
    return view_h() / rows_per_view() - ROW_GAP;
}

/* Schalter: ist `aux` der Schaltzustand "1"/"0" (sonst ein Wort, das die Uhr nur zeigt)? */
static bool aux_is_binary(const char *aux)
{
    return strcmp(aux, "1") == 0 || strcmp(aux, "0") == 0;
}

/* Braucht diese Zeile die Bestaetigung ueber den OK-Chip? */
static bool needs_confirm(const card_list_t *list, int index)
{
    return s_kind == UI_CARD_TODAY || (s_kind == UI_CARD_SWITCHES && list->items[index].confirm);
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
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /*
     * 26 statt 30 px: Seit die Uhrzeit mittig in der Kopfzeile steht, muss
     * "Zuhause" links davor enden (rund 115 statt 135 px breit).
     *
     * Ueberschrift und Kartenpunkte stehen auf derselben waagerechten Mitte
     * wie die kleine Uhrzeit der Kopfzeile: Die liegt auf lv_layer_top() bei
     * UI_SCREEN_PAD + UI_HEADER_LABEL_Y in der 18er; diese Karte liegt im
     * Inhalt (also schon um UI_SCREEN_PAD eingerueckt), und die 26er ist um
     * 9 px hoeher -- der Unterschied der Zeilenhoehen wird halbiert und
     * abgezogen. Bis 0.3.5 stand "Heute" rund 10 px tiefer als die Uhrzeit.
     */
    const int32_t header_mid = UI_HEADER_LABEL_Y + lv_font_get_line_height(UI_FONT_SMALL) / 2;
    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, &todoteck_watch_font_26, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, UI_HEADER_INSET_X,
                 header_mid - lv_font_get_line_height(&todoteck_watch_font_26) / 2);

    /* Welche Karte: Punkte oben rechts (statt "1/3"), auf derselben Mitte. */
    ui_dots_create(&s_card_dots, s_root);
    lv_obj_align(s_card_dots.box, LV_ALIGN_TOP_RIGHT, -UI_HEADER_INSET_X,
                 header_mid - UI_DOTS_SIZE / 2);
    /* Lage in der Liste: schmaler Balken unten mittig, nur wenn die Liste scrollt. */
    s_bar_track = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_bar_track);
    lv_obj_set_size(s_bar_track, BAR_W, BAR_H);
    lv_obj_set_style_radius(s_bar_track, BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(s_bar_track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bar_track, lv_color_hex(0x3a4656), 0);
    lv_obj_align(s_bar_track, LV_ALIGN_TOP_MID, 0, CONTENT_H - 14);
    lv_obj_add_flag(s_bar_track, LV_OBJ_FLAG_HIDDEN);
    s_bar_thumb = lv_obj_create(s_bar_track);
    lv_obj_remove_style_all(s_bar_thumb);
    lv_obj_set_size(s_bar_thumb, BAR_W / 2, BAR_H);
    lv_obj_set_style_radius(s_bar_thumb, BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(s_bar_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bar_thumb, lv_color_hex(0x2ecc71), 0);

    ui_dots_create(&s_page_dots, s_root);
    lv_obj_align(s_page_dots.box, LV_ALIGN_TOP_MID, 0, CONTENT_H - 16);

    ui_status_icons_create(&s_tecki, s_root);
    ui_status_icons_show(&s_tecki, false);
    ui_weather_create_locked(s_root);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_empty, UI_FONT_QUESTION, 0);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, CONTENT_W - 2 * UI_HEADER_INSET_X);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, EMPTY_TECKI_Y + EMPTY_TECKI_BOX + 16);

    /*
     * Der scrollende Ausschnitt: nur senkrecht, Rastung an den Oberkanten
     * der Kinder, kein eigener Balken (der steht unten), kein elastisches
     * Ueberziehen -- das sieht auf einem Sprung per Geste nach Fehler aus.
     */
    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, CONTENT_W, view_h());
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, ROWS_TOP);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    for (int i = 0; i < SEPS_MAX; i++) {
        s_sep[i] = lv_label_create(s_list);
        lv_obj_set_style_text_font(s_sep[i], UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_sep[i], lv_color_hex(0x9aa3ad), 0);
        lv_label_set_long_mode(s_sep[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_sep[i], CONTENT_W - 16);
        lv_obj_add_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < ROWS_MAX; i++) {
        s_row[i] = lv_obj_create(s_list);
        lv_obj_remove_style_all(s_row[i]);
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_set_style_radius(s_row[i], 16, 0);
        lv_obj_set_style_bg_opa(s_row[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(0x1b2430), 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

        /* Projektfarbe als Streifen links; ohne Farbe versteckt. */
        s_row_stripe[i] = lv_obj_create(s_row[i]);
        lv_obj_remove_style_all(s_row_stripe[i]);
        lv_obj_set_style_radius(s_row_stripe[i], STRIPE_W / 2, 0);
        lv_obj_set_style_bg_opa(s_row_stripe[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_row_stripe[i], LV_OBJ_FLAG_HIDDEN);

        /*
         * Feste Breite und Hoehe setzt render_locked(); erst mit beiden
         * kuerzt LV_LABEL_LONG_DOT -- ohne feste Hoehe bricht es nur um.
         */
        s_row_text[i] = lv_label_create(s_row[i]);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);

        s_row_aux[i] = lv_label_create(s_row[i]);
        lv_obj_set_style_text_font(s_row_aux[i], UI_FONT_SMALL, 0);
        lv_obj_align(s_row_aux[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* Rueckgaengig-Chip: hell gefuellt, damit er ueber den dunklen Zeilen steht. */
    s_undo = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_undo);
    lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_undo, UNDO_W, UNDO_H);
    lv_obj_set_style_radius(s_undo, UNDO_H / 2, 0);
    lv_obj_set_style_bg_opa(s_undo, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_undo, lv_color_hex(0xf2f2f2), 0);
    lv_obj_align(s_undo, LV_ALIGN_TOP_MID, 0, ROWS_BOTTOM - UNDO_H);
    lv_obj_t *undo_label = lv_label_create(s_undo);
    lv_label_set_text(undo_label, "Rückgängig");
    lv_obj_set_style_text_font(undo_label, &todoteck_watch_font_23, 0);
    lv_obj_set_style_text_color(undo_label, lv_color_hex(0x000000), 0);
    lv_obj_center(undo_label);
    lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);

    /* OK-Chip: gruen mit schwarzer Schrift, Lage und Text setzt render_locked(). */
    s_ok = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_ok);
    lv_obj_remove_flag(s_ok, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_ok, 16, 0);
    lv_obj_set_style_bg_opa(s_ok, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ok, lv_color_hex(0x2ecc71), 0);
    s_ok_label = lv_label_create(s_ok);
    lv_label_set_text(s_ok_label, "OK");
    lv_obj_set_style_text_font(s_ok_label, &todoteck_watch_font_20, 0);
    lv_obj_set_style_text_color(s_ok_label, lv_color_hex(0x000000), 0);
    lv_obj_center(s_ok_label);
    lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
}

static void hide_undo_locked(void)
{
    s_last.armed = false;
    if (s_undo_timer) {
        lv_timer_delete(s_undo_timer);
        s_undo_timer = NULL;
    }
    if (s_undo) {
        lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Markierung verwerfen; nichts wird gesendet. Zeichnet nicht neu. */
static bool clear_selection_locked(void)
{
    const bool was_active = s_sel.active;
    s_sel.active = false;
    if (s_sel_timer) {
        lv_timer_delete(s_sel_timer);
        s_sel_timer = NULL;
    }
    if (s_ok) {
        lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
    }
    return was_active;
}

static void render_locked(void);

static void sel_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_sel_timer = NULL;
    clear_selection_locked();
    if (s_visible) {
        render_locked();
    }
}

static void undo_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_undo_timer = NULL;
    hide_undo_locked();
}

/* Der Balken unten: Lage und Laenge des sichtbaren Ausschnitts im Ganzen. */
static void update_bar_locked(int32_t scroll_y)
{
    const int32_t view = view_h();
    if (s_content_h <= view) {
        lv_obj_add_flag(s_bar_track, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int32_t thumb_w = BAR_W * view / s_content_h;
    if (thumb_w < 16) {
        thumb_w = 16;
    }
    if (scroll_y < 0) {
        scroll_y = 0;
    }
    if (scroll_y > s_content_h - view) {
        scroll_y = s_content_h - view;
    }
    const int32_t x = (BAR_W - thumb_w) * scroll_y / (s_content_h - view);
    lv_obj_set_width(s_bar_thumb, thumb_w);
    lv_obj_align(s_bar_thumb, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_remove_flag(s_bar_track, LV_OBJ_FLAG_HIDDEN);
}

/* Die Wetterkarte: keine Liste, eigene Seiten, Tecki nur ohne Daten. */
static void render_weather_locked(void)
{
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bar_track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
    const uint8_t pages = ui_weather_page_count();
    if (s_wpage >= pages) {
        s_wpage = (uint8_t)(pages - 1);
    }
    ui_dots_set(&s_page_dots, pages > 1 ? pages : 0, s_wpage);
    if (!ui_weather_has_data()) {
        lv_label_set_text(s_empty, ui_weather_received() ? EMPTY_HINTS[UI_CARD_WEATHER]
                                                         : NO_DATA_HINT);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        ui_status_icons_set_box(&s_tecki, EMPTY_TECKI_BOX, (CONTENT_W - EMPTY_TECKI_BOX) / 2,
                                EMPTY_TECKI_Y);
        ui_status_icons_apply(&s_tecki, EMPTY_POSES[UI_CARD_WEATHER]);
        ui_status_icons_show(&s_tecki, true);
        ui_weather_render_locked(0, false);
        return;
    }
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    ui_status_icons_show(&s_tecki, false);
    ui_weather_render_locked(s_wpage, true);
    /* Eine Warnung, die so noch nicht zu sehen war: einmal vibrieren. */
    if (ui_weather_take_new_warning_locked() && !ui_status_night_mode()) {
        voice_board_haptic_pattern(2, 120, 120);
    }
}

static void render_locked(void)
{
    if (!s_root) {
        return;
    }
    lv_label_set_text(s_title, TITLES[s_kind]);
    ui_dots_set(&s_card_dots, UI_CARD_COUNT, (uint8_t)s_kind);
    if (s_kind == UI_CARD_WEATHER) {
        render_weather_locked();
        return;
    }
    ui_weather_render_locked(0, false);
    ui_dots_set(&s_page_dots, 0, 0);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    const card_list_t *list = &s_lists[s_kind];
    const lv_font_t *font = row_font();
    const int32_t line_h = lv_font_get_line_height(font);
    const int32_t h = row_h();

    if (list->count == 0) {
        lv_label_set_text(s_empty, list->received ? EMPTY_HINTS[s_kind] : NO_DATA_HINT);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        ui_status_icons_set_box(&s_tecki, EMPTY_TECKI_BOX, (CONTENT_W - EMPTY_TECKI_BOX) / 2,
                                EMPTY_TECKI_Y);
        ui_status_icons_apply(&s_tecki, list->received ? EMPTY_POSES[s_kind] : UI_STATUS_ICON_LOOK_LEFT);
        ui_status_icons_show(&s_tecki, true);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        ui_status_icons_show(&s_tecki, false);
    }

    /*
     * Zeilen und Trennzeilen von oben nach unten in den Ausschnitt legen.
     * Eine Trennzeile, wo sich die Gruppe gegenueber der Zeile davor
     * aendert -- auch vor der ersten, wenn sie eine hat.
     */
    int32_t y = 0;
    uint8_t prev_group = 0;
    int sep_used = 0;
    bool ok_shown = false;
    for (int i = 0; i < ROWS_MAX; i++) {
        if (i >= list->count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            s_row_top[i] = y;
            continue;
        }
        const card_item_t *item = &list->items[i];
        if (item->group != 0 && item->group != prev_group && sep_used < SEPS_MAX &&
            item->group <= list->group_count) {
            lv_label_set_text(s_sep[sep_used], list->groups[item->group - 1]);
            lv_obj_align(s_sep[sep_used], LV_ALIGN_TOP_LEFT, 16,
                         y + (SEP_H - lv_font_get_line_height(UI_FONT_SMALL)) / 2);
            lv_obj_remove_flag(s_sep[sep_used], LV_OBJ_FLAG_HIDDEN);
            sep_used++;
            y += SEP_H;
        }
        prev_group = item->group;
        s_row_top[i] = y;
        const bool selected = s_sel.active && s_sel.index == i;
        lv_obj_set_size(s_row[i], CONTENT_W, h);
        lv_obj_align(s_row[i], LV_ALIGN_TOP_LEFT, 0, y);
        /* Markiert: hellere Flaeche und gruener Rand, passend zum OK-Chip. */
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(selected ? 0x2a3b4f : 0x1b2430), 0);
        lv_obj_set_style_border_width(s_row[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_row[i], lv_color_hex(0x2ecc71), 0);

        const bool striped = (item->colour & CARD_COLOUR_SET) != 0;
        const int32_t text_x = striped ? 16 + STRIPE_W + 8 : 16;
        if (striped) {
            lv_obj_set_size(s_row_stripe[i], STRIPE_W, h - 16);
            lv_obj_align(s_row_stripe[i], LV_ALIGN_LEFT_MID, 12, 0);
            lv_obj_set_style_bg_color(s_row_stripe[i],
                                      lv_color_hex(item->colour & 0xFFFFFFu), 0);
            lv_obj_set_style_bg_opa(s_row_stripe[i], item->done ? LV_OPA_40 : LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_row_stripe[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_row_stripe[i], LV_OBJ_FLAG_HIDDEN);
        }

        /*
         * Breite des Titels: bis zum OK-Chip, wenn die Zeile markiert ist;
         * bis zur Zusatzangabe rechts (Uhrzeit, An/Aus), wenn es eine gibt;
         * sonst die volle Zeile. Hoehe: eine Textzeile, wenn der Titel in
         * eine passt (dann steht er mittig), sonst zwei -- LONG_DOT kuerzt
         * nach der zweiten mit "...".
         */
        const bool has_aux = s_kind == UI_CARD_SWITCHES || item->aux[0] != '\0';
        const int32_t text_w = (selected ? CONTENT_W - OK_W - 8 : has_aux ? CONTENT_W - 94 :
                                CONTENT_W - 16) - text_x;
        const bool one_line = lv_text_get_width(item->text, (uint32_t)strlen(item->text), font,
                                                0) <= text_w;
        lv_obj_set_style_text_font(s_row_text[i], font, 0);
        lv_obj_set_size(s_row_text[i], text_w, one_line ? line_h : ROW_TEXT_LINES * line_h);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, text_x, 0);
        lv_label_set_text(s_row_text[i], item->text);
        lv_obj_set_style_text_decor(s_row_text[i],
                                    item->done && s_kind != UI_CARD_SWITCHES ?
                                    LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_text_color(s_row_text[i],
                                    item->done ? lv_color_hex(0x6f7f95) : lv_color_hex(0xf2f2f2),
                                    0);
        if (s_kind == UI_CARD_SWITCHES && aux_is_binary(item->aux)) {
            /* Schalter: An/Aus als farbiges Wort; angetippt zeigt es den Zielzustand. */
            bool on = strcmp(item->aux, "1") == 0;
            if (item->done) {
                on = !on;
            }
            lv_label_set_text(s_row_aux[i], on ? "An" : "Aus");
            lv_obj_set_style_text_color(s_row_aux[i],
                                        on ? lv_color_hex(0x2ecc71) : lv_color_hex(0x9aa3ad), 0);
        } else if (s_kind == UI_CARD_SWITCHES) {
            /*
             * Ein Wort statt An/Aus (0.4.3): die Option eines input_select
             * oder "Offen"/"Zu" eines Schlosses. Den Zielstand kennt die Uhr
             * nicht -- angetippt steht "..." bis die Bruecke die Karte neu
             * schickt (sie tut das nach jeder Aktion).
             */
            lv_label_set_text(s_row_aux[i], item->done ? "..." : item->aux);
            lv_obj_set_style_text_color(s_row_aux[i],
                                        item->done ? lv_color_hex(0x9aa3ad) : lv_color_hex(0x2ecc71),
                                        0);
        } else {
            /* Ueberfaellig (Datum statt Uhrzeit): rot. */
            lv_label_set_text(s_row_aux[i], item->aux);
            lv_obj_set_style_text_color(s_row_aux[i],
                                        is_date_aux(item->aux) ? lv_color_hex(0xe74c3c) :
                                        lv_color_hex(0x9aa3ad), 0);
        }
        /* Unter dem OK-Chip laege die Zusatzangabe nur verdeckt. */
        if (selected) {
            lv_obj_add_flag(s_row_aux[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_row_aux[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        y += h + ROW_GAP;
    }
    for (int i = sep_used; i < SEPS_MAX; i++) {
        lv_obj_add_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_content_h = y > ROW_GAP ? y - ROW_GAP : 0;
    lv_obj_update_layout(s_list);

    /*
     * Der OK-Chip liegt im Wurzelobjekt, nicht im Ausschnitt: Er folgt der
     * markierten Zeile ueber die Lage im Ausschnitt (Bildschirmkoordinaten
     * der Zeile), und steht nur, wenn die Zeile gerade zu sehen ist.
     */
    if (s_sel.active && s_sel.index < list->count) {
        lv_area_t row_area;
        lv_area_t list_area;
        lv_obj_get_coords(s_row[s_sel.index], &row_area);
        lv_obj_get_coords(s_list, &list_area);
        if (row_area.y1 >= list_area.y1 - 4 && row_area.y2 <= list_area.y2 + 4) {
            lv_obj_set_size(s_ok, OK_W, h);
            lv_obj_align(s_ok, LV_ALIGN_TOP_LEFT, CONTENT_W - OK_W,
                         ROWS_TOP + s_row_top[s_sel.index] - lv_obj_get_scroll_y(s_list));
            lv_obj_remove_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
            ok_shown = true;
        }
    }
    if (!ok_shown) {
        lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
    }
    /* Die Chips liegen ueber den Zeilen, also nach ihnen nach vorn. */
    if (s_last.armed) {
        lv_obj_move_foreground(s_undo);
    }
    if (ok_shown) {
        lv_obj_move_foreground(s_ok);
    }
    update_bar_locked(lv_obj_get_scroll_y(s_list));
}

/* An den Anfang der Liste, ohne Animation -- beim Kartenwechsel und Oeffnen. */
static void scroll_top_locked(void)
{
    if (s_list) {
        lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    }
}

/*
 * Einen Ausschnitt weiter (dir +1) oder zurueck (dir -1), an einer
 * Zeilenoberkante ausgerichtet. `false`, wenn es in dieser Richtung nichts
 * mehr gibt -- nach unten heisst das am oberen Ende "zurueck", nach oben am
 * unteren Ende nichts.
 */
static bool scroll_page_locked(int dir)
{
    const card_list_t *list = &s_lists[s_kind];
    const int32_t view = view_h();
    const int32_t max_y = s_content_h > view ? s_content_h - view : 0;
    const int32_t y = lv_obj_get_scroll_y(s_list);
    int32_t target = -1;
    if (dir > 0) {
        if (y >= max_y) {
            return false;
        }
        /* Die erste Zeile, die unten nicht mehr ganz zu sehen ist. */
        for (int i = 0; i < list->count; i++) {
            if (s_row_top[i] + row_h() > y + view) {
                target = s_row_top[i];
                break;
            }
        }
        if (target < 0 || target <= y) {
            target = y + view;
        }
    } else {
        if (y <= 0) {
            return false;
        }
        /* Die Zeile, mit der der vorige Ausschnitt beginnt. */
        const int32_t want = y - view;
        target = 0;
        for (int i = 0; i < list->count; i++) {
            if (s_row_top[i] <= want) {
                target = s_row_top[i];
            }
        }
        if (target >= y) {
            target = 0;
        }
    }
    if (target > max_y) {
        target = max_y;
    }
    if (target < 0) {
        target = 0;
    }
    /* Mit Animation -- das ist der "Schwung" einer Geste, die LVGL selbst nie sieht. */
    lv_obj_scroll_to_y(s_list, target, LV_ANIM_ON);
    /* Chip und Balken folgen sofort dem Ziel, nicht der Animation. */
    update_bar_locked(target);
    if (s_sel.active) {
        clear_selection_locked();
    }
    lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* Abstand des Punkts zur Mitte einer Flaeche, wenn er in ihr samt Toleranz liegt; sonst -1. */
static int32_t hit_distance(lv_obj_t *obj, int16_t x, int16_t y)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return -1;
    }
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    if (x < area.x1 - UI_HIT_TOLERANCE || x > area.x2 + UI_HIT_TOLERANCE ||
        y < area.y1 - UI_HIT_TOLERANCE || y > area.y2 + UI_HIT_TOLERANCE) {
        return -1;
    }
    const int32_t cy = (area.y1 + area.y2) / 2;
    return y > cy ? y - cy : cy - y;
}

/*
 * Zeile `index` der sichtbaren Karte abhaken bzw. umschalten -- so, wie es
 * vor der Bestaetigung jedes Tippen tat: Die Zeile zeigt es sofort, und
 * fuenf Sekunden steht "Rueckgaengig" (ein neues Abhaken startet sie neu).
 */
static ui_cards_action_t check_off_locked(card_list_t *list, int index, ui_cards_hit_t *hit)
{
    card_item_t *item = &list->items[index];
    s_last.kind = s_kind;
    s_last.rev = list->rev;
    s_last.index = (uint8_t)index;
    s_last.prev_done = item->done;
    item->done = s_kind == UI_CARD_SWITCHES ? !item->done : true;
    if (hit) {
        hit->kind = s_kind;
        hit->rev = list->rev;
        hit->index = (uint8_t)index;
        strlcpy(hit->text, item->text, sizeof(hit->text));
        /* Schalter: `aux` ist der Stand vor dem Tippen, `done` kehrt ihn um. */
        hit->binary = aux_is_binary(item->aux);
        hit->on = (strcmp(item->aux, "1") == 0) != item->done;
    }
    hide_undo_locked();
    s_last.armed = true;
    s_undo_timer = lv_timer_create(undo_expired_cb, UNDO_MS, NULL);
    lv_timer_set_repeat_count(s_undo_timer, 1);
    lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
    render_locked();
    return UI_CARDS_ITEM;
}
#endif /* CONFIG_VOICE_BOARD_WATCH206 */

int ui_cards_kind_from_name(const char *name)
{
    for (int i = 0; i < UI_CARD_COUNT; i++) {
        if (name && strcmp(name, KIND_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *ui_cards_kind_name(ui_card_kind_t kind)
{
    return kind < UI_CARD_COUNT ? KIND_NAMES[kind] : "";
}

void ui_cards_begin_part(ui_card_kind_t kind, uint16_t rev, uint8_t part)
{
    if (kind >= UI_CARD_LIST_COUNT) {
        return;
    }
    ui_status_lock();
    if (part == 0) {
        memset(&s_lists[kind], 0, sizeof(s_lists[kind]));
    }
#if CONFIG_VOICE_BOARD_WATCH206
    /*
     * Neue Liste waehrend einer Markierung: Hinter dem Index steht jetzt
     * vielleicht eine andere Aufgabe -- ein "OK" bestaetigte die falsche.
     * Das Neuzeichnen kommt mit ui_cards_commit().
     */
    if (s_visible && s_kind == kind && s_sel.active && s_sel.rev != rev) {
        clear_selection_locked();
    }
#endif
    s_lists[kind].rev = rev;
    s_lists[kind].received = true;
    ui_status_unlock();
}

void ui_cards_add_item(ui_card_kind_t kind, const char *text, const char *aux,
                       const char *colour, const char *group, const char *flags)
{
    if (kind >= UI_CARD_LIST_COUNT) {
        return;
    }
    ui_status_lock();
    card_list_t *list = &s_lists[kind];
    if (list->count < UI_CARD_ITEMS_MAX) {
        card_item_t *item = &list->items[list->count++];
        ui_status_copy_display_text(item->text, text ? text : "", sizeof(item->text));
        ui_status_copy_display_text(item->aux, aux ? aux : "", sizeof(item->aux));
        item->event = flags && strchr(flags, 'e') != NULL;
        item->colour = item->event ? (CARD_EVENT_COLOUR | CARD_COLOUR_SET) : parse_colour(colour);
        item->confirm = !item->event && flags && strchr(flags, 'c') != NULL;
        item->select = kind == UI_CARD_SWITCHES && !item->event && flags &&
                       strchr(flags, 's') != NULL;
        item->done = false;
        item->group = 0;
        if (group && group[0]) {
            char name[CARD_GROUP_LEN];
            ui_status_copy_display_text(name, group, sizeof(name));
            /* Dieselbe Gruppe wie eine vorige Zeile? Sonst ein neuer Name, wenn Platz ist. */
            for (uint8_t g = 0; g < list->group_count; g++) {
                if (strcmp(list->groups[g], name) == 0) {
                    item->group = (uint8_t)(g + 1);
                    break;
                }
            }
            if (item->group == 0 && list->group_count < CARD_GROUPS_MAX && name[0]) {
                strlcpy(list->groups[list->group_count], name, CARD_GROUP_LEN);
                list->group_count++;
                item->group = list->group_count;
            }
        }
    }
    ui_status_unlock();
}

void ui_cards_commit(ui_card_kind_t kind)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    if (s_visible && s_kind == kind) {
        render_locked();
    }
    ui_status_unlock();
#else
    (void)kind;
#endif
}

void ui_cards_show(ui_card_kind_t kind)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (kind >= UI_CARD_COUNT) {
        kind = UI_CARD_TODAY;
    }
    ui_status_lock();
    create_locked();
    hide_undo_locked();
    clear_selection_locked();
    s_kind = kind;
    s_wpage = 0;
    s_visible = true;
    scroll_top_locked();
    render_locked();
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    ui_status_header_refresh_locked();
    ui_status_unlock();
#else
    (void)kind;
#endif
}

void ui_cards_hide(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    s_visible = false;
    hide_undo_locked();
    clear_selection_locked();
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_header_refresh_locked();
    ui_status_unlock();
#endif
}

bool ui_cards_visible(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_visible;
#else
    return false;
#endif
}

/*
 * Gesten auf den Karten -- Teil der einen Gestensprache der Uhr (Tabelle in
 * main.c, handle_touch):
 *
 *   links / rechts   naechste / vorige Karte (Rueckgaengig-Chip verschwindet)
 *   nach oben        naechster Ausschnitt der Liste; am Ende nichts
 *   nach unten       voriger Ausschnitt; am Anfang eine Ebene zurueck: Karten zu
 *   Tippen           Chip "Rueckgaengig" vor Zeile; Zeile abhaken/schalten --
 *                    auf Heute (und bei Schaltern mit Flag `c`) erst
 *                    markieren, dann "OK" (11/2026)
 *
 * Heute fragt nach: Ein Tippen auf eine Zeile markiert sie nur, erst der
 * OK-Chip in der Zeile hakt ab. Ein Fehltipp am Handgelenk -- beim Wischen,
 * am Aermel -- hakte vorher sofort eine Aufgabe ab (Wunsch des Nutzers).
 * Eine andere Zeile antippen markiert diese; daneben tippen, wischen, die
 * Karte wechseln oder schliessen, fuenf Sekunden warten oder eine neue Liste
 * der Bruecke verwerfen die Markierung, ohne etwas zu senden. Einkauf und
 * Schalter ohne Flag bleiben beim direkten Tippen: Dort ist der Fehlgriff
 * harmlos und mit "Rueckgaengig" schnell zurueck. Welche Schalter nachfragen,
 * stellt die App ein (`voice_switches[].confirm`, Flag `c` im `list`).
 *
 * Bis 0.3.6 waren die Listen in Seiten geteilt, "nach oben" lief im Kreis
 * und "nach unten" schloss von jeder Seite. Jetzt scrollt die Liste; am
 * oberen Ende bleibt "nach unten" die eine Ebene zurueck.
 */
ui_cards_action_t ui_cards_touch(int gesture, int16_t x, int16_t y, ui_cards_hit_t *hit)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_cards_action_t action = UI_CARDS_NONE;
    ui_status_lock();
    if (!s_visible) {
        ui_status_unlock();
        return UI_CARDS_NONE;
    }
    /* Jede Geste ausser Tippen verwirft die Markierung. */
    if ((voice_board_gesture_t)gesture != VOICE_BOARD_TAP && clear_selection_locked()) {
        render_locked();
    }
    if (s_kind == UI_CARD_WEATHER) {
        /*
         * Die Wetterkarte: nach oben die naechste Seite, nach unten die
         * vorige -- auf der ersten zurueck aufs Zifferblatt. Links und
         * rechts wie bei den anderen Karten, Tippen tut nichts.
         */
        switch ((voice_board_gesture_t)gesture) {
        case VOICE_BOARD_SWIPE_LEFT:
            action = UI_CARDS_END;
            break;
        case VOICE_BOARD_SWIPE_RIGHT:
            s_kind = (ui_card_kind_t)(s_kind - 1);
            scroll_top_locked();
            render_locked();
            break;
        case VOICE_BOARD_SWIPE_UP:
            if (s_wpage + 1 < ui_weather_page_count()) {
                s_wpage++;
                render_locked();
            }
            break;
        case VOICE_BOARD_SWIPE_DOWN:
            if (s_wpage == 0) {
                action = UI_CARDS_CLOSE;
            } else {
                s_wpage--;
                render_locked();
            }
            break;
        default:
            break;
        }
        ui_status_unlock();
        if (action == UI_CARDS_CLOSE) {
            ui_cards_hide();
        }
        return action;
    }
    card_list_t *list = &s_lists[s_kind];
    switch ((voice_board_gesture_t)gesture) {
    /*
     * Die Karten liegen rechts neben dem Zifferblatt in einer Reihe:
     * Zifferblatt | Heute | Einkauf | Zuhause | Wetter. Nach links wischen holt die
     * naechste Karte von rechts; am Ende der Reihe passiert nichts (kein
     * Ringlauf, sonst verliert man die Orientierung). Nach rechts geht es
     * zurueck, von Heute aus aufs Zifferblatt.
     */
    case VOICE_BOARD_SWIPE_LEFT:
        if (s_kind + 1 < UI_CARD_COUNT) {
            hide_undo_locked();
            s_kind = (ui_card_kind_t)(s_kind + 1);
            s_wpage = 0;
            scroll_top_locked();
            render_locked();
        } else {
            action = UI_CARDS_END;
        }
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        if (s_kind == 0) {
            action = UI_CARDS_CLOSE;
        } else {
            hide_undo_locked();
            s_kind = (ui_card_kind_t)(s_kind - 1);
            scroll_top_locked();
            render_locked();
        }
        break;
    case VOICE_BOARD_SWIPE_UP:
        (void)scroll_page_locked(1);
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        if (!scroll_page_locked(-1)) {
            action = UI_CARDS_CLOSE;
        }
        break;
    case VOICE_BOARD_TAP: {
        /*
         * Mit Toleranz trifft ein Finger zwischen zwei Zeilen beide; es
         * zaehlt die, deren Mitte naeher liegt. Zeilen ausserhalb des
         * Ausschnitts liegen mit ihren Koordinaten ausserhalb des Schirms
         * und treffen nicht.
         */
        int best = -1;
        int32_t best_distance = 0;
        for (int i = 0; i < list->count && i < ROWS_MAX; i++) {
            const int32_t distance = hit_distance(s_row[i], x, y);
            if (distance >= 0 && (best < 0 || distance < best_distance)) {
                best = i;
                best_distance = distance;
            }
        }
        if (s_sel.active) {
            /*
             * OK getroffen: Der Chip liegt in der markierten Zeile und hat
             * deren Mitte; eine Nachbarzeile gewinnt nur, wenn ihre Mitte
             * naeher liegt (Tippen im Zwischenraum).
             */
            const int32_t ok_distance = hit_distance(s_ok, x, y);
            if (ok_distance >= 0 && (best < 0 || ok_distance <= best_distance)) {
                const int index = s_sel.index;
                const bool valid = list->rev == s_sel.rev && index < list->count &&
                                   !list->items[index].event &&
                                   (s_kind == UI_CARD_SWITCHES || !list->items[index].done);
                clear_selection_locked();
                if (valid) {
                    action = check_off_locked(list, index, hit);
                } else {
                    render_locked();
                }
                break;
            }
        }
        if (s_last.armed && hit_distance(s_undo, x, y) >= 0) {
            /*
             * Rueckgaengig: Die Zeile zeigt sofort wieder den Stand vorher --
             * sofern die Liste noch dieselbe ist. Hat die Bruecke inzwischen
             * eine neue geschickt (`rev` anders), zieht der naechste Abgleich
             * die Karte ohnehin nach.
             */
            card_list_t *undo_list = &s_lists[s_last.kind];
            if (undo_list->rev == s_last.rev && s_last.index < undo_list->count) {
                undo_list->items[s_last.index].done = s_last.prev_done;
            }
            if (hit) {
                hit->kind = s_last.kind;
                hit->rev = s_last.rev;
                hit->index = s_last.index;
            }
            hide_undo_locked();
            action = UI_CARDS_UNDO;
            render_locked();
            break;
        }
        const int index = best;
        /*
         * Abhaken geht nur einmal -- ein zweites Tippen auf dieselbe
         * Zeile schickte sonst zweimal "erledigt". Schalter duerfen hin
         * und her.
         */
        /* Termine (Flag `e`) sind nur zum Lesen: kein Markieren, kein OK, kein `item`. */
        if (index < 0 || index >= list->count || list->items[index].event ||
            (list->items[index].done && s_kind != UI_CARD_SWITCHES)) {
            /* Daneben getippt: eine Markierung verfaellt, gesendet wird nichts. */
            if (clear_selection_locked()) {
                render_locked();
            }
            break;
        }
        if (s_kind == UI_CARD_SWITCHES && list->items[index].select) {
            /*
             * input_select (Flag `s`, 0.4.5): die Auswahl oeffnen statt
             * weiterschalten -- ohne OK-Schritt, auch bei `c`: Die Wahl in
             * der Liste ist selbst der zweite Schritt. Gesendet wird hier
             * noch nichts ausser der Bitte um die Optionen (main.c).
             */
            if (clear_selection_locked()) {
                render_locked();
            }
            if (hit) {
                hit->kind = s_kind;
                hit->rev = list->rev;
                hit->index = (uint8_t)index;
                strlcpy(hit->text, list->items[index].text, sizeof(hit->text));
                hit->binary = false;
                hit->on = false;
            }
            action = UI_CARDS_OPTIONS;
            break;
        }
        if (needs_confirm(list, index)) {
            /*
             * Nur markieren (oder die Markierung auf diese Zeile legen).
             * Fuenf Sekunden wartet der OK-Chip, jedes neue Tippen auf eine
             * Zeile startet sie neu. Ein noch stehendes "Rueckgaengig"
             * weicht -- sonst laegen zwei Chips uebereinander.
             */
            clear_selection_locked();
            hide_undo_locked();
            s_sel.active = true;
            s_sel.rev = list->rev;
            s_sel.index = (uint8_t)index;
            s_sel_timer = lv_timer_create(sel_expired_cb, OK_MS, NULL);
            lv_timer_set_repeat_count(s_sel_timer, 1);
            char ok_text[24];
            /* Die Schriften haben keinen Halbgeviertstrich; die Kopie macht "-" daraus. */
            ui_status_copy_display_text(ok_text, s_kind == UI_CARD_SWITCHES ? "OK – schalten"
                                                                            : "OK – erledigt",
                                        sizeof(ok_text));
            lv_label_set_text(s_ok_label, ok_text);
            if (hit) {
                hit->kind = s_kind;
                hit->rev = list->rev;
                hit->index = (uint8_t)index;
            }
            action = UI_CARDS_SELECT;
            render_locked();
            break;
        }
        action = check_off_locked(list, index, hit);
        break;
    }
    default:
        break;
    }
    ui_status_unlock();
    if (action == UI_CARDS_CLOSE) {
        ui_cards_hide();
    }
    ESP_LOGD(TAG, "touch %d -> %d", gesture, (int)action);
    return action;
#else
    (void)gesture;
    (void)x;
    (void)y;
    (void)hit;
    return UI_CARDS_NONE;
#endif
}

void ui_cards_restore(ui_card_kind_t kind, uint16_t rev, uint8_t index)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (kind >= UI_CARD_LIST_COUNT) {
        return;
    }
    ui_status_lock();
    card_list_t *list = &s_lists[kind];
    if (list->rev == rev && index < list->count) {
        list->items[index].done = kind == UI_CARD_SWITCHES ? !list->items[index].done : false;
        if (s_visible && s_kind == kind) {
            render_locked();
        }
    }
    ui_status_unlock();
#else
    (void)kind;
    (void)rev;
    (void)index;
#endif
}

void ui_cards_mark_sent(ui_card_kind_t kind, uint16_t rev, uint8_t index)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (kind >= UI_CARD_LIST_COUNT) {
        return;
    }
    ui_status_lock();
    card_list_t *list = &s_lists[kind];
    if (list->rev == rev && index < list->count) {
        /*
         * Wie check_off_locked(), nur ohne Umschalten: `done` heisst bei
         * einem Wort statt An/Aus "..." bis zur neuen Karte. Der Chip steht
         * nur, wenn die Karte zu sehen ist -- sonst bleibt der Verlauf.
         */
        card_item_t *item = &list->items[index];
        hide_undo_locked();
        s_last.kind = kind;
        s_last.rev = rev;
        s_last.index = index;
        s_last.prev_done = item->done;
        item->done = true;
        if (s_visible && s_kind == kind && s_undo) {
            s_last.armed = true;
            s_undo_timer = lv_timer_create(undo_expired_cb, UNDO_MS, NULL);
            lv_timer_set_repeat_count(s_undo_timer, 1);
            lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
            render_locked();
        }
    }
    ui_status_unlock();
#else
    (void)kind;
    (void)rev;
    (void)index;
#endif
}

/* Neue Schriftgroesse: Zeilenhoehe und Schrift aendern sich mit. */
void ui_cards_refresh(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    if (s_visible) {
        render_locked();
    }
    ui_status_unlock();
#endif
}
