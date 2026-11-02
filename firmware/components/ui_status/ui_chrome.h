#pragma once

/*
 * Kopfzeilen-Helfer der Uhr (Paket J, 0.5.0) -- nur fuer Dateien dieser
 * Komponente. Was main.c davon braucht (Lautlos-Zeichen, Mikrofon-Punkt,
 * Renderzeit), steht in include/ui_status.h.
 *
 * Bausteine fuer jede Unterszene (Auswahl, Layout-Wahl, spaeter Details und
 * Diagnose):
 *
 *   ui_back_create()  das sichtbare Zurueck "‹" links in der Kopfzeile:
 *                     56 x 48 px Trefferflaeche, Mitte bei (62, 40) absolut --
 *                     ausserhalb der Eckrundung (ui_layout_watch206.h).
 *   ui_back_hit()     liegt der Finger darauf (mit UI_HIT_TOLERANCE)?
 *   UI_TYPO(...)      die Typo-Skala (ui_layout_watch206.h)
 *   ui_glyph_*()      gezeichnete Symbole (ui_glyph.h)
 *
 * Die Unterszene legt ihre Wurzel wie gewohnt auf ui_status_screen() mit
 * CONTENT_W x CONTENT_H an; ui_back_create() setzt den Knopf relativ dazu.
 * Ihr Titel sollte links erst ab UI_BACK_RESERVE beginnen (oder mittig mit
 * diesem Abstand auf beiden Seiten stehen), sonst laeuft er unter den Pfeil.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Lage im Inhalt (Rand UI_SCREEN_PAD schon abgezogen). */
#define UI_BACK_X 18
#define UI_BACK_Y 0
#define UI_BACK_W 56
#define UI_BACK_H 48
/* Bis hierher (im Inhalt) gehoert die Kopfzeile links dem Zurueck. */
#define UI_BACK_RESERVE (UI_BACK_X + UI_BACK_W + 4)

lv_obj_t *ui_back_create(lv_obj_t *parent);
bool ui_back_hit(lv_obj_t *back, int16_t x, int16_t y);

/*
 * Lautlos-, Mikrofon- und Renderzeit-Teil (ui_chrome.c). Angelegt einmal beim
 * Start auf lv_layer_top(); `refresh` blendet nach Taschenlampe und
 * Nachtlade-Modus ein oder aus. Alle mit gehaltener Sperre.
 */
void ui_chrome_create_locked(lv_display_t *display);
void ui_chrome_refresh_locked(void);
