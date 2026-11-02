#pragma once

/*
 * Kleine Symbole der Uhr, gezeichnet statt gesetzt (Paket J, 0.5.0).
 *
 * Die Schriften der Uhr sind Latin-1: Sie kennen weder "‹" (U+2039) noch
 * Glühbirne, Schloss oder Lautsprecher, und eine Symbolschrift kostete Flash
 * für eine Handvoll Zeichen. Jedes Symbol ist deshalb **ein** LVGL-Objekt
 * ohne Kinder, das sich in LV_EVENT_DRAW_MAIN selbst aus Grundformen zeichnet
 * (Kreis, Rechteck, Linie, Bogen, Dreieck) -- auf einem Raster von 24 x 24,
 * skaliert auf die Größe des Objekts. Eine Kartenzeile kostet so ein Objekt
 * statt vier oder fünf.
 *
 * Zustand: `on` heißt gefüllt bzw. hell in `colour` ("an", beim Schloss
 * "offen"), aus heißt Umriss in UI_GLYPH_OFF_COLOUR. Pfeile, Liste und die
 * Zeichen der Kopfzeile kennen kein "aus" und zeichnen immer in `colour`.
 *
 * Nur die Uhr (CMakeLists.txt baut die Datei nur dort).
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef enum {
    UI_GLYPH_NONE,
    UI_GLYPH_BACK,    /* "‹": zurück, links in der Kopfzeile */
    UI_GLYPH_FORWARD, /* "›": rechts geht es weiter */
    UI_GLYPH_LIGHT,   /* Glühbirne: light (Flag `l`) */
    UI_GLYPH_SWITCH,  /* Kippschalter: switch, input_boolean (Flag `w`) */
    UI_GLYPH_FAN,     /* Lüfter: fan (Flag `f`) */
    UI_GLYPH_LOCK,    /* Schloss: lock (Flag `k`); on = offen */
    UI_GLYPH_LIST,    /* Auswahl: input_select (Flag `s`) */
    UI_GLYPH_SILENT,  /* Lautsprecher mit Kreuz: Lautlos an */
    UI_GLYPH_STILL,   /* Mond: "Dunkel & still" an */
} ui_glyph_t;

/* Grau des Umrisses im Zustand "aus". */
#define UI_GLYPH_OFF_COLOUR 0x6f7f95

/* Ein leeres Symbol (UI_GLYPH_NONE, versteckt) von `size` x `size` px. */
lv_obj_t *ui_glyph_create(lv_obj_t *parent, int32_t size);
/* Art, Zustand und Farbe (0xRRGGBB); zeichnet nur neu, wenn sich etwas ändert. */
void ui_glyph_set(lv_obj_t *glyph, ui_glyph_t kind, bool on, uint32_t colour);
