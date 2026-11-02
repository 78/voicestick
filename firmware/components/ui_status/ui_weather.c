#include "ui_weather.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "ui_cards.h"
#include "ui_status_internal.h"

/*
 * Der Stand, wie die Bruecke ihn schickt. Alle Texte sind schon auf
 * hoechstens 40 Byte gekuerzt (Vertrag); die Puffer hier liegen knapp
 * darueber, der ganze Satz misst rund 800 Byte und steht statisch im
 * internen RAM -- weniger als eine der drei Listen.
 */
#define W_TEXT_LEN 41
#define W_HOURS 6
#define W_DAYS 5
#define W_POLLEN 4
#define W_WARNINGS 3

typedef struct {
    uint16_t rev;
    bool received;
    /* Wann Teil 0 kam (UTC-Sekunden, 0 = nie) -- fuer das Alter in der Kopfzeile (0.5.0). */
    int64_t received_at;
    char now[W_TEXT_LEN];
    char today[W_TEXT_LEN];
    char rain[W_TEXT_LEN];
    char sun[24];
    char wind[24];
    char stale[8];
    uint8_t hour_count;
    char hour_label[W_HOURS][6];
    char hour_temp[W_HOURS][8];
    bool hour_rain[W_HOURS];
    uint8_t day_count;
    char day_name[W_DAYS][4];
    char day_temps[W_DAYS][16];
    char day_cond[W_DAYS][24];
    uint8_t pollen_count;
    char pollen[W_POLLEN][32];
    uint8_t warning_count;
    char warnings[W_WARNINGS][W_TEXT_LEN];
} weather_t;

static weather_t s_w;
/* Die zuletzt mit Vibration angezeigte Warnung -- eine neue vibriert einmal. */
static char s_warned[W_TEXT_LEN];

void ui_weather_begin(uint16_t rev, const char *now, const char *today, const char *rain,
                      const char *sun, const char *wind, const char *stale)
{
    ui_status_lock();
    memset(&s_w, 0, sizeof(s_w));
    s_w.rev = rev;
    s_w.received = true;
    s_w.received_at = (int64_t)time(NULL);
    ui_status_copy_display_text(s_w.now, now ? now : "", sizeof(s_w.now));
    ui_status_copy_display_text(s_w.today, today ? today : "", sizeof(s_w.today));
    ui_status_copy_display_text(s_w.rain, rain ? rain : "", sizeof(s_w.rain));
    ui_status_copy_display_text(s_w.sun, sun ? sun : "", sizeof(s_w.sun));
    ui_status_copy_display_text(s_w.wind, wind ? wind : "", sizeof(s_w.wind));
    ui_status_copy_display_text(s_w.stale, stale ? stale : "", sizeof(s_w.stale));
    ui_status_unlock();
}

void ui_weather_add_hour(const char *label, const char *temp, bool rain)
{
    ui_status_lock();
    if (s_w.hour_count < W_HOURS) {
        const uint8_t i = s_w.hour_count++;
        ui_status_copy_display_text(s_w.hour_label[i], label ? label : "", sizeof(s_w.hour_label[i]));
        ui_status_copy_display_text(s_w.hour_temp[i], temp ? temp : "", sizeof(s_w.hour_temp[i]));
        s_w.hour_rain[i] = rain;
    }
    ui_status_unlock();
}

void ui_weather_add_day(const char *day, const char *temps, const char *condition)
{
    ui_status_lock();
    if (s_w.day_count < W_DAYS) {
        const uint8_t i = s_w.day_count++;
        ui_status_copy_display_text(s_w.day_name[i], day ? day : "", sizeof(s_w.day_name[i]));
        ui_status_copy_display_text(s_w.day_temps[i], temps ? temps : "", sizeof(s_w.day_temps[i]));
        ui_status_copy_display_text(s_w.day_cond[i], condition ? condition : "",
                                    sizeof(s_w.day_cond[i]));
    }
    ui_status_unlock();
}

void ui_weather_add_pollen(const char *text)
{
    ui_status_lock();
    if (s_w.pollen_count < W_POLLEN && text && text[0]) {
        ui_status_copy_display_text(s_w.pollen[s_w.pollen_count], text,
                                    sizeof(s_w.pollen[s_w.pollen_count]));
        s_w.pollen_count++;
    }
    ui_status_unlock();
}

void ui_weather_add_warning(const char *text)
{
    ui_status_lock();
    if (s_w.warning_count < W_WARNINGS && text && text[0]) {
        ui_status_copy_display_text(s_w.warnings[s_w.warning_count], text,
                                    sizeof(s_w.warnings[s_w.warning_count]));
        s_w.warning_count++;
    }
    ui_status_unlock();
}

void ui_weather_commit(void)
{
    /* Die Karte zeichnet, falls sie gerade zu sehen ist -- auch die Wetterseite. */
    ui_cards_refresh();
}

uint8_t ui_weather_page_count(void)
{
    if (!s_w.now[0]) {
        return 1;
    }
    return (s_w.pollen_count > 0 || s_w.warning_count > 0) ? 3 : 2;
}

int64_t ui_weather_received_at(void)
{
    return s_w.received ? s_w.received_at : 0;
}

uint16_t ui_weather_rev(void)
{
    return s_w.rev;
}

void ui_weather_clear_locked(void)
{
    memset(&s_w, 0, sizeof(s_w));
}

bool ui_weather_received(void)
{
    return s_w.received;
}

bool ui_weather_has_data(void)
{
    return s_w.now[0] != '\0';
}

bool ui_weather_take_new_warning_locked(void)
{
    if (s_w.warning_count == 0) {
        s_warned[0] = '\0';
        return false;
    }
    if (strcmp(s_warned, s_w.warnings[0]) == 0) {
        return false;
    }
    strlcpy(s_warned, s_w.warnings[0], sizeof(s_warned));
    return true;
}

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"

/*
 * Schriften aus der Typo-Skala (Paket J): Kopfzeile, Regen, Stunden,
 * Wetterwort und Sonne/Wind in Meta, der Tag und die Zeilen der Folgeseiten
 * in Inhalt. Die Stundenzellen (115 px breit) und die Spalten der Tage sind
 * auf hoechstens 20 bzw. 26 px ausgelegt -- Meta wird nie groesser als 20,
 * Inhalt nie groesser als 26. Ausgenommen ist nur die Temperatur "jetzt" in
 * der 30er: eine Anzeige-Ziffer wie die Uhrzeit.
 */
#define W_META UI_TYPO(UI_TYPO_META)
#define W_BODY UI_TYPO(UI_TYPO_BODY)

/*
 * Ein Vorrat an Labels, je Seite anders belegt (Schrift, Farbe, Lage, Text
 * setzt render). Seite 1 braucht 11, Seite 2 fuenf Zeilen zu dreien, Seite 3
 * bis zu sieben -- 16 Labels reichen fuer jede, und mehr Objekte kosten
 * internen RAM, den die Aufnahme braucht.
 */
#define W_LINES 16
#define W_TOP (HEADER_H + 22)
#define W_BOTTOM (CONTENT_H - 24)

static lv_obj_t *s_head;      /* grau: "Stand 08:30", "Naechste Tage", "Pollen & Warnungen" */
static lv_obj_t *s_warn;      /* rot: die erste Warnung unter dem Titel (Seite 1) */
static lv_obj_t *s_line[W_LINES];

/* Regenblau, wie es die Spezifikation nennt. */
#define BLUE lv_color_hex(0x4aa3df)

void ui_weather_create_locked(lv_obj_t *parent)
{
    if (s_head) {
        return;
    }
    s_head = lv_label_create(parent);
    lv_obj_set_style_text_color(s_head, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(s_head, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_head, CONTENT_W - 32);
    lv_obj_set_style_text_align(s_head, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_head, LV_OBJ_FLAG_HIDDEN);

    s_warn = lv_label_create(parent);
    lv_obj_set_style_text_color(s_warn, lv_color_hex(0xe74c3c), 0);
    lv_label_set_long_mode(s_warn, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_warn, CONTENT_W - 32);
    lv_obj_set_style_text_align(s_warn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_warn, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < W_LINES; i++) {
        s_line[i] = lv_label_create(parent);
        lv_label_set_long_mode(s_line[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_line[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Ein Label belegen: Text, Schrift, Farbe, Lage (x, y, Breite) und Ausrichtung. */
static void put(int i, const char *text, const lv_font_t *font, lv_color_t colour, int32_t x,
                int32_t y, int32_t w, lv_text_align_t align)
{
    lv_obj_t *l = s_line[i];
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
}

/* Enthaelt der Text ein Regen-Wort? Faerbt die Zeile blau. */
static bool rainy(const char *text)
{
    return strstr(text, "Regen") || strstr(text, "regen") || strstr(text, "Schauer") ||
           strstr(text, "Gewitter") || strstr(text, "Schnee");
}

static void render_page_now(void)
{
    const lv_color_t white = lv_color_hex(0xf2f2f2);
    const lv_color_t grey = lv_color_hex(0x9aa3ad);
    int32_t y = W_TOP;
    int used = 0;
    if (s_w.warning_count > 0) {
        lv_label_set_text(s_warn, s_w.warnings[0]);
        lv_obj_align(s_warn, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_remove_flag(s_warn, LV_OBJ_FLAG_HIDDEN);
        y += lv_font_get_line_height(W_BODY) + 4;
    }
    if (s_w.stale[0]) {
        char stand[24];
        snprintf(stand, sizeof(stand), "Stand %s", s_w.stale);
        lv_label_set_text(s_head, stand);
        lv_obj_align(s_head, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_remove_flag(s_head, LV_OBJ_FLAG_HIDDEN);
        y += lv_font_get_line_height(W_META) + 4;
    }
    y += 6;
    put(used++, s_w.now, &todoteck_watch_font_30, white, 0, y, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    y += lv_font_get_line_height(&todoteck_watch_font_30) + 6;
    put(used++, s_w.today, W_BODY, white, 0, y, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    y += lv_font_get_line_height(W_BODY) + 6;
    put(used++, s_w.rain, W_META, rainy(s_w.rain) ? BLUE : grey, 0, y, CONTENT_W,
        LV_TEXT_ALIGN_CENTER);
    y += lv_font_get_line_height(W_META) + 14;
    /* Stundenreihe 2 x 3: "12h 11°", Regenstunden blau. */
    const int32_t cell_w = (CONTENT_W - 32) / 3;
    for (int i = 0; i < s_w.hour_count && i < W_HOURS; i++) {
        char cell[16];
        snprintf(cell, sizeof(cell), "%s %s", s_w.hour_label[i], s_w.hour_temp[i]);
        put(used++, cell, W_META, s_w.hour_rain[i] ? BLUE : white,
            16 + (i % 3) * cell_w, y + (i / 3) * (lv_font_get_line_height(W_META) + 6),
            cell_w, LV_TEXT_ALIGN_CENTER);
    }
    if (s_w.hour_count > 0) {
        y += ((s_w.hour_count + 2) / 3) * (lv_font_get_line_height(W_META) + 6) + 12;
    }
    /* Sonne und Wind unten, klein und grau. */
    const int32_t small_h = lv_font_get_line_height(W_META);
    int32_t bottom = W_BOTTOM - small_h;
    if (s_w.wind[0]) {
        char wind[32];
        snprintf(wind, sizeof(wind), "Wind %s", s_w.wind);
        put(used++, wind, W_META, grey, 0, bottom > y ? bottom : y, CONTENT_W,
            LV_TEXT_ALIGN_CENTER);
        bottom -= small_h + 4;
    }
    if (s_w.sun[0]) {
        char sun[32];
        snprintf(sun, sizeof(sun), "Sonne %s", s_w.sun);
        put(used++, sun, W_META, grey, 0, bottom > y ? bottom : y, CONTENT_W,
            LV_TEXT_ALIGN_CENTER);
    }
}

static void render_page_days(void)
{
    const lv_color_t white = lv_color_hex(0xf2f2f2);
    const lv_color_t green = lv_color_hex(0x2ecc71);
    lv_label_set_text(s_head, "Nächste Tage");
    lv_obj_align(s_head, LV_ALIGN_TOP_MID, 0, W_TOP);
    lv_obj_remove_flag(s_head, LV_OBJ_FLAG_HIDDEN);
    const int32_t top = W_TOP + lv_font_get_line_height(W_META) + 12;
    const int32_t pitch = (W_BOTTOM - top) / W_DAYS;
    const int32_t line_h = lv_font_get_line_height(W_BODY);
    int used = 0;
    for (int i = 0; i < s_w.day_count && i < W_DAYS; i++) {
        const int32_t y = top + i * pitch + (pitch - line_h) / 2;
        /* Tag gruen, Temperaturen weiss, Wetter rechts -- blau bei Regen. */
        put(used++, s_w.day_name[i], W_BODY, green, 16, y, 46, LV_TEXT_ALIGN_LEFT);
        put(used++, s_w.day_temps[i], W_BODY, white, 62, y, 130, LV_TEXT_ALIGN_LEFT);
        put(used++, s_w.day_cond[i], W_META,
            rainy(s_w.day_cond[i]) ? BLUE : lv_color_hex(0xc9d1d9), 192, y + 2, CONTENT_W - 192 - 16,
            LV_TEXT_ALIGN_RIGHT);
    }
}

static void render_page_pollen(void)
{
    lv_label_set_text(s_head, "Pollen & Warnungen");
    lv_obj_align(s_head, LV_ALIGN_TOP_MID, 0, W_TOP);
    lv_obj_remove_flag(s_head, LV_OBJ_FLAG_HIDDEN);
    int32_t y = W_TOP + lv_font_get_line_height(W_META) + 12;
    const int32_t pitch = lv_font_get_line_height(W_BODY) + 10;
    int used = 0;
    for (int i = 0; i < s_w.pollen_count && i < W_POLLEN; i++) {
        put(used++, s_w.pollen[i], W_BODY, lv_color_hex(0xf2f2f2), 16, y,
            CONTENT_W - 32, LV_TEXT_ALIGN_LEFT);
        y += pitch;
    }
    if (s_w.pollen_count > 0 && s_w.warning_count > 0) {
        y += 10;
    }
    for (int i = 0; i < s_w.warning_count && i < W_WARNINGS; i++) {
        put(used++, s_w.warnings[i], W_BODY, lv_color_hex(0xe74c3c), 16, y,
            CONTENT_W - 32, LV_TEXT_ALIGN_LEFT);
        y += pitch;
    }
}

void ui_weather_render_locked(uint8_t page, bool visible)
{
    if (!s_head) {
        return;
    }
    lv_obj_add_flag(s_head, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_warn, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < W_LINES; i++) {
        lv_obj_add_flag(s_line[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_font(s_head, W_META, 0);
    lv_obj_set_style_text_font(s_warn, W_BODY, 0);
    if (!visible || !s_w.now[0]) {
        return;
    }
    if (page == 0) {
        render_page_now();
    } else if (page == 1) {
        render_page_days();
    } else {
        render_page_pollen();
    }
}

#else /* StickS3: keine Karten */

void ui_weather_create_locked(lv_obj_t *parent)
{
    (void)parent;
}

void ui_weather_render_locked(uint8_t page, bool visible)
{
    (void)page;
    (void)visible;
}

#endif
