#pragma once

/*
 * Die Karten der Uhr: Heute, Einkauf, Zuhause, Wetter.
 *
 * Erreichbar vom Ziffernblatt mit einem Wischen nach links (oder nach oben);
 * links/rechts wechselt die Karte, nach oben scrollt die Liste weiter, nach
 * unten zurueck -- am Anfang eine Ebene zurueck. Ein Tippen auf eine Zeile
 * hakt sie ab (Einkauf) oder schaltet sie um (Zuhause) -- ausgefuehrt von
 * der Bruecke, die Karte zeigt es sofort. Auf Heute (und bei Schaltern mit
 * Flag `c`) markiert das Tippen die Zeile nur; ausgefuehrt wird erst mit
 * dem Chip "OK - erledigt" / "OK - schalten" in der Zeile. Danach steht
 * fuenf Sekunden "Rueckgaengig"; aus dem Verlauf geht es 15 Minuten.
 *
 * Die Inhalte schickt die Bruecke als `list`-Ereignis (docs/protocol.md).
 * Die Uhr kennt nur Texte und Positionen, keine IDs: Ein Tippen meldet
 * Art, Stand (`rev`) und Index, und die Bruecke weiss, was dahinter steht.
 * Auf dem StickS3 gibt es die Karten nicht; die Funktionen tun dort nichts.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_CARD_TODAY,
    UI_CARD_SHOPPING,
    UI_CARD_SWITCHES,
    /* Die Wetterkarte (ui_weather.c): keine Liste, eigene Seiten, keine Trefferflaechen. */
    UI_CARD_WEATHER,
    UI_CARD_COUNT,
} ui_card_kind_t;

/* Die Karten mit `list`-Daten: alle vor der Wetterkarte. */
#define UI_CARD_LIST_COUNT UI_CARD_WEATHER

#define UI_CARD_ITEMS_MAX 15
#define UI_CARD_TEXT_LEN 41
/* Rechts in der Zeile: Uhrzeit, Datum, "1"/"0" -- oder seit 0.4.3 ein Wort (Option, "Offen"), 15 Bytes. */
#define UI_CARD_AUX_LEN 16

typedef enum {
    UI_CARDS_NONE,   /* nichts zu tun (oder intern erledigt) */
    UI_CARDS_ITEM,   /* Zeile angetippt -- `hit` sagt welche */
    UI_CARDS_CLOSE,  /* eine Ebene zurueck (die Karten sind schon zu) */
    UI_CARDS_UNDO,   /* "Rueckgaengig" angetippt -- `hit` sagt, welche Zeile */
    UI_CARDS_END,    /* am Ende der Reihe weitergewischt -- nichts mehr rechts */
    UI_CARDS_SELECT, /* Heute: Zeile markiert, wartet auf "OK" -- noch nichts senden */
    UI_CARDS_OPTIONS, /* Schalter mit Flag `s` (input_select, 0.4.5): Auswahl oeffnen -- `hit` sagt welche */
} ui_cards_action_t;

typedef struct {
    ui_card_kind_t kind;
    uint16_t rev;
    uint8_t index;
    /*
     * Fuer den Verlaufseintrag: der Zeilentext und bei Schaltern der
     * Zielzustand -- `binary` sagt, ob `aux` "1"/"0" war (dann gilt `on`);
     * sonst zeigt die Zeile ein Wort, und die Uhr kennt den Zielstand nicht.
     */
    char text[UI_CARD_TEXT_LEN];
    bool on;
    bool binary;
} ui_cards_hit_t;

/* "today", "shopping", "switches" -> Art; -1 bei Unbekanntem. */
int ui_cards_kind_from_name(const char *name);
const char *ui_cards_kind_name(ui_card_kind_t kind);

/* Eine Liste kommt in Teilen: Teil 0 leert sie, jeder Teil haengt an. */
void ui_cards_begin_part(ui_card_kind_t kind, uint16_t rev, uint8_t part);
/*
 * `colour`: "#RRGGBB" (Projektfarbe, nur `today`) oder ""/NULL -- dann kein
 * Streifen. `group`: Ueberschrift der Gruppe ("Ueberfaellig", nur `today`),
 * ""/NULL = keine. `flags`: "c" = erst markieren, dann "OK" (Schalter mit
 * Bestaetigung); "e" = Termin (0.4.4, nur `today`): nicht antippbar,
 * Streifen in Kalender-Blau statt Projektfarbe; "s" = Auswahl (0.4.5, nur
 * `switches`, ein input_select): Tippen oeffnet die Liste der Optionen
 * (ui_options.h) statt weiterzuschalten, ohne OK-Schritt, auch neben `c`;
 * ""/NULL = direkt.
 */
void ui_cards_add_item(ui_card_kind_t kind, const char *text, const char *aux,
                       const char *colour, const char *group, const char *flags);
/* Nach dem letzten Teil: neu zeichnen, falls die Karte gerade zu sehen ist. */
void ui_cards_commit(ui_card_kind_t kind);

void ui_cards_show(ui_card_kind_t kind);
/* Neu zeichnen, falls sichtbar -- nach einer neuen Schriftgroesse. */
void ui_cards_refresh(void);
void ui_cards_hide(void);
bool ui_cards_visible(void);

/*
 * Rueckgaengig aus dem Verlauf: die Zeile zeigt wieder den Stand vor dem
 * Tippen, sofern die Liste noch dieselbe ist (`rev`). Zeichnet neu, falls sichtbar.
 */
void ui_cards_restore(ui_card_kind_t kind, uint16_t rev, uint8_t index);

/*
 * Eine Option der Auswahl ist gesendet (0.4.5): Die Zeile zeigt "..." bis
 * zur neuen Karte, und fuenf Sekunden steht "Rueckgaengig" -- wie nach dem
 * Tippen auf einen Schalter. Nur, wenn die Liste noch dieselbe ist (`rev`).
 */
void ui_cards_mark_sent(ui_card_kind_t kind, uint16_t rev, uint8_t index);

/* Gesten (voice_board_gesture_t als int) und Lage des Fingers. */
ui_cards_action_t ui_cards_touch(int gesture, int16_t x, int16_t y, ui_cards_hit_t *hit);
