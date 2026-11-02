#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Antwortmoeglichkeiten auf eine Rueckfrage: hoechstens drei. Bis 0.4.6 je
 * 15 Byte ("Ja", "Nein"); seit 0.5.0 (Punkt 43) auch Vorschlaege statt
 * "Nicht verstanden" mit bis zu 32 Zeichen -- in UTF-8 bis rund 40 Byte.
 * Was nicht nebeneinander passt, steht untereinander in zwei Zeilen.
 */
#define UI_STATUS_CHOICES_MAX 3
#define UI_STATUS_CHOICE_LEN 41

/* Zeilen unter der Uhrzeit (nur die Uhr): hoechstens drei, je 56 Byte (bis 0.3.6: 40). */
#define UI_STATUS_FACE_LINES 3
#define UI_STATUS_FACE_LEN 57

/*
 * Ergebnis eines Turns, wie die Bruecke es mit `ui_state` meldet
 * (`outcome`, docs/protocol.md): angelegt, erledigt, Antwort, Fehler. Der
 * Verlauf zeigt je Eintrag einen Punkt in der Farbe und das Wort dazu.
 */
typedef enum {
    UI_OUTCOME_NONE,
    UI_OUTCOME_CREATED,
    UI_OUTCOME_DONE,
    UI_OUTCOME_ANSWER,
    UI_OUTCOME_ERROR,
} ui_outcome_t;

/* Was ein Chip im Verlauf ausloest -- main.c schickt das Ereignis an die Bruecke. */
typedef enum {
    UI_HISTORY_CHIP_NONE,
    UI_HISTORY_CHIP_REPEAT,     /* "Nochmal": `repeat` mit der Frage */
    UI_HISTORY_CHIP_UNDO_TURN,  /* "Rueckgaengig" eines Turns: `undo_turn` mit `ref` */
    UI_HISTORY_CHIP_UNDO_CARD,  /* "Rueckgaengig" einer Karten-Aktion: `undo` mit Koordinaten */
} ui_history_chip_t;

typedef struct {
    ui_history_chip_t chip;
    char question[64];   /* fuer `repeat` */
    uint16_t ref;        /* fuer `undo_turn` */
    uint8_t card_kind;   /* fuer `undo`: ui_card_kind_t, Stand, Position */
    uint16_t card_rev;
    uint8_t card_index;
} ui_history_chip_hit_t;

typedef enum {
    UI_STATUS_MOOD_NONE,
    UI_STATUS_MOOD_HAPPY,
    UI_STATUS_MOOD_LOOK_LEFT,
    UI_STATUS_MOOD_LOOK_RIGHT,
} ui_status_mood_t;

esp_err_t ui_status_init(void);
esp_err_t ui_status_set_brightness(uint8_t brightness);
void ui_status_prepare_deep_sleep(void);
/*
 * Schirm ganz aus (Sleep-In des Controllers, LVGL-Refresh pausiert) und
 * wieder an. Nur die Uhr; der StickS3 kennt nur die Helligkeit. Das
 * Einschalten dauert 120 ms (Sleep-Out-Wartezeit).
 */
void ui_status_set_display_off(bool off);
void ui_status_set_device_name(const char *device_name);
void ui_status_set_advertising(void);
void ui_status_set_pairing(const char *device_name);
void ui_status_set_idle(void);
/*
 * Antwort der Bruecke plus die verstandene Frage; beides wandert in den
 * Verlauf, mit Ergebnis-Marker und der `ref`, unter der die Bruecke den Turn
 * 15 Minuten lang zuruecknimmt (0 = nicht ruecknehmbar).
 */
void ui_status_set_idle_text(const char *text, const char *question, ui_outcome_t outcome,
                             uint16_t ref);
/*
 * Karten-Aktion als Verlaufseintrag ("Erledigt: Muell rausstellen", "Licht
 * Kueche: An") samt Koordinaten fuer `undo` -- die Szene aendert sich nicht.
 */
void ui_status_history_add_card(const char *line, uint8_t card_kind, uint16_t card_rev,
                                uint8_t card_index);
/* Liegt der Finger auf "Nochmal" oder "Rueckgaengig" des gezeigten Eintrags? */
bool ui_status_history_chip_at(int16_t x, int16_t y, ui_history_chip_hit_t *out);
/* Nach einem gesendeten Rueckgaengig: der gezeigte Eintrag ist nicht noch einmal ruecknehmbar. */
void ui_status_history_mark_undone(void);
/*
 * Verlauf in den NVS, nur die Uhr: 30 s nach einer Aenderung meldet sich der
 * Zeitgeber ueber diesen Ruf (aus dem esp_timer-Task, kleiner Stack) statt
 * selbst zu schreiben. Der Abnehmer ruft ui_status_history_save() aus seinem
 * eigenen Task; ohne Abnehmer schreibt der Zeitgeber selbst.
 */
typedef void (*ui_status_history_save_cb_t)(void);
void ui_status_set_history_save_cb(ui_status_history_save_cb_t cb);
void ui_status_history_save(void);
void ui_status_set_idle_dimmed(bool dimmed);
/* Einen Schritt zurueck durch die letzten Fragen und Antworten. */
void ui_status_browse_history(void);
/* Eine Antwort neuer -- das Gegenstueck zum Blaettern, fuer das Wischen auf der Uhr. */
void ui_status_browse_newer(void);
/* Steht gerade der Verlauf (oder sein leerer Zustand) auf dem Schirm? */
bool ui_status_in_history(void);
/* Zurueck aufs Ziffernblatt, ohne Bruecke auf "Koppeln". */
void ui_status_show_home(void);
/*
 * Antwortflaechen unter der gerade gezeigten Antwort (nur die Uhr). Die
 * naechste Szene raeumt sie wieder weg.
 */
/*
 * Was unter der Uhrzeit steht, und der naechste Termin (UTC-Sekunden, 0 =
 * keiner). Die Uhr fuehrt den Countdown selbst nach.
 */
void ui_status_set_face(const char (*lines)[UI_STATUS_FACE_LEN], uint8_t count,
                        const char *event_title, int64_t event_start,
                        const uint8_t *kinds, uint8_t kind_count);
/* Teckis Stimmung fuer `duration_ms`, danach wieder die Szene. */
void ui_status_set_mood(ui_status_mood_t mood, uint32_t duration_ms);
void ui_status_set_choices(const char (*choices)[UI_STATUS_CHOICE_LEN], uint8_t count);
/* Welche Flaeche liegt unter dem Finger? -1: keine. `out` bekommt ihren Text. */
int ui_status_choice_at(int16_t x, int16_t y, char *out, size_t out_size);
void ui_status_set_cancelled(void);
/*
 * Transkript zuerst (nur die Uhr zeichnet "Abbrechen" und den Balken):
 * "Verstanden" + Text, `cancel_ms` Frist. Endet mit der naechsten Szene.
 */
void ui_status_set_transcript(const char *text, uint32_t cancel_ms);
/* Steht die Transkript-Szene gerade da? */
bool ui_status_transcript_shown(void);
/* ... und laeuft ihre Frist noch? */
bool ui_status_transcript_cancellable(void);
/* Liegt der Finger auf "Abbrechen" (und laeuft die Frist noch)? */
bool ui_status_transcript_cancel_at(int16_t x, int16_t y);
void ui_status_set_transcript_cancelled(void);
/*
 * Kurzform <-> ganze Antwort. `true`, wenn umgeschaltet wurde; `false`, wenn
 * die Szene nichts umzuschalten hat.
 */
bool ui_status_toggle_more(void);
/* Stehen gerade Antwortflaechen einer Rueckfrage da? */
bool ui_status_choices_open(void);
/* Schriftgroesse aus `watch_settings` (0 klein, 1 normal, 2 gross); nur die Uhr wertet sie aus. */
void ui_status_set_font_level(uint8_t level);
/* Ein Firmware-Update laeuft: Tecki zeigt es auf Ziffernblatt und Bereit-Szene. */
void ui_status_set_updating(bool updating);
void ui_status_set_press_too_short(void);
/* Sprechtaste gedrueckt, aber keine Bruecke verbunden -- vorher geschah dann sichtbar nichts. */
void ui_status_set_not_connected(void);
void ui_status_set_no_speech(const char *hint);
void ui_status_set_link(bool connected);
/* Uhrzeit von der Bruecke: UTC-Sekunden plus Abstand der Ortszeit in Minuten. */
void ui_status_set_clock(int64_t epoch_utc, int32_t tz_offset_min);
/* Nach dem Tiefschlaf: nur der Abstand kommt zurueck, die Systemuhr lief weiter. */
void ui_status_restore_clock(int32_t tz_offset_min);
void ui_status_set_recording_meter(uint8_t level_percent, uint8_t remaining_percent);
void ui_status_set_recording(uint32_t session_id);
void ui_status_set_battery(int level_percent, bool charging, bool usb_powered);
/*
 * Akku-Restzeit fuer das Zifferblatt ("noch etwa 2 h"), fertig formatiert von
 * main.c; leer = nichts zeigen. Steht rot unter den Zifferblatt-Zeilen (nur die Uhr).
 */
void ui_status_set_battery_remaining(const char *line);
/*
 * Nachtlade-Modus (nur die Uhr): am Kabel zwischen 22 und 6 Uhr zeigt das
 * Zifferblatt nur die grosse Uhrzeit in Dunkelgrau -- keine Zeilen, kein
 * Tecki, keine Kopfzeile. Andere Szenen bleiben, wie sie sind.
 */
void ui_status_set_night_mode(bool on);
/*
 * Nachtmodus (0.4.4, nur die Uhr wertet es aus): im Fenster `watch_settings.night`
 * zeigt das Zifferblatt -- alle drei Layouts -- alles in gedaempftem
 * Bernstein (bis 0.4.6 Rot; Rot bleibt seit 0.5.0 den Warnungen). Andere
 * Szenen bleiben farbig; die Helligkeit deckelt main.c.
 */
void ui_status_set_night_tint(bool on);

/*
 * Zifferblatt-Layouts (0.4.4): `watch_settings.face` und die Auswahl am
 * langen Druck. Die Namen stehen in ui_face_picker.c (FACE_LAYOUT_NAMES)
 * und im Protokoll (doku-vertrag:uhr-layouts).
 */
typedef enum {
    UI_FACE_INFO,     /* Uhrzeit in Zellen, drei Zeilen, Termin, Akku, Tecki */
    UI_FACE_MINIMAL,  /* nur grosse Uhrzeit, Datum, Akku-Punkt */
    UI_FACE_LARGE,    /* sehr grosse Uhrzeit zweizeilig, eine Zeile darunter */
    UI_FACE_COUNT,
} ui_face_layout_t;

/* "info", "minimal", "large" -> Layout; -1 bei Unbekanntem. */
int ui_face_layout_from_name(const char *name);
const char *ui_face_layout_name(ui_face_layout_t layout);
void ui_status_set_face_layout(ui_face_layout_t layout);
/* Steht gerade das Zifferblatt (welches Layout auch immer) auf dem Schirm? */
bool ui_status_face_shown(void);

/*
 * Auswahl des Layouts (nur die Uhr): eine grosse Vorschau, links/rechts
 * wischen blaettert, Seitenpunkte, Name darunter. Tippen uebernimmt, nach
 * unten oder vom linken Rand bricht ab, nach 10 s ohne Eingabe auch.
 */
typedef enum {
    UI_FACE_PICKER_NONE,       /* geblaettert oder nichts */
    UI_FACE_PICKER_CHOSEN,     /* uebernommen -- `chosen` sagt welches; Auswahl ist zu */
    UI_FACE_PICKER_CANCELLED,  /* abgebrochen; Auswahl ist zu */
} ui_face_picker_action_t;

void ui_face_picker_show(ui_face_layout_t current);
void ui_face_picker_hide(void);
bool ui_face_picker_visible(void);
/* Gesten (voice_board_gesture_t als int) und Lage des Fingers. */
ui_face_picker_action_t ui_face_picker_touch(int gesture, int16_t x, int16_t y,
                                             ui_face_layout_t *chosen);

/*
 * "Denkt nach" mit Stufe (0.4.4): `status` ist die Zeile ("Verstehe …",
 * "Erledige …", ohne Stufe "Denkt nach"), `text` ein Teiltext darunter.
 * `since_us` (esp_timer) ist der Beginn des Nachdenkens in diesem Turn: ab
 * fuenf Sekunden haengt die Zeile einen Sekundenzaehler an ("Erledige … 7 s"),
 * einmal je Sekunde nachgezogen, solange der Schirm an ist. 0 = kein Zaehler.
 */
void ui_status_set_thinking(const char *status, const char *text, int64_t since_us);
void ui_status_set_partial_text(const char *text);
void ui_status_set_ota_progress(uint32_t written, uint32_t size);
void ui_status_set_ota_rebooting(void);
void ui_status_set_error(const char *message);

/*
 * Paket J (0.5.0), nur die Uhr -- auf dem StickS3 tun die Funktionen nichts.
 *
 * Lautlos und "Dunkel & still" sichtbar: je ein kleines Zeichen in der
 * Kopfzeile jeder Szene (auch auf dem Zifferblatt), solange es an ist.
 */
void ui_status_set_quiet_marks(bool silent, bool still);
/* Roter Punkt oben mittig, solange das Mikrofon offen ist -- in jeder Szene. */
void ui_status_set_mic_open(bool open);

/*
 * Renderzeit (Punkt 47): zusammengefasst ueber rund zehn Sekunden, in denen
 * gezeichnet wurde -- fuer die Diagnose-Seite. `false`, solange noch kein
 * Fenster voll ist (oder auf dem StickS3).
 */
typedef struct {
    uint32_t frames;     /* Bilder im Fenster */
    uint16_t window_s;   /* Laenge des Fensters in Sekunden */
    uint16_t fps_x10;    /* Bilder je Sekunde mal zehn */
    uint16_t avg_ms_x10; /* mittlere Zeit je Bild in ms mal zehn */
    uint16_t max_ms;     /* laengstes Bild im Fenster */
} ui_render_stats_t;
bool ui_status_render_stats(ui_render_stats_t *out);

/*
 * Lange Antwort (Punkt 42): Passt die ganze Antwort nicht auf den Schirm,
 * steht sie in Ausschnitten mit Seitenpunkten darunter. Einen Ausschnitt
 * weiter (+1) oder zurueck (-1); `false`, wenn es in der Richtung keinen
 * gibt -- dann tut die Geste nichts.
 */
bool ui_status_answer_scroll(int dir);

/*
 * Zifferblatt-Zeilen antippbar (Punkt 13): `face.kinds` der Bruecke, parallel
 * zu `lines`, als diese Werte (`kinds` in ui_status_set_face; 0 Eintraege =
 * keine, dann ist keine Zeile ein Ziel -- wie bis 0.4.6). Der Termin-
 * Countdown gilt mit `kinds` als UI_FACE_HIT_EVENT.
 */
typedef enum {
    UI_FACE_HIT_NONE,
    UI_FACE_HIT_TASK,
    UI_FACE_HIT_WEATHER,
    UI_FACE_HIT_WASTE,
    UI_FACE_HIT_EVENT,
    UI_FACE_HIT_OTHER,
} ui_face_hit_t;
/* "task", "weather", ... -> Art; Unbekanntes wird UI_FACE_HIT_OTHER. */
ui_face_hit_t ui_face_hit_from_name(const char *name);
/* Welche Zifferblatt-Zeile liegt unter dem Finger? NONE ausserhalb oder ohne `kinds`. */
ui_face_hit_t ui_status_face_hit_at(int16_t x, int16_t y);
/*
 * Gesperrt (Punkt 32): Die Bruecke meldet, dass der Server das Geraete-Token
 * ablehnt. Das Zifferblatt zeigt dann statt der Zeilen "Gesperrt -- in der
 * App neu koppeln".
 */
void ui_status_set_locked(bool locked);
/* Die juengste Antwort zeigen (BOOT kurz auf dem Zifferblatt, Punkt 12); ohne Verlauf die Bilanz. */
void ui_status_show_latest(void);
/*
 * Ein Verlaufseintrag ohne Frage und ohne Rueckgaengig (Erinnerungen,
 * Punkt 19) -- die Szene aendert sich nicht.
 */
void ui_status_history_add_note(const char *line);
