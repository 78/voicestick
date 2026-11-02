#pragma once

/*
 * Blaetter der Uhr (Paket J, Teil 2, 0.5.0): kleine Unterszenen ueber allem
 * anderen, jede mit dem sichtbaren Zurueck "‹" links oben (ui_chrome.h).
 * Nach rechts wischen oder "‹" schliesst, oben/unten scrollt, wo es etwas
 * zu scrollen gibt -- dieselbe Gestenregel wie ueberall seit 0.5.0.
 *
 *   Frage       Titel, Text und bis zu fuenf Knoepfe untereinander:
 *               Aktionsblatt (Punkte 14/15/20), Erinnerung (19), Update (37)
 *   Details     eine Aufgabe in Teilen von der Bruecke (`detail`, 14/15/20)
 *   PIN         vier Felder 1-4 vor dem Schalten eines Schlosses (31)
 *   Diagnose    eine Textseite, die main.c fuellt (36)
 *
 * Was die Knoepfe bewirken, entscheidet main.c; hier wird nur gezeichnet und
 * getroffen. Nur die Uhr: Die Datei wird nur dort gebaut, und main.c ruft
 * die Funktionen nur dort.
 */

#include <stdbool.h>
#include <stdint.h>

/* ── Frage ─────────────────────────────────────────────────────────────── */

#define UI_PROMPT_BUTTONS_MAX 5

typedef enum {
    UI_PROMPT_NONE,
    UI_PROMPT_SHEET,    /* Aktionsblatt einer Aufgabe */
    UI_PROMPT_REMINDER, /* Erinnerung aus Todoteck */
    UI_PROMPT_UPDATE,   /* neue Firmware */
} ui_prompt_kind_t;

/* Was ui_prompt_touch() meldet. */
#define UI_PROMPT_TOUCH_NONE (-2)  /* nichts (gescrollt, daneben) */
#define UI_PROMPT_TOUCH_CLOSED (-1) /* zu, ohne Knopf (Zurueck, rechts, Frist) */

/*
 * Zeigen (ersetzt eine offene Frage). `body` darf leer sein. `idle_ms` > 0:
 * so lange ohne Eingabe, dann zu (wie die Auswahl); 0 = bleibt stehen.
 */
void ui_prompt_show(ui_prompt_kind_t kind, const char *title, const char *body,
                    const char *const *buttons, uint8_t count, uint32_t idle_ms);
void ui_prompt_hide(void);
bool ui_prompt_visible(void);
/* Welche Frage steht (UI_PROMPT_NONE: keine)? */
ui_prompt_kind_t ui_prompt_kind(void);
/* Index des Knopfs, UI_PROMPT_TOUCH_CLOSED oder UI_PROMPT_TOUCH_NONE. Nach einem Knopf ist sie zu. */
int ui_prompt_touch(int gesture, int16_t x, int16_t y);

/* ── Details ───────────────────────────────────────────────────────────── */

/* Oeffnen im Zustand "Laedt ..." fuer die Zeile `rev`/`index` von Heute; `title` bis zur Antwort. */
void ui_detail_open(uint16_t rev, uint8_t index, const char *title);
/*
 * Ein Teil `detail` der Bruecke (aus dem Bluetooth-Strang; die Sperre nimmt
 * sich die Funktion). Passt er nicht zur offenen Szene, faellt er weg. Teil 0
 * setzt Titel, Projekt, Termin und Prioritaet (1-4, 0 = keine) und leert
 * den Rest; Folgeteile haengen Text bzw. Unteraufgaben an. NULL = Feld fehlt.
 */
void ui_detail_part(uint16_t rev, uint8_t index, uint8_t part, uint8_t total,
                    const char *title, const char *project, const char *due, int prio,
                    const char *text, const char *const *subtasks, const bool *subtask_done,
                    uint8_t subtask_count);
void ui_detail_hide(void);
bool ui_detail_visible(void);
/* `true`, wenn die Szene danach zu ist. */
bool ui_detail_touch(int gesture, int16_t x, int16_t y);

/* ── PIN ───────────────────────────────────────────────────────────────── */

#define UI_PIN_DIGITS 4

typedef enum {
    UI_PIN_NONE,    /* nichts oder eine Ziffer mehr */
    UI_PIN_DONE,    /* vier Ziffern -- `out` traegt sie ("1423") */
    UI_PIN_CLOSED,  /* zu, ohne Eingabe */
} ui_pin_action_t;

void ui_pin_show(const char *title);
void ui_pin_hide(void);
bool ui_pin_visible(void);
/* Eingabe leeren und eine Zeile darunter zeigen ("Falsch -- noch 2 Versuche"), `error` rot. */
void ui_pin_reset(const char *message, bool error);
/* `out` hat Platz fuer UI_PIN_DIGITS + 1 Zeichen. */
ui_pin_action_t ui_pin_touch(int gesture, int16_t x, int16_t y, char *out);

/* ── Diagnose ──────────────────────────────────────────────────────────── */

/* Zeigen bzw. nachziehen: Zeilen durch '\n' getrennt. */
void ui_diag_show(const char *text);
void ui_diag_update(const char *text);
void ui_diag_hide(void);
bool ui_diag_visible(void);
/* `true`, wenn die Seite danach zu ist. */
bool ui_diag_touch(int gesture, int16_t x, int16_t y);
