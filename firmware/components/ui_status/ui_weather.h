#pragma once

/*
 * Die Wetterkarte der Uhr -- die vierte Karte in der Reihe (Zifferblatt |
 * Heute | Einkauf | Zuhause | Wetter). Die Daten schickt die Bruecke als
 * `weather`-Ereignis in bis zu vier Teilen (docs/protocol.md); gezeichnet
 * wird in das Wurzelobjekt der Karten (ui_cards.c), das auch die Gesten
 * verteilt: nach oben die naechste Seite, nach unten die vorige, auf der
 * ersten zurueck aufs Zifferblatt. Trefferflaechen hat die Karte keine.
 *
 *   Seite 1  jetzt, heute, Regen, sechs Stunden, Sonne und Wind
 *   Seite 2  die naechsten fuenf Tage
 *   Seite 3  Pollen und Warnungen -- nur, wenn es welche gibt
 *
 * Nur fuer Dateien dieser Komponente; auf dem StickS3 gibt es die Karte
 * nicht, die Datenfunktionen nehmen die Werte an und tun nichts damit.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Teil 0: leert den Stand und setzt die Zusammenfassung; leere Felder = kein Wetter. */
void ui_weather_begin(uint16_t rev, const char *now, const char *today, const char *rain,
                      const char *sun, const char *wind, const char *stale);
/* Teil 1: eine Stunde ("12h", "11°", Regen ja/nein) -- hoechstens sechs. */
void ui_weather_add_hour(const char *label, const char *temp, bool rain);
/* Teil 2: ein Tag ("Mo", "14° / 6°", "Regen 60 %") -- hoechstens fuenf. */
void ui_weather_add_day(const char *day, const char *temps, const char *condition);
/* Teil 3: Pollen (hoechstens vier) und Warnungen (hoechstens drei). */
void ui_weather_add_pollen(const char *text);
void ui_weather_add_warning(const char *text);
/* Nach einem Teil: neu zeichnen, falls die Karte gerade zu sehen ist (ueber ui_cards). */
void ui_weather_commit(void);

/* Sperre gehalten (ui_status_lock): Objekte im Kartenwurzel anlegen, einmal. */
void ui_weather_create_locked(lv_obj_t *parent);
/* Sperre gehalten: Seite `page` (0..) zeichnen oder alles verstecken. */
void ui_weather_render_locked(uint8_t page, bool visible);
/* Wieviele Seiten es gerade gibt (2 oder 3; 1 ohne Daten). */
uint8_t ui_weather_page_count(void);
/* Wann das letzte Wetter kam (UTC-Sekunden), 0 = nie (0.5.0). */
int64_t ui_weather_received_at(void);
/* Stand der Bruecke (`rev` aus Teil 0) -- fuer `context` (0.5.0). */
uint16_t ui_weather_rev(void);
/* Sperre gehalten: alles vergessen, wie vor dem ersten Wetter (Gesperrt, 0.5.0). */
void ui_weather_clear_locked(void);
/* Kam je ein Wetter von der Bruecke (auch ein leeres)? */
bool ui_weather_received(void);
/* Gibt es ueberhaupt Daten (now nicht leer)? */
bool ui_weather_has_data(void);
/*
 * Steht eine Warnung, die so noch nicht gezeigt wurde? Einmal `true` je
 * neuem Text -- die Karte vibriert dann beim Anzeigen (2 x 120 ms).
 */
bool ui_weather_take_new_warning_locked(void);
