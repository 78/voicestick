#pragma once

/*
 * Auswahl fuer `input_select` auf der Karte Zuhause (0.4.5).
 *
 * Bis 0.4.4 schaltete ein Tippen auf eine solche Zeile zyklisch zur
 * naechsten Option. Jetzt traegt die Zeile im `list` das Flag `s`, und ein
 * Tippen oeffnet diese Szene: Titel = Zeilentext, darunter die Optionen als
 * grosse Zeilen, die aktuelle gruen umrandet. Die Optionen kennt die Uhr
 * nicht -- sie fragt die Bruecke (`options_request`) und zeigt bis zur
 * Antwort (`options`) "Laedt ...". Tippen auf eine Option waehlt sie (main.c
 * schickt `option`), Tippen auf die aktive schliesst nur. Nach unten wischen
 * (am Anfang der Liste), vom linken Rand zurueck und zehn Sekunden ohne
 * Eingabe schliessen ohne Senden; kommt binnen drei Sekunden keine Antwort,
 * steht anderthalb Sekunden "Keine Antwort". docs/protocol.md, 0.4.5.
 *
 * Auf dem StickS3 gibt es die Szene nicht; die Funktionen tun dort nichts.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ui_cards.h"

/* So viele Optionen schickt der Server hoechstens, je bis 16 Zeichen (UTF-8 bis 40 Byte). */
#define UI_OPTIONS_MAX 8
#define UI_OPTIONS_TEXT_LEN 41

typedef enum {
    UI_OPTIONS_NONE,   /* nichts zu tun */
    UI_OPTIONS_CHOSEN, /* eine andere als die aktive Option gewaehlt -- `choice` sagt welche */
    UI_OPTIONS_CLOSED, /* geschlossen, ohne etwas zu senden */
} ui_options_action_t;

typedef enum {
    UI_OPTIONS_FILL_IGNORED, /* keine offene Auswahl fuer diesen Stand und Index */
    UI_OPTIONS_FILL_SHOWN,   /* Optionen stehen */
    UI_OPTIONS_FILL_EMPTY,   /* leere Liste (Karte inzwischen neu): geschlossen */
} ui_options_fill_t;

typedef struct {
    /* Koordinaten der Zeile wie bei `item` -- auch fuer `undo` aus Chip und Verlauf. */
    ui_card_kind_t kind;
    uint16_t rev;
    uint8_t index;
    /* Index der gewaehlten Option in `items` der Bruecke. */
    uint8_t choice;
    /* Fuer den Verlaufseintrag "Buero Modus: Gaming". */
    char title[UI_CARD_TEXT_LEN];
    char option[UI_OPTIONS_TEXT_LEN];
} ui_options_choice_t;

/* Auswahl fuer eine Zeile oeffnen, im Zustand "Laedt ..." (das `options_request` schickt main.c). */
void ui_options_open(ui_card_kind_t kind, uint16_t rev, uint8_t index, const char *title);
/*
 * Antwort `options` der Bruecke. Passt sie zur offenen Auswahl (`rev`,
 * `index`), stehen danach die Optionen; `current` ist die aktive (-1 =
 * unbekannt). Aus dem Bluetooth-Strang gerufen -- die Sperre nimmt sich die
 * Funktion selbst.
 */
ui_options_fill_t ui_options_fill(uint16_t rev, uint8_t index, int current,
                                  const char *const *items, uint8_t count);
void ui_options_hide(void);
bool ui_options_visible(void);
/* Gesten (voice_board_gesture_t als int) und Lage des Fingers. */
ui_options_action_t ui_options_touch(int gesture, int16_t x, int16_t y,
                                     ui_options_choice_t *choice);
