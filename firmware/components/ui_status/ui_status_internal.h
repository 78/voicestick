#pragma once

/*
 * Nur fuer Dateien dieser Komponente (ui_cards.c): dieselbe Sperre, derselbe
 * Schirm, dieselbe Textbereinigung wie ui_status.c. Kein oeffentlicher
 * Header -- wer ausserhalb zeichnen will, geht ueber ui_status.h.
 */

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

void ui_status_lock(void);
void ui_status_unlock(void);
lv_obj_t *ui_status_screen(void);
/* Latin-1-taugliche Kopie: Emoji raus, Typografie zu ASCII (wie alle Anzeigetexte). */
void ui_status_copy_display_text(char *dst, const char *src, size_t size);
/* Schriftgroesse aus `watch_settings`: 0 = klein, 1 = normal, 2 = gross. */
uint8_t ui_status_font_level(void);
/*
 * Die kleine Uhrzeit in der Kopfzeile neu bewerten -- nach jedem Ein- und
 * Ausblenden von Karten, Kontrollzentrum und Taschenlampe. Sperre gehalten.
 */
void ui_status_header_refresh_locked(void);
/* Uhrzeit "07:42" ("--:--" ungestellt) und Datum "So 1.11." (leer ungestellt) -- fuer die Vorschau der Layouts. */
void ui_status_format_clock(char *out, size_t size);
void ui_status_format_date(char *out, size_t size);
/*
 * "08:12" in Ortszeit fuer einen Zeitpunkt (UTC-Sekunden) -- leer, solange
 * die Uhr nicht gestellt ist. Sperre gehalten. Fuer das Alter der Karten (0.5.0).
 */
void ui_status_format_time_of_locked(int64_t epoch_utc, char *out, size_t size);
/* Nachtlade-Modus aktiv? Dann vibriert und piept nichts (ui_cards.c fragt vor der Warn-Vibration). */
bool ui_status_night_mode(void);

/*
 * Seitenpunkte (nur die Uhr): eine Reihe, hoechstens UI_DOTS_MAX Punkte von
 * (zehn Verlaufseintraege plus Bilanz-Seite brauchen elf)
 * je UI_DOTS_SIZE px -- die Groesse steht hier, damit ui_cards.c die Reihe
 * auf die Mitte der Kopfzeile setzen kann.
 */
#define UI_DOTS_MAX 12
#define UI_DOTS_SIZE 8

typedef struct {
    lv_obj_t *box;
    lv_obj_t *dot[UI_DOTS_MAX];
} ui_dots_t;

void ui_dots_create(ui_dots_t *dots, lv_obj_t *parent);
/* `count` 0 versteckt die Reihe; `active` ist der gruene Punkt (von links). */
void ui_dots_set(ui_dots_t *dots, uint8_t count, uint8_t active);
