#pragma once

/*
 * Kontrollzentrum der Uhr: mit einem Wischen vom oberen Rand nach unten.
 * Bis zu sechs Kacheln, Reihenfolge und Auswahl aus der App
 * (`watch_settings.tiles`); ohne Einstellung Lautlos, Helligkeit, Dunkel & still,
 * Sparen, Lampe, Akku. Was die Kacheln bewirken, entscheidet main.c; hier
 * wird nur gezeichnet und getroffen. Dazu die Taschenlampe: der ganze Schirm weiss, bis zum
 * naechsten Antippen. Auf dem StickS3 tun die Funktionen nichts.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_CONTROL_BRIGHTNESS,
    UI_CONTROL_SILENT,
    UI_CONTROL_CINEMA,
    UI_CONTROL_SAVER,
    UI_CONTROL_TORCH,
    UI_CONTROL_BATTERY,
    UI_CONTROL_COUNT,
} ui_control_tile_t;

typedef struct {
    const char *brightness;   /* "Hell", "Mittel", "Dunkel" */
    bool silent;
    bool cinema;
    bool saver;
    int battery_percent;      /* -1: unbekannt */
    bool charging;
    const char *remaining;    /* "noch 9 h" oder "" -- die Restzeit aus main.c */
} ui_control_state_t;

void ui_control_show(const ui_control_state_t *state);
void ui_control_update(const ui_control_state_t *state);
void ui_control_hide(void);
bool ui_control_visible(void);
/* Kachel unter dem Finger, -1: keine. */
int ui_control_tile_at(int16_t x, int16_t y);
/* "silent", "brightness", ... -> Kachel; -1 bei Unbekanntem (still verworfen). */
int ui_control_tile_from_name(const char *name);
/*
 * Sichtbare Kacheln in Reihenfolge (ui_control_tile_t). Doppelte und
 * unbekannte fallen weg; bleibt keine uebrig, aendert sich nichts.
 */
void ui_control_set_tiles(const uint8_t *tiles, uint8_t count);

void ui_torch_set(bool on);
bool ui_torch_on(void);
