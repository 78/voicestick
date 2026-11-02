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

/*
 * Welche Art Geraet hinter einer Schalterzeile steht (Paket J, 0.5.0): die
 * Flags `l` light, `w` switch/input_boolean, `f` fan, `k` lock im `list`.
 * Die Uhr zeichnet daraus ein Symbol; ohne Flag (aeltere Bruecke) bleibt es
 * beim Wort "An"/"Aus".
 */
typedef enum {
    CARD_DOMAIN_NONE,
    CARD_DOMAIN_LIGHT,
    CARD_DOMAIN_SWITCH,
    CARD_DOMAIN_FAN,
    CARD_DOMAIN_LOCK,
} card_domain_t;

typedef struct {
    char text[UI_CARD_TEXT_LEN];
    char aux[UI_CARD_AUX_LEN];
    uint32_t colour;  /* Projektfarbe (nur `today`), CARD_COLOUR_SET wenn gesetzt */
    uint8_t group;    /* 1..CARD_GROUPS_MAX in `groups`, 0 = keine */
    bool confirm;     /* Schalter: erst markieren, dann "OK - schalten" (Flag `c`) */
    bool event;       /* Termin (Flag `e`): nicht antippbar, nur zum Lesen */
    bool select;      /* input_select (Flag `s`, 0.4.5): Tippen oeffnet die Auswahl */
    uint8_t domain;   /* card_domain_t aus `l`/`w`/`f`/`k` (0.5.0), nur `switches` */
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
#include "ui_glyph.h"
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
 * "zurueck". Die Seitenpunkte unten zeigen die Lage, ein Punkt je Ausschnitt
 * -- dieselben wie bei der Wetterkarte (0.3.6 bis 0.4.5 war es ein Balken). Der Finger steuert den Scroll nicht selbst: Die
 * Touch-Schicht meldet Gesten erst beim Abheben, LVGL kennt kein
 * Eingabegeraet; der Schwung ist die Animation des Sprungs.
 *
 * Zwischen zwei Gruppen (`group` je Eintrag, nur Heute) steht eine
 * Trennzeile: duenner Text in der Meta-Stufe, 28 px hoch, kein Trefferziel.
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
/* Symbol links in einer Schalterzeile (Flags `l`/`w`/`f`/`k`/`s`, 0.5.0). */
#define ROW_ICON 28
#define ROW_ICON_X 14
/* Kopfzeile: "‹" vor dem Titel, "›" rechts (Paket J); gezeichnet, die Schriften kennen sie nicht. */
#define HEAD_ARROW 20
#define HEAD_ARROW_X 22
#define HEAD_TITLE_X (HEAD_ARROW_X + HEAD_ARROW - 2)

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
/* Innenabstand links/rechts der "OK"-Pille in der Zeile. */
#define PILL_PAD 8
/* Teckis Flaeche auf der leeren Karte. */
#define EMPTY_TECKI_BOX 96
#define EMPTY_TECKI_Y 110

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_head_prev;
static lv_obj_t *s_head_next;
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
static lv_obj_t *s_row_icon[ROWS_MAX];
static lv_obj_t *s_row_pill[ROWS_MAX];
static lv_obj_t *s_sep[SEPS_MAX];
static lv_obj_t *s_undo;
static lv_obj_t *s_undo_label;
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

/* Zeilen in der Inhalt-Stufe der Typo-Skala (klein 20, normal 23, gross 26). */
static const lv_font_t *row_font(void)
{
    return UI_TYPO(UI_TYPO_BODY);
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
     * Kopfzeile (Paket J): "‹ Heute" links, "›" rechts -- der Pfeil steht
     * nur, wo es weitergeht; links von Heute liegt das Zifferblatt, dort
     * steht er also immer. Bis 0.4.6 sagten Punkte oben rechts, welche Karte
     * es ist, und unten stand eine zweite Punktreihe fuer die Lage in der
     * Liste -- zwei gleiche Zeichen fuer zwei verschiedene Dinge. Punkte
     * gibt es jetzt nur noch unten.
     *
     * Der Titel steht in der Titel-Stufe (hoechstens 26 px): Die kleine
     * Uhrzeit in der Kopfzeile beginnt bei x 163 im Inhalt, "Zuhause" in der
     * 26er endet bei 40 + 114 = 154. Titel und Pfeile stehen auf derselben
     * waagerechten Mitte wie die Uhrzeit (UI_SCREEN_PAD + UI_HEADER_LABEL_Y
     * in der 18er auf lv_layer_top(); die Karte liegt schon im Inhalt);
     * render_locked() setzt die Lage nach der Schriftgroesse.
     */
    const int32_t header_mid = UI_HEADER_LABEL_Y + lv_font_get_line_height(UI_FONT_SMALL) / 2;
    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x2ecc71), 0);
    s_head_prev = ui_glyph_create(s_root, HEAD_ARROW);
    lv_obj_set_pos(s_head_prev, HEAD_ARROW_X, header_mid - HEAD_ARROW / 2);
    s_head_next = ui_glyph_create(s_root, HEAD_ARROW);
    lv_obj_align(s_head_next, LV_ALIGN_TOP_RIGHT, -UI_HEADER_INSET_X, header_mid - HEAD_ARROW / 2);

    ui_dots_create(&s_page_dots, s_root);
    lv_obj_align(s_page_dots.box, LV_ALIGN_TOP_MID, 0, CONTENT_H - 16);

    ui_status_icons_create(&s_tecki, s_root);
    ui_status_icons_show(&s_tecki, false);
    ui_weather_create_locked(s_root);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, CONTENT_W - 2 * UI_HEADER_INSET_X);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, EMPTY_TECKI_Y + EMPTY_TECKI_BOX + 16);

    /*
     * Der scrollende Ausschnitt: nur senkrecht, Rastung an den Oberkanten
     * der Kinder, keine eigene Leiste (die Seitenpunkte stehen unten), kein elastisches
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
        lv_obj_align(s_row_aux[i], LV_ALIGN_RIGHT_MID, -16, 0);

        /* Geraete-Symbol links (nur Zuhause, mit Flag), sonst versteckt. */
        s_row_icon[i] = ui_glyph_create(s_row[i], ROW_ICON);
        lv_obj_align(s_row_icon[i], LV_ALIGN_LEFT_MID, ROW_ICON_X, 0);

        /*
         * "OK"-Pille (Paket J): sagt vor dem Tippen, dass ein zweiter
         * Schritt kommt. Grau, eine Zeile hoch, rechts vor der Zusatzangabe.
         */
        s_row_pill[i] = lv_label_create(s_row[i]);
        lv_label_set_text(s_row_pill[i], "OK");
        lv_obj_set_style_bg_opa(s_row_pill[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row_pill[i], lv_color_hex(0x3a4656), 0);
        lv_obj_set_style_text_color(s_row_pill[i], lv_color_hex(0xc9d1d9), 0);
        lv_obj_set_style_radius(s_row_pill[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(s_row_pill[i], PILL_PAD, 0);
        lv_obj_set_style_pad_ver(s_row_pill[i], 1, 0);
        lv_obj_add_flag(s_row_pill[i], LV_OBJ_FLAG_HIDDEN);
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
    s_undo_label = undo_label;
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

/*
 * Lage in der Liste: dieselben Seitenpunkte unten wie bei der Wetterkarte
 * (11/2026, vorher ein schmaler Balken -- zwei Zeichen fuer dasselbe "hier
 * geht es weiter"). Eine Seite ist ein Ausschnitt; hoch/runter bewegt die
 * Liste um genau einen (scroll_view_locked), also passt ein Punkt je Ausschnitt.
 * Am Ende der Liste ist immer der letzte Punkt aktiv, auch wenn der letzte
 * Ausschnitt nur teilweise voll ist.
 */
static void update_bar_locked(int32_t scroll_y)
{
    const int32_t view = view_h();
    if (view <= 0 || s_content_h <= view) {
        ui_dots_set(&s_page_dots, 0, 0);
        return;
    }
    const int32_t max_y = s_content_h - view;
    int32_t pages = (max_y + view - 1) / view + 1;
    if (pages > UI_DOTS_MAX) {
        pages = UI_DOTS_MAX;
    }
    if (scroll_y < 0) {
        scroll_y = 0;
    }
    int32_t page = scroll_y >= max_y ? pages - 1 : (scroll_y + view / 2) / view;
    if (page > pages - 1) {
        page = pages - 1;
    }
    ui_dots_set(&s_page_dots, (uint8_t)pages, (uint8_t)page);
}

/* Die Wetterkarte: keine Liste, eigene Seiten, Tecki nur ohne Daten. */
static void render_weather_locked(void)
{
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
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
        voice_board_haptic_kind(VOICE_HAPTIC_REMINDER);
    }
}

/*
 * Kopfzeile und die Schriften, die der Typo-Skala folgen -- vor jedem
 * Zeichnen, weil `watch_settings.font` sich jederzeit aendern kann.
 */
static void render_head_locked(void)
{
    const lv_font_t *title = UI_TYPO(UI_TYPO_TITLE);
    const int32_t header_mid = UI_HEADER_LABEL_Y + lv_font_get_line_height(UI_FONT_SMALL) / 2;
    lv_label_set_text(s_title, TITLES[s_kind]);
    lv_obj_set_style_text_font(s_title, title, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, HEAD_TITLE_X,
                 header_mid - lv_font_get_line_height(title) / 2);
    ui_glyph_set(s_head_prev, UI_GLYPH_BACK, true, 0x9aa3ad);
    ui_glyph_set(s_head_next, s_kind + 1 < UI_CARD_COUNT ? UI_GLYPH_FORWARD : UI_GLYPH_NONE, true,
                 0x9aa3ad);

    const lv_font_t *meta = UI_TYPO(UI_TYPO_META);
    lv_obj_set_style_text_font(s_empty, UI_TYPO(UI_TYPO_BODY), 0);
    lv_obj_set_style_text_font(s_undo_label, UI_TYPO(UI_TYPO_BODY), 0);
    lv_obj_set_style_text_font(s_ok_label, meta, 0);
    for (int i = 0; i < SEPS_MAX; i++) {
        lv_obj_set_style_text_font(s_sep[i], meta, 0);
    }
    for (int i = 0; i < ROWS_MAX; i++) {
        lv_obj_set_style_text_font(s_row_aux[i], meta, 0);
        lv_obj_set_style_text_font(s_row_pill[i], meta, 0);
    }
}

/* Lock: offen? Die Bruecke schickt "Offen"/"Zu" (0.4.3) -- oder "1"/"0". */
static bool aux_is_open(const char *aux)
{
    return strcmp(aux, "1") == 0 || strncmp(aux, "Offen", 5) == 0 || strncmp(aux, "offen", 5) == 0;
}

/* Symbol einer Schalterzeile aus Flag und Zustand; UI_GLYPH_NONE ohne Flag. */
static ui_glyph_t row_glyph(const card_item_t *item, bool *on, uint32_t *colour)
{
    *on = false;
    *colour = 0x2ecc71;
    if (s_kind != UI_CARD_SWITCHES) {
        return UI_GLYPH_NONE;
    }
    if (item->select) {
        *on = true;
        *colour = 0xc9d1d9;
        return UI_GLYPH_LIST;
    }
    const bool binary = aux_is_binary(item->aux);
    /* Angetippt zeigt eine An/Aus-Zeile den Zielzustand, wie vorher das Wort. */
    const bool state = binary ? (strcmp(item->aux, "1") == 0) != item->done : false;
    switch ((card_domain_t)item->domain) {
    case CARD_DOMAIN_LIGHT:
        *on = state;
        *colour = 0xf2b23c;
        return UI_GLYPH_LIGHT;
    case CARD_DOMAIN_SWITCH:
        *on = state;
        return UI_GLYPH_SWITCH;
    case CARD_DOMAIN_FAN:
        *on = state;
        *colour = 0x5ec4ff;
        return UI_GLYPH_FAN;
    case CARD_DOMAIN_LOCK:
        /* Angetippt kennt die Uhr den Zielstand nicht: grau, bis die neue Karte kommt. */
        *on = !item->done && aux_is_open(item->aux);
        *colour = 0xf2b23c;
        return UI_GLYPH_LOCK;
    case CARD_DOMAIN_NONE:
        break;
    }
    return UI_GLYPH_NONE;
}

static void render_locked(void)
{
    if (!s_root) {
        return;
    }
    render_head_locked();
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
                         y + (SEP_H - lv_font_get_line_height(UI_TYPO(UI_TYPO_META))) / 2);
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
        bool glyph_on = false;
        uint32_t glyph_colour = 0;
        const ui_glyph_t glyph = row_glyph(item, &glyph_on, &glyph_colour);
        const int32_t text_x = glyph != UI_GLYPH_NONE ? ROW_ICON_X + ROW_ICON + 10 :
                               striped ? 16 + STRIPE_W + 8 : 16;
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
        ui_glyph_set(s_row_icon[i], glyph, glyph_on, glyph_colour);

        /*
         * Die Zusatzangabe rechts. Mit Geraete-Symbol (Flags `l`/`w`/`f`/`k`,
         * 0.5.0) traegt das Symbol den Zustand, das Wort entfaellt -- ausser
         * "..." nach dem Tippen auf ein Schloss, dessen Zielstand die Uhr
         * nicht kennt. Die Auswahl (`s`) zeigt ihre Option weiter als Wort.
         */
        const char *aux_text = item->aux;
        uint32_t aux_colour = 0x9aa3ad;
        bool aux_shown = true;
        if (s_kind == UI_CARD_SWITCHES && glyph != UI_GLYPH_NONE && glyph != UI_GLYPH_LIST) {
            aux_shown = item->domain == CARD_DOMAIN_LOCK && item->done;
            aux_text = "...";
        } else if (s_kind == UI_CARD_SWITCHES && aux_is_binary(item->aux)) {
            /* Schalter ohne Flag: An/Aus als farbiges Wort; angetippt zeigt es den Zielzustand. */
            bool on = strcmp(item->aux, "1") == 0;
            if (item->done) {
                on = !on;
            }
            aux_text = on ? "An" : "Aus";
            aux_colour = on ? 0x2ecc71 : 0x9aa3ad;
        } else if (s_kind == UI_CARD_SWITCHES) {
            /*
             * Ein Wort statt An/Aus (0.4.3): die Option eines input_select
             * oder "Offen"/"Zu" eines Schlosses. Den Zielstand kennt die Uhr
             * nicht -- angetippt steht "..." bis die Bruecke die Karte neu
             * schickt (sie tut das nach jeder Aktion).
             */
            aux_text = item->done ? "..." : item->aux;
            aux_colour = item->done ? 0x9aa3ad : 0x2ecc71;
        } else {
            /* Ueberfaellig (Datum statt Uhrzeit): rot. */
            aux_colour = is_date_aux(item->aux) ? 0xe74c3c : 0x9aa3ad;
            aux_shown = item->aux[0] != '\0';
        }
        /* Unter dem OK-Chip laege die Zusatzangabe nur verdeckt. */
        aux_shown = aux_shown && !selected;
        lv_label_set_text(s_row_aux[i], aux_text);
        lv_obj_set_style_text_color(s_row_aux[i], lv_color_hex(aux_colour), 0);
        const lv_font_t *meta = UI_TYPO(UI_TYPO_META);
        const int32_t aux_w = aux_shown ? lv_text_get_width(aux_text, (uint32_t)strlen(aux_text),
                                                            meta, 0) : 0;
        if (aux_shown) {
            lv_obj_remove_flag(s_row_aux[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_row_aux[i], LV_OBJ_FLAG_HIDDEN);
        }

        /*
         * Die "OK"-Pille (Paket J): Folgenreiches braucht OK, alles andere
         * wirkt sofort -- und das soll man vor dem Tippen sehen. Sie steht
         * an jeder Zeile, die markiert statt ausgefuehrt wird (Heute,
         * Schalter mit `c`, Schloesser), solange sie nicht schon abgehakt
         * oder gerade markiert ist (dann steht dort der gruene Chip).
         */
        const bool pill = !selected && !item->event && !item->select &&
                          needs_confirm(list, i) &&
                          (s_kind == UI_CARD_SWITCHES || !item->done);
        int32_t right = 16 + (aux_shown ? aux_w + 8 : 0);
        if (pill) {
            lv_obj_align(s_row_pill[i], LV_ALIGN_RIGHT_MID, -right, 0);
            lv_obj_remove_flag(s_row_pill[i], LV_OBJ_FLAG_HIDDEN);
            right += lv_text_get_width("OK", 2, meta, 0) + 2 * PILL_PAD + 8;
        } else {
            lv_obj_add_flag(s_row_pill[i], LV_OBJ_FLAG_HIDDEN);
        }

        /*
         * Breite des Titels: bis zum OK-Chip, wenn die Zeile markiert ist;
         * sonst bis zu Pille und Zusatzangabe rechts (gemessen, seit 0.5.0
         * nicht mehr pauschal 78 px). Hoehe: eine Textzeile, wenn der Titel
         * in eine passt (dann steht er mittig), sonst zwei -- LONG_DOT
         * kuerzt nach der zweiten mit "...".
         */
        const int32_t text_w = (selected ? CONTENT_W - OK_W - 8 : CONTENT_W - right) - text_x;
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
    /* Chip und Seitenpunkte folgen sofort dem Ziel, nicht der Animation. */
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
        /*
         * Schloesser (`k`) fragen immer nach -- auch wenn eine Bruecke das
         * `c` vergisst: Eine offene Haustuer ist kein Fehltipp, den man mit
         * "Rueckgaengig" wieder einfaengt (Paket J).
         */
        item->confirm = !item->event && flags &&
                        (strchr(flags, 'c') != NULL ||
                         (kind == UI_CARD_SWITCHES && strchr(flags, 'k') != NULL));
        item->select = kind == UI_CARD_SWITCHES && !item->event && flags &&
                       strchr(flags, 's') != NULL;
        item->domain = CARD_DOMAIN_NONE;
        if (kind == UI_CARD_SWITCHES && flags) {
            item->domain = strchr(flags, 'k') ? CARD_DOMAIN_LOCK :
                           strchr(flags, 'l') ? CARD_DOMAIN_LIGHT :
                           strchr(flags, 'f') ? CARD_DOMAIN_FAN :
                           strchr(flags, 'w') ? CARD_DOMAIN_SWITCH : CARD_DOMAIN_NONE;
        }
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
 *   links / rechts   naechste / vorige Karte (Rueckgaengig-Chip verschwindet);
 *                    rechts von Heute aus: Karten zu, aufs Zifferblatt
 *   nach oben        naechster Ausschnitt der Liste; am Ende nichts
 *   nach unten       voriger Ausschnitt; am Anfang nichts (0.5.0)
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
 * und "nach unten" schloss von jeder Seite. Von 0.4.0 bis 0.4.6 scrollte
 * die Liste, und am oberen Ende war "nach unten" die eine Ebene zurueck --
 * dieselbe Geste tat also je nach Lage zweierlei. Seit Paket J (0.5.0)
 * gilt eine Regel: rechts ist zurueck, oben/unten scrollt nur.
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
         * vorige -- auf der ersten nichts (seit 0.5.0; vorher zurueck aufs
         * Zifferblatt). Links und rechts wie bei den anderen Karten, Tippen
         * tut nichts.
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
            if (s_wpage > 0) {
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
        /* Am Anfang der Liste: nichts (Paket J) -- zurueck geht es nach rechts. */
        (void)scroll_page_locked(-1);
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
