#include "ui_status.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <time.h>
#include <sys/time.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "stick_s3_board.h"
#include "ui_cards.h"
#include "ui_control.h"
#include "ui_history_text.h"
#include "ui_status_icons.h"
#include "ui_status_internal.h"

/*
 * Zwei Geraete, eine Anzeige-Logik. Was beim StickS3 hier als Mass oder
 * Schrift steht, kommt fuer die Waveshare-Uhr aus ui_layout_watch206.h --
 * unter denselben Namen. Der Ablauf (Szenen, Verlauf, Ziffernblatt, "passt
 * der Text?") ist fuer beide derselbe; nur Treiber und Geometrie wechseln.
 */
#if CONFIG_VOICE_BOARD_WATCH206
#define UI_WATCH 1
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_chrome.h"
#include "ui_display_watch206.h"
#include "ui_layout_watch206.h"
#define LCD_BACKLIGHT_DEFAULT 128
#else
#define UI_WATCH 0
#endif

static const char *TAG = "ui_status";

#if !UI_WATCH
#define LCD_HOST SPI2_HOST

/*
 * Querformat: 240 breit, 135 hoch.
 *
 * Das Panel selbst ist hochkant (135x240); gedreht wird im Controller ueber
 * das MV-Bit (esp_lcd_panel_swap_xy), nicht in LVGL — eine Software-Drehung
 * kostete bei jedem Flush Rechenzeit fuer etwas, das der ST7789 umsonst kann.
 *
 * Der Grund fuer die Drehung ist der Text: Eine Antwort wie "Aufgabe
 * angelegt: AliExpress Bestellung - Faellig: Di., 25.08.2026 - Projekt: Home
 * Lab" braucht auf 119 nutzbaren Pixeln Breite sieben Zeilen, auf 224 noch
 * drei. Dieselbe Antwort steht also in einer groesseren Schrift auf dem
 * Schirm, und am Handgelenk liest sie sich in der Laengsrichtung des Arms
 * ohnehin natuerlicher.
 *
 * Die Versaetze tauschen mit: Das sichtbare Fenster des Panels liegt hochkant
 * bei (52,40), nach dem Achsentausch also bei (40,52).
 */
#define LCD_H_RES 240
#define LCD_V_RES 135
#define LCD_X_GAP 40
#define LCD_Y_GAP 52

/*
 * Welche der beiden Querformat-Lagen.
 *
 * Der Achsentausch allein dreht das Bild um 90 Grad **und** spiegelt es; erst
 * eine der beiden Spiegelachsen macht daraus eine echte Drehung. Welche der
 * zwei moeglichen Lagen (um 180 Grad zueinander verdreht) die Tasten dorthin
 * legt, wo man sie am Handgelenk haben will, entscheidet sich am Geraet und
 * nicht hier. Steht das Bild nach dem ersten Flashen auf dem Kopf, ist diese
 * 0 eine 1 — sonst nichts.
 */
#define LCD_LANDSCAPE_FLIP 0

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_PWM_MAX 255
#define LCD_BACKLIGHT_DEFAULT 128

/*
 * Wie viele Bildzeilen ein Zeichenpuffer fasst — und warum die Zahl beim
 * Querformat kleiner werden musste.
 *
 * Zwei dieser Puffer liegen dauerhaft im **DMA-faehigen internen** RAM, und
 * aus demselben Topf holt sich der Aufnahmepfad bei jedem Tastendruck seinen
 * 32-KB-Stack und die I2S-Puffer. Die Rechnung:
 *
 *   hochkant, 24 Zeilen:  135 * 24 * 2 Byte * 2 = 12.960 Byte
 *   quer,     24 Zeilen:  240 * 24 * 2 Byte * 2 = 23.040 Byte
 *   quer,     10 Zeilen:  240 * 10 * 2 Byte * 2 =  9.600 Byte
 *
 * Die Drehung allein haette den Puffer also verdoppelt: gut zehn Kilobyte
 * dauerhaft weniger fuer alles andere. Das Ergebnis stand auf dem Display —
 * "Aufnahme startet nicht: ESP_ERR_NO_MEM", bei jedem Tastendruck. Mit zehn
 * Zeilen liegt der Verbrauch wieder unter dem der hochkanten Fassung, und der
 * Platz reicht auch fuer den Sensor-Task der Handgelenk-Geste.
 *
 * Der Preis sind mehr Uebertragungen je Bild (14 statt 6 fuer den vollen
 * Schirm). Bei 20 MHz kostet ein voller Schirm so oder so rund 26 ms SPI-Zeit;
 * was dazukommt, ist der Aufwand je Uebertragung, und der faellt neben einem
 * Geraet, das sonst gar nicht aufnimmt, nicht ins Gewicht.
 */
#define LVGL_DRAW_BUF_LINES 10
#endif /* !UI_WATCH */
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE (5 * 1024)
#define LVGL_TASK_PRIORITY 2

#if !UI_WATCH
#define LCD_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#endif
/*
 * Eigene Schriften statt der eingebauten LVGL-Montserrat: die bringen nur
 * ASCII mit, "Rueckspuelen" erschiene also mit Luecken. Diese hier sind mit
 * lv_font_conv aus Montserrat erzeugt und decken zusaetzlich Latin-1 ab,
 * also Umlaute und Eszett.
 */
/*
 * Die Schrift des Ziffernblatts. Nur Ziffern, Doppelpunkt und Bindestrich —
 * mehr steht dort nie, und ein voller Latin-1-Satz in dieser Groesse kostete
 * ein Vielfaches an Flash. `line_height` faellt dadurch auf 47 Pixel: Die
 * Schrift kennt weder Ober- noch Unterlaengen, die sie einrechnen muesste,
 * und die Ziffern werden bei gleicher Kastenhoehe entsprechend groesser als
 * bei einer vollstaendigen Schrift derselben Stufe.
 */
LV_FONT_DECLARE(todoteck_clock_64);
LV_FONT_DECLARE(todoteck_font_16);
LV_FONT_DECLARE(todoteck_font_14);
LV_FONT_DECLARE(todoteck_font_13);
LV_FONT_DECLARE(todoteck_font_12);
LV_FONT_DECLARE(todoteck_font_11);
LV_FONT_DECLARE(todoteck_font_10);

/*
 * Wofuer der untere Text steht — und damit, wieviel Flaeche er bekommt.
 *
 * `UI_TEXT_HINT` ist Beiwerk ("Halten zum Sprechen", der Geraetename beim
 * Koppeln): klein, gedaempft, unter Tecki. `UI_TEXT_MESSAGE` ist die Auskunft
 * selbst — die Antwort der Bruecke, ein Zwischenstand, eine Fehlermeldung.
 * Die gibt es nur einmal, und sie muss lesbar sein.
 */
typedef enum {
    UI_TEXT_HINT,
    UI_TEXT_MESSAGE,
} ui_text_kind_t;

/* Das Zustandswort ueber dem Text: "Bereit", "Koppeln", "Nichts gehoert". */
#define UI_STATUS_TEXT_MAX 32
/*
 * Die Frage, die anstelle des Zustandsworts stehen kann. Dieselbe Zahl wie das
 * Budget der Bruecke (`VoiceProtocol.QUESTION_BUDGET_BYTES`) — was sie schickt,
 * passt damit ohne Rest hier hinein.
 */
#define UI_QUESTION_MAX 64
#define UI_HINT_TEXT_MAX 192

static _lock_t s_lvgl_lock;
static bool s_ready;
static lv_display_t *s_display;
static lv_obj_t *s_screen;
static lv_obj_t *s_ble_dot;
static lv_obj_t *s_status_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_position_label;
static lv_obj_t *s_meter_track;
static lv_obj_t *s_meter_fill;
static lv_obj_t *s_time_track;
static lv_obj_t *s_time_fill;
static ui_status_icons_t s_icons;
static ui_status_icon_scene_t s_scene = UI_STATUS_ICON_BOOT;
/* Beiwerk oder Auskunft — entscheidet ueber Schriftgroesse und Platz. */
static ui_text_kind_t s_text_kind = UI_TEXT_HINT;
static char s_status_text[UI_STATUS_TEXT_MAX] = "Startet";
static char s_hint_text[UI_HINT_TEXT_MAX] = "einen Moment";
/*
 * Die verstandene Frage zur angezeigten Antwort. Sie steht anstelle des
 * Zustandsworts ueber dem Text — klein und zweizeilig, siehe QUESTION_H.
 * Leer heisst: Es gab keine, dann traegt die Zeile wie bisher das Wort.
 */
static char s_question_text[UI_QUESTION_MAX];
static char s_device_name[24] = "BLE";
static bool s_dimmed;
/*
 * Die Uhr steht erst, wenn die Bruecke sie einmal gestellt hat — vorher zeigt
 * das Geraet lieber gar keine Zeit als eine falsche. `s_clock_offset_min` ist
 * der Abstand zur UTC in Minuten, wie ihn die Bruecke meldet; damit braucht
 * die Firmware keine Zeitzonendatenbank und keine Sommerzeitregel. Wechselt
 * die Zeitzone oder die Sommerzeit, schickt die Bruecke den Wert beim
 * naechsten Verbinden ohnehin neu.
 */
static bool s_clock_valid;
static int32_t s_clock_offset_min;
/*
 * Der Verbindungszustand kam bisher aus der Szene: "Koppeln" faerbte den
 * Punkt blau, alles andere gruen. Das log, sobald die Bruecke waehrend einer
 * Anzeige wegbrach — man sah es erst beim naechsten Sprechversuch. Jetzt
 * traegt der Punkt den echten Zustand, unabhaengig davon, was auf dem
 * Display steht.
 */
static bool s_link_connected;
/* Faerbt die Akkuzahl: rot ab 10 Prozent (bis 0.3.6: 20), blau am Strom, sonst gedaempft. */
static bool s_battery_low;
/* Restzeit fuers Zifferblatt ("noch etwa 2 h"), fertig aus main.c; leer = nichts. */
static char s_battery_remaining[24];
/* Nachtlade-Modus: nur die grosse Uhrzeit, dunkelgrau (ui_status_set_night_mode). */
static bool s_night_mode;
/*
 * Nachtmodus-Fenster (0.4.4, `watch_settings.night`): das Zifferblatt in
 * gedaempftem Bernstein -- alle drei Layouts, auch im Nachtlade-Modus.
 * Andere Szenen bleiben farbig. `s_face_tint_shown` sagt, ob das zuletzt
 * gezeichnete Zifferblatt getoent war; Akkuzahl und Zeilen faerben sich
 * danach.
 *
 * Bis 0.4.6 war der Ton ein gedaempftes Rot (#b0301c / #6a1d10). Rot heisst
 * auf der Uhr aber "Achtung" -- ueberfaellig, Akku knapp, Warnung --, und
 * nachts war eine rote Warnzeile vom roten Zifferblatt nicht mehr zu
 * unterscheiden. Bernstein schont das Auge fast genauso und laesst Rot den
 * Warnungen (Paket J, 0.5.0).
 */
static bool s_night_tint;
static bool s_face_tint_shown;
#define NIGHT_TINT_TEXT 0xb87a1c
#define NIGHT_TINT_MUTED 0x6b4710
/* Layout des Zifferblatts (`watch_settings.face`, 0.4.4) -- nur die Uhr wertet es aus. */
static ui_face_layout_t s_face_layout = UI_FACE_INFO;
/* Stand beim letzten Zeichnen das Zifferblatt? Fuer den langen Druck (main.c). */
static bool s_face_shown;
/*
 * "Denkt nach" mit Sekundenzaehler (0.4.4): seit wann gedacht wird
 * (esp_timer-Mikrosekunden, 0 = kein Zaehler) und der Sekundentakt dazu.
 * Der Takt laeuft nur in der Denk-Szene; sonst pausiert er.
 */
static int64_t s_thinking_since_us;
static lv_timer_t *s_thinking_timer;
#define THINKING_COUNTER_FROM_S 5
static const char *thinking_status_locked(ui_status_icon_scene_t scene, const char *base);
/* Schirm ganz aus (nur die Uhr): dann zaehlt der Sekundentakt nicht sichtbar mit. */
static bool s_display_is_off;
static bool s_battery_charging;
/* Gedaempfte Farbe der laufenden Szene — hell auf Creme, blass auf Dunkel. */
static lv_color_t s_muted_colour;
/*
 * Der Verlauf: die letzten Fragen und Antworten, durch die die Blaettertaste
 * zappt.
 *
 * Bis 08/2026 stand hier eine einzige Zeichenkette — die letzte Antwort, die
 * ein Druck auf die damalige Seitentaste zurueckholte. Der Fall dahinter bleibt
 * derselbe (der Schirm dimmt nach 15 Sekunden und schlaeft nach fuenf
 * Minuten; wer erst danach hinsieht, hat nichts gelesen), er hoert nur nicht
 * bei eins auf: Wer drei Sachen hintereinander diktiert und dann nachsieht,
 * will alle drei.
 *
 * Fuenf Plaetze auf dem StickS3, weil der Ring vollstaendig im internen RAM
 * liegt und jeder Platz gut 270 Byte kostet. Die Uhr haelt zehn (knapp
 * 2,8 KB) und sichert sie im NVS, damit sie den Neustart ueberleben (11/2026,
 * Paket G). Wer laenger zurueck muss, sieht in der App nach — dort steht der
 * Verlauf ohnehin ungekuerzt.
 *
 * Die Frage kommt aus dem Steuerbefehl der Bruecke (Feld `question`). Bleibt
 * sie leer — aeltere Bruecke, oder eine, die sie nicht schickt —, traegt der
 * Eintrag nur die Antwort, und die Statuszeile faellt auf "Verlauf" zurueck.
 *
 * Je Eintrag dazu (nur die Uhr zeigt es): Zeitpunkt, Ergebnis-Marker
 * (`outcome`), die `ref` fuer "Rueckgaengig" eines Turns, und bei einer
 * Karten-Aktion deren Koordinaten (Art, Stand, Position) fuer `undo`. Alles
 * in festen Feldern, damit der Ring als ein Block ins NVS geht.
 */
#if UI_WATCH
#define UI_HISTORY_MAX 10
#else
#define UI_HISTORY_MAX 5
#endif

typedef enum {
    UI_HISTORY_TURN,
    UI_HISTORY_CARD,
} ui_history_kind_t;

typedef struct {
    char question[UI_QUESTION_MAX];
    char answer[UI_HINT_TEXT_MAX];
    int64_t at;          /* UTC-Sekunden; 0 = die Uhr stand nicht */
    uint16_t ref;        /* undo_turn-Kennung der Bruecke, 0 = nicht ruecknehmbar */
    uint16_t card_rev;
    uint8_t outcome;     /* ui_outcome_t */
    uint8_t kind;        /* ui_history_kind_t */
    uint8_t card_kind;
    uint8_t card_index;
    uint8_t undone;      /* Rueckgaengig schon geschickt */
} ui_history_entry_t;

static ui_history_entry_t s_history[UI_HISTORY_MAX];
/* Wieviele Plaetze belegt sind und wohin der naechste Eintrag geht. */
static uint8_t s_history_count;
static uint8_t s_history_next;
/*
 * Wo das Blaettern gerade steht: -1 heisst "nicht im Verlauf". Auf der Uhr
 * ist 0 die Bilanz-Seite ("Heute 4 Befehle ...") und 1 die juengste Antwort;
 * auf dem StickS3 ist 0 die juengste Antwort (HISTORY_FIRST). Jeder neue
 * Turn setzt ihn zurueck — sonst blaetterte der naechste Druck mitten in
 * einem Verlauf weiter, den man vor zehn Minuten verlassen hat.
 */
static int8_t s_history_cursor = -1;
#if UI_WATCH
#define HISTORY_FIRST 1
#else
#define HISTORY_FIRST 0
#endif
/* "Rueckgaengig" gilt 15 Minuten -- dieselbe Frist wie in Bruecke und Server. */
#define HISTORY_UNDO_WINDOW_S (15 * 60)
/*
 * Steht gerade "Verlauf -- Noch keine Antwort" da? Dann bringt der naechste
 * Druck zurueck, statt denselben Schirm noch einmal zu zeigen. Jede andere
 * Szene loescht die Marke (set_scene_full).
 */
static bool s_empty_history_shown;
/*
 * Antwortmoeglichkeiten auf eine Rueckfrage ("Ja", "Nein") -- nur die Uhr
 * zeigt sie, als Flaechen unter dem Text. Jede andere Szene raeumt sie weg
 * (set_scene_full), genau wie die Verlaufsmarke: Sie gehoeren zu genau der
 * einen Antwort, unter der sie stehen.
 */
static char s_choices[UI_STATUS_CHOICES_MAX][UI_STATUS_CHOICE_LEN];
/*
 * Der Text, wie die Bruecke ihn geschickt hat (0.5.0): `choice` meldet ihn
 * zurueck, und die Bruecke ordnet ihn einem Vorschlag zu -- die Anzeige-
 * Kopie oben hat Typografie schon zu ASCII gemacht und passte dann nicht mehr.
 */
static char s_choice_raw[UI_STATUS_CHOICES_MAX][UI_STATUS_CHOICE_LEN];
static uint8_t s_choice_count;
static lv_obj_t *s_choice_btn[UI_STATUS_CHOICES_MAX];
static lv_obj_t *s_choice_label[UI_STATUS_CHOICES_MAX];
/*
 * Was die Bruecke unter die Uhrzeit schreibt (naechste Aufgabe, Wetter,
 * Muell) und der naechste Termin. Nur die Uhr zeigt das; der Termin laeuft
 * als Countdown, den die Uhr selbst nachfuehrt -- die Bruecke schickt nur
 * Titel und Beginn.
 */
static char s_face[UI_STATUS_FACE_LINES][UI_STATUS_FACE_LEN];
static uint8_t s_face_count;
static char s_event_title[UI_STATUS_FACE_LEN];
static int64_t s_event_start;
/* Termin, bis drei Zeilen der Bruecke, dazu die rote Akku-Restzeit. */
#define FACE_LABELS (UI_STATUS_FACE_LINES + 2)
static lv_obj_t *s_face_label[FACE_LABELS];
/*
 * Zeilen antippbar (0.5.0, Punkt 13): die Arten aus `face.kinds` und je
 * sichtbarem Label, wofuer es steht (ui_face_hit_t) -- gesetzt beim Zeichnen.
 * Ohne `kinds` bleibt alles UI_FACE_HIT_NONE.
 */
static uint8_t s_face_kinds[UI_STATUS_FACE_LINES];
static uint8_t s_face_kind_count;
static uint8_t s_face_label_kind[FACE_LABELS];
/* Gesperrt (0.5.0, Punkt 32): statt der Zeilen "Gesperrt -- in der App neu koppeln". */
static bool s_face_locked;
/*
 * Teckis Stimmung fuer ein paar Sekunden ueber der Szene: freut sich ueber
 * eine Antwort, schaut beim Wischen in die Richtung. Laeuft ab und faellt
 * auf die Szene zurueck.
 */
static ui_status_mood_t s_mood;
static lv_timer_t *s_mood_timer;
/*
 * "2/5" in der Kopfzeile, solange geblaettert wird. Leer heisst: versteckt.
 *
 * Zwoelf Byte fuer fuenf Zeichen: Der Uebersetzer rechnet bei `snprintf` mit
 * dem vollen Wertebereich der beiden Zahlen (bis zu neun Zeichen), nicht mit
 * dem, den der Ring zulaesst -- und `-Werror=format-truncation` macht daraus
 * einen Fehler. Vier ungenutzte Byte sind billiger als eine Rechnung, die den
 * Bereich fuer den Uebersetzer nachweist.
 */
static char s_position_text[12];
/*
 * Kurzform (nur die Uhr): Eine Antwort, die nicht in zwei Zeilen passt,
 * steht zuerst gekuerzt da, mit "Tippen fuer mehr" darunter. `s_shortable`
 * sagt, ob die laufende Szene eine Antwort ist (frisch oder aus dem Verlauf),
 * `s_expanded`, ob sie gerade lang gezeigt wird. Jede Szene setzt beides
 * zurueck.
 */
static bool s_shortable;
static bool s_expanded;
/* Transkript-Szene: bis wann "Abbrechen" noch gilt (esp_timer-Mikrosekunden). */
static int64_t s_transcript_deadline_us;
/* Fuer Teckis Statuspose: Update laeuft, Akkustand in Prozent (-1 unbekannt). */
static bool s_updating;
static int s_battery_percent = -1;
/* Steht die Uhrzeit gerade gross in der Szene? Dann nicht noch einmal oben. */
static bool s_main_clock_shown;
#if UI_WATCH
static lv_obj_t *s_header_clock;
static lv_obj_t *s_more_btn;
static lv_obj_t *s_cancel_btn;
static lv_obj_t *s_cancel_track;
static lv_obj_t *s_cancel_fill;
static ui_dots_t s_history_dots;
static lv_obj_t *s_more_label;
/*
 * Lange Antwort in Ausschnitten (Paket J, Punkt 42): Passt die ganze Antwort
 * nicht in die Textflaeche, steht sie dort zeilengenau in Ausschnitten
 * (LONG_WRAP mit fester Hoehe, gescrollt um ganze Zeilen), darunter eine
 * Punktreihe wie in den Listen -- ein Punkt je Ausschnitt. Oben/unten
 * wischen blaettert (ui_status_answer_scroll). Bis 0.4.6 endete eine solche
 * Antwort mit "..." und der Rest war nur in der App zu lesen.
 */
static ui_dots_t s_answer_dots;
static uint8_t s_answer_page;
static uint8_t s_answer_pages;
static int32_t s_answer_page_h;
/*
 * Der gezeigte Verlaufseintrag (Kopie, waehrend er auf dem Schirm steht):
 * Marker und Zeit in der Zeile ueber der Frage, darunter die Chips. Jede
 * andere Szene raeumt das weg (assign_scene_locked).
 */
static bool s_meta_shown;
static ui_history_entry_t s_meta_entry;
static lv_obj_t *s_meta_dot;
static lv_obj_t *s_meta_label;
#define HISTORY_CHIPS 2
static lv_obj_t *s_chip_btn[HISTORY_CHIPS];
static lv_obj_t *s_chip_label[HISTORY_CHIPS];
static ui_history_chip_t s_chip_action[HISTORY_CHIPS];
static uint8_t s_chip_count;
/*
 * Die grosse Uhrzeit, zellenweise: fuenf Labels, jede Ziffer mittig in einer
 * Zelle fester Breite, der Doppelpunkt in einer schmalen. Die Gesamtbreite
 * ist damit fuer jede Uhrzeit dieselbe, und "11:47" steht nicht mehr links
 * von "08:00" -- in einer proportionalen Schrift traegt die "1" rechts Luft,
 * und LVGL zentriert die Summe der Vorschuebe, nicht die Tinte.
 */
#define CLOCK_CELLS 5
static lv_obj_t *s_clock_cell[CLOCK_CELLS];
/*
 * Die anderen Layouts (0.4.4): "Gross" zeigt Stunden und Minuten in zwei
 * Zeilen der 180er, "Minimal" das Datum unter der Uhrzeit und den Akku als
 * Punkt. Lage setzt render_alt_face_locked().
 */
static lv_obj_t *s_large_label[2];
static lv_obj_t *s_face_date;
static lv_obj_t *s_battery_dot;
/* Verlauf im NVS: gebuendelt schreiben, hoechstens einmal je 30 s. */
static esp_timer_handle_t s_history_save_timer;
static bool s_history_dirty;
#define HISTORY_SAVE_DELAY_US (30 * 1000 * 1000ULL)
#define HISTORY_NVS_NAMESPACE "verlauf"
#define HISTORY_NVS_KEY "ring"
#define HISTORY_NVS_MAGIC 0x314c5256u /* "VRL1" */
static void history_load(void);
static void history_save_now(void);
#endif
/* Zuletzt gesetzte Uhrzeit auf dem Schirm (gross oder klein): nur bei neuen Ziffern zeichnen. */
static char s_clock_text[8];

#if !UI_WATCH
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *display = (lv_display_t *)user_ctx;
    lv_display_flush_ready(display);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    const int x1 = area->x1;
    const int x2 = area->x2;
    const int y1 = area->y1;
    const int y2 = area->y2;

    lv_draw_sw_rgb565_swap(px_map, (x2 - x1 + 1) * (y2 - y1 + 1));
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
}
#endif /* !UI_WATCH */

/*
 * LVGLs Uhr liest die Systemzeit, statt dass ein Zeitgeber sie alle 10 ms
 * weiterschiebt (Paket H, Punkt 2). Der periodische esp_timer weckte den Chip
 * hundertmal je Sekunde aus dem Light Sleep -- fuer nichts als ein Inkrement.
 */
static uint32_t lvgl_tick_get(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    while (true) {
        _lock_acquire(&s_lvgl_lock);
        uint32_t delay_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);

        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}

static lv_obj_t *create_blob(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    return obj;
}

#if !UI_WATCH
/*
 * Kopfzeile und Schriften des StickS3. Die Uhr bringt dieselben Namen in
 * ui_layout_watch206.h mit.
 */
#define UI_SCREEN_PAD 8
#define UI_DOT_SIZE 8
#define UI_DOT_Y 6
#define UI_HEADER_LABEL_Y 4
#define UI_BATTERY_W 34
#define UI_POSITION_X 14
#define UI_HEADER_INSET_X 0
#define UI_FONT_STATUS (&todoteck_font_16)
#define UI_FONT_QUESTION (&todoteck_font_11)
#define UI_FONT_SMALL (&todoteck_font_10)
#define UI_FONT_CLOCK_FACE (&todoteck_clock_64)
#define UI_FONT_CLOCK_SMALL (&lv_font_montserrat_28)
#define UI_DARK_THEME 0
#endif

/*
 * Der Akku steht als Zahl da, nicht als Zeichnung.
 *
 * Das Symbol daneben sagte dasselbe noch einmal, nur ungenauer — und auf
 * einem Ziffernblatt zaehlt jeder Pixel, den etwas nicht braucht. Geblieben
 * ist die Prozentzahl; der farbige Fuellstand war ohnehin nur auf zwei
 * Stufen unterscheidbar.
 */
static void create_battery_ui(lv_obj_t *screen)
{
    s_battery_label = lv_label_create(screen);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_text_font(s_battery_label, UI_FONT_SMALL, 0);
    lv_label_set_long_mode(s_battery_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_battery_label, UI_BATTERY_W);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -UI_HEADER_INSET_X, UI_HEADER_LABEL_Y);
}

/*
 * Layoutmasse im Inhaltsbereich: 240x135 abzueglich 8 Pixel Rand ringsum.
 *
 * Im Querformat stehen Figur und Text **nebeneinander** statt uebereinander:
 * Tecki links in einer festen Spalte, rechts daneben Statuszeile und Text.
 * Hochkant war die Reihenfolge von oben nach unten die einzig moegliche; hier
 * waere sie die schlechtere, weil 119 Pixel Hoehe fuer Figur *und* mehrere
 * Textzeilen nicht reichen — die Figur allein braucht schon mehr als die
 * Haelfte davon.
 */
#if !UI_WATCH
#define CONTENT_W (LCD_H_RES - 16)
#define CONTENT_H (LCD_V_RES - 16)

/* Kopfzeile: Name und Verbindungspunkt links, Akku rechts. */
#define HEADER_H 16

/*
 * Die Spalte fuer Tecki. Die Zahl steht in ui_status_icons.h, weil dort die
 * ganze Figur aus ihr faellt — hier wird sie nur noch geerbt, damit Spalte
 * und Figur nicht auseinanderlaufen koennen.
 */
#define TECKI_BOX UI_TECKI_BOX
/* Oberkante der Figur in ihrer Spalte. */
#define TECKI_TOP_Y 35
/* Tecki steht links in seiner Spalte. */
#define TECKI_X 0
#define COL_GAP 10
/* Linke Kante und Breite der Textspalte neben der Figur. */
#define COL_X (TECKI_BOX + COL_GAP)
#define COL_W (CONTENT_W - COL_X)

#define METER_WIDTH 130
#define METER_LEVEL_HEIGHT 6
#define METER_TIME_HEIGHT 3
/* Unter der Statuszeile in der Textspalte, nicht mittig ueber den Schirm. */
#define METER_X (COL_X + (COL_W - METER_WIDTH) / 2)
#define METER_LEVEL_Y 74
#define METER_TIME_Y 86
#endif /* !UI_WATCH */

/*
 * Zwei Balken, die nur waehrend der Aufnahme sichtbar sind.
 *
 * Oben der Pegel: er beantwortet die Frage "hoert das Ding mich ueberhaupt?",
 * die man sonst erst nach dem Absenden beantwortet bekommt — und dann mit
 * einer leeren Transkription. Darunter die Restzeit bis zur
 * 30-Sekunden-Grenze, die der Server ohnehin zieht; ein Satz, der genau an
 * der Grenze abbricht, ist teurer als ein Balken, der vorher warnt.
 */
static void create_meters(lv_obj_t *screen)
{
    s_meter_track = lv_obj_create(screen);
    lv_obj_remove_style_all(s_meter_track);
    lv_obj_set_size(s_meter_track, METER_WIDTH, METER_LEVEL_HEIGHT);
    lv_obj_set_style_radius(s_meter_track, METER_LEVEL_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_meter_track, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(s_meter_track, lv_color_hex(0x9a8fa4), 0);
    lv_obj_align(s_meter_track, LV_ALIGN_TOP_LEFT, METER_X, METER_LEVEL_Y);
    lv_obj_add_flag(s_meter_track, LV_OBJ_FLAG_HIDDEN);

    s_meter_fill = lv_obj_create(s_meter_track);
    lv_obj_remove_style_all(s_meter_fill);
    lv_obj_set_size(s_meter_fill, 2, METER_LEVEL_HEIGHT);
    lv_obj_set_style_radius(s_meter_fill, METER_LEVEL_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_meter_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_meter_fill, lv_color_hex(0x55c98a), 0);
    lv_obj_align(s_meter_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_time_track = lv_obj_create(screen);
    lv_obj_remove_style_all(s_time_track);
    lv_obj_set_size(s_time_track, METER_WIDTH, METER_TIME_HEIGHT);
    lv_obj_set_style_radius(s_time_track, METER_TIME_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_time_track, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(s_time_track, lv_color_hex(0x9a8fa4), 0);
    lv_obj_align(s_time_track, LV_ALIGN_TOP_LEFT, METER_X, METER_TIME_Y);
    lv_obj_add_flag(s_time_track, LV_OBJ_FLAG_HIDDEN);

    s_time_fill = lv_obj_create(s_time_track);
    lv_obj_remove_style_all(s_time_fill);
    lv_obj_set_size(s_time_fill, METER_WIDTH, METER_TIME_HEIGHT);
    lv_obj_set_style_radius(s_time_fill, METER_TIME_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_time_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_time_fill, lv_color_hex(0xf2b23c), 0);
    lv_obj_align(s_time_fill, LV_ALIGN_LEFT_MID, 0, 0);
}

/*
 * Unterkante des Texts. Eine fuer beide Faelle: Ob er neben Tecki in der
 * Spalte steht oder ohne ihn ueber die ganze Breite, aendert die Breite —
 * nach unten hat er in beiden Faellen bis zum Rand Platz.
 */
#if !UI_WATCH
#define TEXT_BOTTOM_Y (CONTENT_H - 2)
#define STATUS_Y_WITH_TECKI (HEADER_H + 6)
#define STATUS_Y_TEXT_ONLY (HEADER_H + 2)
#define TEXT_GAP 4
#endif

/*
 * Die Uhr — der Grund, warum das Geraet auch am Handgelenk etwas taugt, wenn
 * gerade niemand spricht.
 *
 * Sie erscheint in den drei Zustaenden ohne Auskunft (Koppeln, Bereit,
 * Ruht) und nur dann, wenn die Bruecke die Zeit schon einmal geschickt hat:
 * Der StickS3 hat keinen RTC-Baustein und kein WLAN, seine einzige Zeitquelle
 * ist das `time`-Steuerereignis (docs/protocol.md). Ohne das waere jede
 * angezeigte Uhrzeit geraten.
 *
 * Gesetzt in Montserrat statt in den Todoteck-Schriften: "07:42" ist reines
 * ASCII, der Latin-1-Grund fuer die eigenen Schriften trifft hier also nicht
 * zu — und die eingebaute Schrift gibt es in Groessen, die wir nicht selbst
 * erzeugen muessen.
 */
/*
 * Zwei Ziffernblaetter, weil zwei Fragen dahinterstehen.
 *
 * In "Bereit" und "Ruht" ist die Statuszeile eine Selbstverstaendlichkeit —
 * dass das Geraet bereit ist, sieht man daran, dass es nichts anderes sagt.
 * Der Platz gehoert dort der Uhrzeit, und die wird so gross, wie die Spalte
 * sie traegt (Montserrat 48: "07:42" misst rund 118 von 150 Pixeln).
 *
 * In "Koppeln" bleibt das Wort stehen: Dass keine Bruecke da ist, sieht man
 * sonst nirgends — und daneben passt die Uhr nur in der kleineren Stufe.
 */
#if !UI_WATCH
#define CLOCK_STATUS_Y (HEADER_H + 4)
#define CLOCK_Y (HEADER_H + 28)
#define CLOCK_HINT_Y (CONTENT_H - 22)
#endif

/*
 * Das Ziffernblatt (Bereit und Ruht) — die Anzeige, die das Geraet die meiste
 * Zeit zeigt, und deshalb die einzige, die nichts Ueberfluessiges tragen darf.
 *
 * Weg ist "Halten zum Sprechen": Der Satz stand dort, seit die Firmware ein
 * Geraet ohne Uhr war. Wer den Stick am Handgelenk traegt, hat ihn beim
 * zweiten Mal gelesen und danach nie wieder gebraucht — den Hinweis, welche
 * Taste spricht, gibt das Geraet ohnehin beim ersten Druck.
 *
 * Weg ist auch Teckis Spalte: Auf dem Ziffernblatt sitzt er klein in der
 * unteren linken Ecke. Er sagt dort dasselbe wie vorher (Augen offen =
 * bereit, geschlossen = ruht), nimmt der Uhrzeit aber nichts mehr weg.
 *
 * Die Uhrzeit bekommt dafuer die ganze Breite und eine eigene Schriftstufe:
 * `todoteck_clock_64` misst "04:44" — die breiteste moegliche Uhrzeit — mit
 * 189 von 224 Pixeln; das haelt auch die schmalste Stelle aus. Die Ziffern
 * sind darin 47 Pixel hoch statt 35 wie bisher in Montserrat 48.
 */
#if !UI_WATCH
/* Untere linke Ecke; die Uhr setzt Tecki stattdessen unten mittig. */
#define CLOCK_FACE_TECKI_X 0
#define CLOCK_FACE_TECKI_Y (CONTENT_H - UI_TECKI_BOX_SMALL)
/*
 * Mittig zwischen Kopfzeile und Unterkante: (103 - 47) / 2 = 28 unter der
 * Kopfzeile. Die Ziffern enden damit genau dort, wo Tecki anfaengt — was
 * nichts ausmacht, weil er in der linken Ecke sitzt und die Uhrzeit
 * waagerecht mittig steht: Selbst die breiteste faengt erst bei Pixel 17 an,
 * die uebliche ("19:11") bei 46.
 */
#define CLOCK_FACE_Y (HEADER_H + 28)
/* Wie oft die Anzeige nachzieht. Sekunden zeigt sie nicht, Minuten genuegen. */
#define CLOCK_TICK_MS 10000
#endif /* !UI_WATCH */

/*
 * Die Schriftleiter fuer den unteren Text, von gross nach klein.
 *
 * Genommen wird die groesste Stufe, auf der die Auskunft noch ganz auf den
 * Schirm passt — vorher gab es nur 16 oder 10, und weil eine laengere Antwort
 * in 16 nie passt, landete praktisch jede in der kleinsten Schrift, mit
 * Weissflaeche darunter. Die Zwischenstufen fuellen genau diese Luecke.
 *
 * Die 16er ist der halbfette Schnitt (600) und traegt zugleich die
 * Statuszeile; die kleineren Stufen sind der normale (500), weil halbfett
 * unter 14 Pixeln zulaeuft.
 *
 * `line_space` steht dabei pro Stufe, nicht global: lv_font_conv rundet die
 * Zeilenhoehe je nach Groesse anders (10->11, 11->14, 12->16, 13->16, 14->18,
 * 16->21), sie traegt also mal mehr, mal weniger Luft fuer Umlautpunkte
 * schon in sich. Die Zahl hier gleicht das auf einen Zeilenabstand von rund
 * dem 1,35-fachen der Schriftgroesse aus.
 */
typedef struct {
    const lv_font_t *font;
    int32_t line_space;
} text_step_t;

#if UI_WATCH
static const text_step_t TEXT_STEPS[] = UI_TEXT_STEPS_INIT;
#else
static const text_step_t TEXT_STEPS[] = {
    { &todoteck_font_16, 2 },
    { &todoteck_font_14, 1 },
    { &todoteck_font_13, 1 },
    { &todoteck_font_12, 0 },
    { &todoteck_font_11, 1 },
    { &todoteck_font_10, 2 },
};
#endif

#define TEXT_STEP_COUNT (sizeof(TEXT_STEPS) / sizeof(TEXT_STEPS[0]))
/* Die kleinste Stufe: Rueckfall, wenn selbst sie ueberlaeuft. */
#define TEXT_STEP_SMALLEST (TEXT_STEPS[TEXT_STEP_COUNT - 1])

/*
 * Schriftgroesse in drei Stufen (nur die Uhr; eingestellt in der App, kommt
 * mit `watch_settings`). Sie waehlt fuer die Auskunft -- Antwort, Verlauf,
 * Transkript -- einen Ausschnitt der Schriftleiter oben, statt neue
 * Schriften mitzubringen:
 *
 *   Stufe     Antwort        Fragezeile   Kartenzeile (ui_cards.c)
 *   s  (0)    26 ... 18      18           20, 7 Zeilen je Seite
 *   m  (1)    30 ... 18      20           23, 6 Zeilen (bisheriges Bild)
 *   l  (2)    30 ... 23      23           26, 5 Zeilen
 *
 * Was bei `l` nicht in 23 passt, bekommt ohnehin die Kurzform mit "mehr"
 * (siehe make_short_form). Die Vollansicht braucht weder Blaettern noch
 * Seitenteilung: Eine Antwort ist hoechstens 191 Byte lang, und die stehen
 * selbst in der 23er ohne Tecki in rund acht Zeilen zu 31 Pixeln -- 250 von
 * mindestens 280 freien Pixeln, auch mit Ja/Nein-Flaechen darunter. Was
 * trotzdem ueberliefe, endet wie bisher mit Auslassungspunkten.
 *
 * Hinweise (UI_TEXT_HINT) folgen der Stufe nicht: Sie sind Beiwerk und
 * bleiben klein.
 */
static uint8_t s_font_level = 1;
#if UI_WATCH
static const uint8_t LEVEL_FIRST_STEP[3] = { 1, 0, 0 };
static const uint8_t LEVEL_LAST_STEP[3] = { 4, 4, 2 };
static const lv_font_t *const LEVEL_QUESTION_FONT[3] = {
    &todoteck_watch_font_18, &todoteck_watch_font_20, &todoteck_watch_font_23,
};
_Static_assert(TEXT_STEP_COUNT == 5, "LEVEL_*_STEP zaehlt fuenf Stufen");
#endif

/* Erste und letzte Stufe, die die Auskunft bei der eingestellten Groesse nimmt. */
static size_t message_first_step(void)
{
#if UI_WATCH
    return LEVEL_FIRST_STEP[s_font_level % 3];
#else
    return 0;
#endif
}

static size_t message_last_step(void)
{
#if UI_WATCH
    return LEVEL_LAST_STEP[s_font_level % 3];
#else
    return TEXT_STEP_COUNT - 1;
#endif
}

/* Schrift der Fragezeile ueber der Antwort -- auf der Uhr nach Stufe. */
static const lv_font_t *question_font(void)
{
#if UI_WATCH
    return LEVEL_QUESTION_FONT[s_font_level % 3];
#else
    return UI_FONT_QUESTION;
#endif
}

typedef struct {
    bool tecki;                    /* bleibt die Figur stehen? */
    const lv_font_t *font;         /* Schrift des unteren Texts */
    int32_t line_space;            /* Zeilenabstand dazu */
    int32_t status_y;              /* Oberkante der Kopfzeile ueber dem Text */
    int32_t x;                     /* linke Kante von Kopfzeile und Text */
    int32_t width;                 /* Breite beider */
    const lv_font_t *status_font;  /* Schrift der Kopfzeile */
    int32_t status_h;              /* Hoehe, die sie belegt */
} text_layout_t;

/*
 * Die Zeile ueber dem Text traegt zweierlei, und beides braucht eine andere
 * Schrift.
 *
 * Ein **Zustandswort** ("Bereit", "Abgebrochen", "Koppeln") ist kurz und die
 * Ueberschrift des Schirms: halbfette 16, eine Zeile. Eine **Frage** ist ein
 * ganzer diktierter Satz und die Bildunterschrift zur Antwort darunter --
 * klein, gedaempft, zwei Zeilen.
 *
 * Der Unterschied ist nicht kosmetisch, er entscheidet, ob die Zeile ueberhaupt
 * etwas sagt: In der 16er passen in die 150 Pixel breite Spalte rund sechzehn
 * Zeichen, aus "Leg eine Aufgabe an, Pool rueckspuelen" wird also "Leg eine
 * Aufgabe...". In der 11er auf zwei Zeilen sind es rund siebzig -- genug fuer
 * den ganzen Satz, und genau der macht einen Verlaufseintrag
 * wiedererkennbar.
 */
#if !UI_WATCH
#define QUESTION_LINES 2
#define QUESTION_LINE_SPACE 1
#endif
#define QUESTION_H (QUESTION_LINES * question_font()->line_height + \
                    (QUESTION_LINES - 1) * QUESTION_LINE_SPACE)

/* Hoehe einer Antwortflaeche und Abstand darueber -- nur die Uhr hat sie. */
#define CHOICE_H 72
#define CHOICE_GAP 10

#if UI_WATCH
/*
 * Was die Uhr sonst noch unter den Text stellt (Masse siehe auch die
 * Trefferflaechen-Tabelle in ui_layout_watch206.h):
 *
 *   Seitenpunkte des Verlaufs  ganz unten, 8 px, eine Reihe
 *   "Tippen fuer mehr"         darueber, MORE_H hoch, wenn die Kurzform steht
 *   "Abbrechen" + Balken       in der Transkript-Szene
 */
#define DOTS_ROW_H 20
#define MORE_H 52
#define MORE_W 280
#define MORE_GAP 8
#define CANCEL_H 64
#define CANCEL_GAP 10
#define CANCEL_BAR_H 6
#define CANCEL_BAR_GAP 12
/*
 * Verlauf: die Zeile mit Marker und Zeit ueber der Frage (META_H hoch) und
 * die Chips "Nochmal" / "Rueckgaengig" unten, nebeneinander, 56 px hoch --
 * ueber der Punktreihe und unter "Tippen fuer mehr". Zwei Chips zu 170 px
 * mit 10 px Luecke sind 350 px breit und beginnen bei x 14 (+16 Rand): Die
 * Mitten liegen bei x 115 und 295, weit weg von den Eckrundungen.
 */
#define META_H 26
#define META_DOT 12
#define CHIP_H 56
#define CHIP_W 170
#define CHIP_GAP 10
#endif

/*
 * Was die laufende Darstellung unten belegt. Gesetzt am Anfang von
 * render_scene_locked(), gelesen von text_bottom_y() -- die Entscheidung
 * "Kurzform ja/nein" faellt erst dort, am gemessenen Text.
 */
static bool s_render_dots;
static bool s_render_answer_dots;
static bool s_render_more;
static bool s_render_meta;
static bool s_transcript_active;

#if UI_WATCH
/*
 * Lange Antwortflaechen (0.5.0, Punkt 43): Vorschlaege wie "Aufgabe: Milch
 * holen" passen nicht zu dritt nebeneinander in die 30er. Dann stehen sie
 * untereinander, volle Breite, in der 20er mit bis zu zwei Zeilen.
 */
#define CHOICE_STACK_H 64
#define CHOICE_STACK_GAP 8
#define CHOICE_STACK_FONT (&todoteck_watch_font_20)

static bool choices_stacked(void)
{
    if (s_choice_count == 0) {
        return false;
    }
    const int32_t w = (CONTENT_W - (s_choice_count - 1) * CHOICE_GAP) / s_choice_count;
    for (int i = 0; i < s_choice_count; i++) {
        if (lv_text_get_width(s_choices[i], (uint32_t)strlen(s_choices[i]), UI_FONT_STATUS, 0) >
            w - 24) {
            return true;
        }
    }
    return false;
}
#endif

/* Was die Antwortflaechen unten belegen, samt Abstand darueber; 0 ohne. */
static int32_t choices_block_h(void)
{
    if (s_choice_count == 0) {
        return 0;
    }
#if UI_WATCH
    if (choices_stacked()) {
        return s_choice_count * CHOICE_STACK_H + (s_choice_count - 1) * CHOICE_STACK_GAP +
               CHOICE_GAP;
    }
#endif
    return CHOICE_H + CHOICE_GAP;
}

/* Unterkante des Texts: ueber allem, was darunter steht. */
static int32_t text_bottom_y(void)
{
    int32_t y = TEXT_BOTTOM_Y - choices_block_h();
#if UI_WATCH
    if (s_render_dots) {
        y -= DOTS_ROW_H;
    }
    if (s_render_answer_dots) {
        y -= DOTS_ROW_H;
    }
    if (s_chip_count > 0) {
        y -= CHIP_H + CHIP_GAP;
    }
    if (s_render_more) {
        y -= MORE_H + MORE_GAP;
    }
    if (s_transcript_active) {
        y -= CANCEL_H + CANCEL_GAP + CANCEL_BAR_H + CANCEL_BAR_GAP;
    }
#else
    (void)s_render_dots;
    (void)s_render_answer_dots;
    (void)s_render_more;
    (void)s_render_meta;
#endif
    return y;
}

/* Hoehe der Marker-Zeile ueber der Frage im Verlauf; 0, wenn keine steht. */
static int32_t meta_h(void)
{
#if UI_WATCH
    return s_render_meta ? META_H : 0;
#else
    return 0;
#endif
}

static int32_t text_height(const char *text, const text_step_t *step, int32_t width)
{
    lv_point_t size;
    lv_text_get_size(&size, text ? text : "", step->font, 0, step->line_space, width,
                     LV_TEXT_FLAG_NONE);
    return size.y;
}

/*
 * Die groesste Stufe zwischen `first` und `last`, auf der der Text noch in
 * `room` passt — sonst NULL.
 */
static const text_step_t *largest_step_fitting(const char *text, int32_t room, int32_t width,
                                               size_t first, size_t last)
{
    for (size_t i = first; i <= last && i < TEXT_STEP_COUNT; i++) {
        if (text_height(text, &TEXT_STEPS[i], width) <= room) {
            return &TEXT_STEPS[i];
        }
    }
    return NULL;
}

/*
 * Wo Statuszeile und Text hinkommen, wie gross er wird — und ob Tecki dabei
 * stehen bleibt.
 *
 * Der Anlass steht auf einem Foto vom Handgelenk: "Aufgabe angelegt:
 * AliExpress Bestellung · Faellig: Di., 25.08.2026 · Projekt: Home Lab"
 * wuchs vom unteren Rand nach oben ueber die Statuszeile und mitten durch die
 * Figur hindurch — lesbar war danach weder das eine noch das andere.
 *
 * Die Entscheidung faellt deshalb am gemessenen Text, nicht an der Szene:
 * Passt er in die Spalte neben Tecki, bleibt die Figur stehen. Passt er
 * nicht, weicht sie und der Text bekommt die ganze Breite. Sie sagt ohnehin
 * dasselbe wie die Statuszeile daneben; der Text sagt etwas, das es nur
 * einmal gibt. Und in beiden Faellen bekommt der Text die groesste Schrift,
 * die der freie Platz noch traegt.
 *
 * Im Querformat faellt die Entscheidung seltener gegen Tecki als hochkant:
 * Die Spalte ist mit 150 Pixeln breiter als frueher der ganze Schirm.
 */
static text_layout_t plan_text_layout(const char *status, const char *question,
                                      const char *text, ui_text_kind_t kind)
{
    const bool with_question = question && question[0];
    const lv_font_t *status_font = with_question ? question_font() : UI_FONT_STATUS;
    const int32_t full_status_h = with_question ? QUESTION_H : UI_FONT_STATUS->line_height;
    const int32_t status_h = (with_question || (status && status[0])) ? full_status_h : 0;
    const int32_t top_beside = STATUS_Y_WITH_TECKI + meta_h() + status_h + TEXT_GAP;
    const int32_t room_beside_tecki = text_bottom_y() - top_beside;

    /* Die Auskunft folgt der eingestellten Schriftgroesse, Beiwerk nicht. */
    const size_t first = kind == UI_TEXT_MESSAGE ? message_first_step() : 0;
    const size_t last = kind == UI_TEXT_MESSAGE ? message_last_step() : TEXT_STEP_COUNT - 1;

    if (kind == UI_TEXT_MESSAGE) {
        const text_step_t *step = largest_step_fitting(text, room_beside_tecki, COL_W,
                                                       first, last);
        if (step) {
            return (text_layout_t){ true, step->font, step->line_space, STATUS_Y_WITH_TECKI,
                                    COL_X, COL_W, status_font, status_h };
        }
    } else if (text_height(text, &TEXT_STEP_SMALLEST, COL_W) <= room_beside_tecki) {
        /* Beiwerk bleibt klein und gedaempft, auch wenn Platz waere. */
        return (text_layout_t){ true, TEXT_STEP_SMALLEST.font, TEXT_STEP_SMALLEST.line_space,
                                STATUS_Y_WITH_TECKI, COL_X, COL_W, status_font, status_h };
    }

    /*
     * Ohne Figur: Kopfzeile nach oben unter den Kopf, der Rest gehoert dem
     * Text ueber die volle Breite. Die Zeile wird immer eingerechnet, auch
     * wenn sie gerade leer ist — sonst spraenge das Layout, sobald sie doch
     * etwas traegt.
     */
    const int32_t top = STATUS_Y_TEXT_ONLY + meta_h() + full_status_h + TEXT_GAP;
    const text_step_t *step = largest_step_fitting(text, text_bottom_y() - top, CONTENT_W,
                                                   first, last);
    if (!step) {
        step = &TEXT_STEPS[last];
    }
    return (text_layout_t){ false, step->font, step->line_space, STATUS_Y_TEXT_ONLY,
                            0, CONTENT_W, status_font, full_status_h };
}

#if UI_WATCH
/*
 * Die Kurzform einer Antwort.
 *
 * Auf dem Handgelenk liest man eine Antwort im Vorbeigehen; drei, vier Zeilen
 * in kleiner werdender Schrift sind dort eine Zumutung. Passt die Antwort
 * nicht in zwei Zeilen der Antwortschrift (der groessten Stufe der
 * eingestellten Groesse), steht zuerst nur ihr erster Satz da -- bis `.`,
 * `!` oder `?`, gefolgt von Leerraum oder dem Ende --, hoechstens aber
 * SHORT_FORM_CHARS Zeichen. Laenger wird an der letzten Wortgrenze davor
 * gekuerzt und bekommt "..." (die Schriften kennen U+2026 nicht, siehe
 * ascii_substitute). Ein Satzende zaehlt erst ab SHORT_FORM_MIN_SENTENCE
 * Byte: "Di. 25.08." oder "Nr. 3" sollen keinen Satz beenden.
 *
 * `false`, wenn die Kurzform die ganze Antwort waere -- dann gibt es nichts
 * "mehr" zu zeigen.
 */
#define SHORT_FORM_CHARS 90
#define SHORT_FORM_MIN_SENTENCE 20
/* 90 Zeichen Latin-1 sind hoechstens 180 Byte (der Text ist schon bereinigt). */
#define SHORT_FORM_MAX (SHORT_FORM_CHARS * 2 + 4)

static bool fits_two_lines(const char *text)
{
    const text_step_t *step = &TEXT_STEPS[message_first_step()];
    const int32_t two_lines = 2 * step->font->line_height + step->line_space;
    return text_height(text, step, CONTENT_W) <= two_lines;
}

static bool make_short_form(char *out, size_t size, const char *text)
{
    const size_t len = strlen(text);
    size_t i = 0;
    size_t chars = 0;
    size_t last_space = 0;
    size_t cut = 0;
    bool ellipsis = false;

    while (i < len) {
        if (chars == SHORT_FORM_CHARS) {
            /* Zu lang fuer einen Satz: an der letzten Wortgrenze kuerzen. */
            cut = last_space >= SHORT_FORM_MIN_SENTENCE ? last_space : i;
            ellipsis = true;
            break;
        }
        const char c = text[i];
        if ((c == '.' || c == '!' || c == '?') && i + 1 >= SHORT_FORM_MIN_SENTENCE &&
            (text[i + 1] == ' ' || text[i + 1] == '\n' || text[i + 1] == '\0')) {
            cut = i + 1;
            break;
        }
        if (c == ' ' || c == '\n') {
            last_space = i;
        }
        /* Ein Zeichen weiter -- nie mitten in ein UTF-8-Zeichen. */
        i++;
        while (i < len && ((unsigned char)text[i] & 0xC0) == 0x80) {
            i++;
        }
        chars++;
    }
    if (cut == 0) {
        return false;
    }
    /* Steht nach dem Schnitt nur noch Leerraum, ist die Kurzform das Ganze. */
    size_t rest = cut;
    while (rest < len && (text[rest] == ' ' || text[rest] == '\n')) {
        rest++;
    }
    if (rest >= len) {
        return false;
    }
    if (ellipsis) {
        /* "Pool messen, ..." liest sich schlechter als "Pool messen ..." */
        while (cut > 0 && (text[cut - 1] == ' ' || text[cut - 1] == ',' ||
                           text[cut - 1] == ':' || text[cut - 1] == ';' ||
                           text[cut - 1] == '-')) {
            cut--;
        }
    }
    const size_t tail = ellipsis ? 3 : 0;
    if (cut + tail + 1 > size) {
        cut = size - tail - 1;
        while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) {
            cut--;
        }
    }
    memcpy(out, text, cut);
    if (ellipsis) {
        memcpy(out + cut, "...", 3);
    }
    out[cut + tail] = '\0';
    return true;
}

/*
 * Punktreihe fuer Seiten: aktiver Punkt gruen, die anderen gedaempft,
 * 8 px Durchmesser, 8 px Abstand. Der Kasten waechst mit der Zahl der
 * Punkte; ausgerichtet wird er einmal beim Anlegen (lv_obj_align merkt sich
 * die Ausrichtung, eine neue Breite verschiebt ihn also mit).
 */
#define DOT_SIZE UI_DOTS_SIZE
#define DOT_GAP 8

void ui_dots_create(ui_dots_t *dots, lv_obj_t *parent)
{
    dots->box = lv_obj_create(parent);
    lv_obj_remove_style_all(dots->box);
    lv_obj_remove_flag(dots->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dots->box, DOT_SIZE, DOT_SIZE);
    for (int i = 0; i < UI_DOTS_MAX; i++) {
        dots->dot[i] = create_blob(dots->box, DOT_SIZE, DOT_SIZE, lv_color_hex(0x3a4656));
        lv_obj_set_pos(dots->dot[i], i * (DOT_SIZE + DOT_GAP), 0);
        lv_obj_add_flag(dots->dot[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(dots->box, LV_OBJ_FLAG_HIDDEN);
}

void ui_dots_set(ui_dots_t *dots, uint8_t count, uint8_t active)
{
    if (!dots->box) {
        return;
    }
    if (count > UI_DOTS_MAX) {
        count = UI_DOTS_MAX;
    }
    if (count == 0) {
        lv_obj_add_flag(dots->box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_width(dots->box, count * DOT_SIZE + (count - 1) * DOT_GAP);
    for (int i = 0; i < UI_DOTS_MAX; i++) {
        if (i >= count) {
            lv_obj_add_flag(dots->dot[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_style_bg_color(dots->dot[i],
                                  i == active ? lv_color_hex(0x2ecc71) : lv_color_hex(0x3a4656),
                                  0);
        lv_obj_remove_flag(dots->dot[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(dots->box, LV_OBJ_FLAG_HIDDEN);
}
#endif /* UI_WATCH */

/*
 * Zeigt diese Szene die Uhr?
 *
 * Nur dort, wo nichts Wichtigeres steht: Koppeln, Bereit und Ruht, und auch
 * dort nur, solange der untere Text Beiwerk ist ("Halten zum Sprechen").
 * Sobald eine Auskunft anliegt, gehoert der Platz ihr — eine Antwort liest
 * man einmal, die Uhrzeit steht beim naechsten Blick wieder da.
 */
static bool clock_scene(ui_status_icon_scene_t scene, ui_text_kind_t kind)
{
    if (kind != UI_TEXT_HINT) {
        return false;
    }
    return scene == UI_STATUS_ICON_IDLE || scene == UI_STATUS_ICON_PAIRING ||
           scene == UI_STATUS_ICON_RESTING;
}

/*
 * Das volle Ziffernblatt: kein Statuswort, kein Hinweis, Tecki klein in der
 * Ecke. "Bereit" und "Ruht" sagen nichts, was der Schirm nicht ohnehin zeigt.
 * "Koppeln" sagt etwas — das behaelt Wort, Hinweis und die grosse Figur.
 */
static bool clock_only_scene(ui_status_icon_scene_t scene, ui_text_kind_t kind,
                             const char *text)
{
    if (text && text[0]) {
        /*
         * Es gibt etwas zu sagen ("Abgebrochen — Nichts gesendet", "Zu kurz",
         * "Nichts gehoert"). Dann bleibt es beim kleineren Zifferblatt mit
         * Statuswort und Zeile darunter: Ein Geraet, das auf einen Abbruch
         * hin nur die Uhrzeit zeigt, sieht aus, als haette es die Taste gar
         * nicht bemerkt.
         */
        return false;
    }
    return clock_scene(scene, kind) &&
           (scene == UI_STATUS_ICON_IDLE || scene == UI_STATUS_ICON_RESTING);
}

/*
 * "07:42" aus der Systemzeit plus gemeldetem Abstand zur UTC — und "--:--",
 * solange keine Zeit gestellt wurde.
 *
 * Die Striche statt einer leeren Flaeche sind Absicht: Ohne sie sieht ein
 * Geraet, dessen Bruecke die Zeit nicht schickt, genauso aus wie eines mit
 * alter Firmware. Genau diese Verwechslung ist beim ersten Praxistest
 * passiert. Eine geratene Uhrzeit waere schlechter als keine — "unbekannt"
 * anzuzeigen ist etwas anderes als zu raten.
 */
static void format_clock(char *out, size_t size)
{
    if (!s_clock_valid) {
        snprintf(out, size, "--:--");
        return;
    }
    const time_t now = time(NULL) + (time_t)s_clock_offset_min * 60;
    struct tm parts;
    gmtime_r(&now, &parts);
    snprintf(out, size, "%02d:%02d", parts.tm_hour, parts.tm_min);
}

/*
 * Faerbt die Akkuzahl. Sie traegt seit dem Wegfall des Symbols beides: den
 * Stand als Ziffern und den Zustand als Farbe.
 */
static void apply_battery_colour_locked(void)
{
    if (!s_battery_label) {
        return;
    }
    /* Nachts auf dem Zifferblatt bernsteinfarben gedaempft -- nur das Akku-Rot bleibt Rot. */
    lv_obj_set_style_text_color(s_battery_label,
                                s_battery_low ? lv_color_hex(0xf97373) :
                                s_face_tint_shown ? lv_color_hex(NIGHT_TINT_MUTED) :
                                s_battery_charging ? lv_color_hex(0x5ec4ff) :
                                s_muted_colour,
                                0);
#if UI_WATCH
    /* Der Akku-Punkt des Layouts "Minimal": gruen, ab 20 % gelb, ab 10 % rot, am Kabel blau. */
    if (s_battery_dot) {
        const uint32_t dot = s_battery_low ? 0xe74c3c :
                             s_face_tint_shown ? NIGHT_TINT_MUTED :
                             s_battery_charging ? 0x5ec4ff :
                             s_battery_percent >= 0 && s_battery_percent <= 20 ? 0xf2b23c :
                             0x2ecc71;
        lv_obj_set_style_bg_color(s_battery_dot, lv_color_hex(dot), 0);
    }
#endif
}

#if UI_WATCH
/* Ortsstunde aus Systemzeit und gemeldetem Abstand; -1 ohne gestellte Uhr. */
static int local_hour(void)
{
    if (!s_clock_valid) {
        return -1;
    }
    const time_t now = time(NULL) + (time_t)s_clock_offset_min * 60;
    struct tm parts;
    gmtime_r(&now, &parts);
    return parts.tm_hour;
}
#endif

/*
 * Der Termin als eine Zeile, Tag und Zeit vorn, der Titel dahinter:
 * "Jetzt: Zahnarzt", "in 25 min Zahnarzt", "Heute 09:30 Zahnarzt",
 * "Morgen 09:00 Zahnarzt", "Mo 09:00 Zahnarzt", ab einer Woche "12.11.
 * 09:00 Zahnarzt". Ganztaegig (Beginn 00:00 Ortszeit) ohne Uhrzeit:
 * "Heute Urlaub", "Morgen Urlaub", "Mo Urlaub" -- und "Heute" steht dann
 * den ganzen Tag, nicht nur bis eine halbe Stunde nach Mitternacht.
 *
 * Bis 11/2026 stand nur "Sugarbabes 09:00" da; um 10:36 war damit der
 * Termin morgen gemeint, und das las sich wie einer, den man verpasst hat.
 * Der Tag steht deshalb immer dabei, und zwar vorn: Ein langer Titel wird
 * hinten mit "..." gekuerzt (das Label kuerzt ohnehin am Ende), Tag und
 * Uhrzeit nie. Gerechnet wird in Ortszeit ueber den Versatz, den die
 * Bruecke mit `time` schickt.
 *
 * Leer, wenn keiner ansteht oder er schon eine halbe Stunde laeuft.
 */
#define EVENT_LINE_CHARS 40

/* Ortstag als fortlaufende Zahl (Tage seit 1970 in Ortszeit). */
static int64_t local_day(int64_t epoch_utc)
{
    const int64_t local = epoch_utc + (int64_t)s_clock_offset_min * 60;
    return local >= 0 ? local / 86400 : (local - 86399) / 86400;
}

/* `prefix` + " " + Titel, der Titel UTF-8-sauber auf den Rest der Zeile gekuerzt. */
static void join_event_line(char *out, size_t size, const char *prefix)
{
    size_t prefix_chars = strlen(prefix);  /* Praefixe sind reines ASCII */
    size_t budget = EVENT_LINE_CHARS > prefix_chars + 1 ? EVENT_LINE_CHARS - prefix_chars - 1 : 4;
    size_t title_chars = 0;
    for (const unsigned char *p = (const unsigned char *)s_event_title; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            title_chars++;
        }
    }
    char title[UI_STATUS_FACE_LEN + 4];
    size_t n = 0;
    if (title_chars <= budget) {
        snprintf(title, sizeof(title), "%s", s_event_title);
    } else {
        /* Platz fuer "..." lassen und nur an Zeichengrenzen schneiden. */
        size_t keep = budget > 3 ? budget - 3 : 1;
        size_t chars = 0;
        const char *src = s_event_title;
        while (src[n] != '\0') {
            if (((unsigned char)src[n] & 0xC0) != 0x80) {
                if (chars == keep) {
                    break;
                }
                chars++;
            }
            n++;
        }
        while (n > 0 && src[n - 1] == ' ') {
            n--;
        }
        if (n > sizeof(title) - 4) {
            n = sizeof(title) - 4;
        }
        memcpy(title, src, n);
        memcpy(title + n, "...", 4);
    }
    snprintf(out, size, "%s %s", prefix, title);
}

static void format_event(char *out, size_t size)
{
    static const char *const WEEKDAYS[7] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };
    out[0] = '\0';
    if (!s_event_title[0] || s_event_start <= 0 || !s_clock_valid) {
        return;
    }
    const int64_t now = (int64_t)time(NULL);
    const int64_t delta_min = (s_event_start - now) / 60;
    const time_t start = (time_t)s_event_start + (time_t)s_clock_offset_min * 60;
    struct tm parts;
    gmtime_r(&start, &parts);
    const bool all_day = parts.tm_hour == 0 && parts.tm_min == 0;
    const int64_t days = local_day(s_event_start) - local_day(now);

    char prefix[32];
    if (all_day) {
        /* Ganztaegig: Heute gilt bis Mitternacht, vorbei ist erst gestern. */
        if (days < 0) {
            return;
        }
    } else if (delta_min < -30) {
        return;
    } else if (delta_min <= 0) {
        snprintf(out, size, "Jetzt: %.30s", s_event_title);
        return;
    } else if (delta_min < 60) {
        snprintf(prefix, sizeof(prefix), "in %u min", (unsigned)delta_min % 60u);
        join_event_line(out, size, prefix);
        return;
    }

    char day[16];
    if (days == 0) {
        snprintf(day, sizeof(day), "Heute");
    } else if (days == 1) {
        snprintf(day, sizeof(day), "Morgen");
    } else if (days < 7) {
        /* Innerhalb einer Woche ist der Wochentag eindeutig. */
        snprintf(day, sizeof(day), "%s", WEEKDAYS[parts.tm_wday % 7]);
    } else {
        snprintf(day, sizeof(day), "%02u.%02u.", (unsigned)parts.tm_mday % 32u,
                 (unsigned)(parts.tm_mon + 1) % 13u);
    }
    if (all_day) {
        snprintf(prefix, sizeof(prefix), "%s", day);
    } else {
        snprintf(prefix, sizeof(prefix), "%s %02u:%02u", day, (unsigned)parts.tm_hour % 24u,
                 (unsigned)parts.tm_min % 60u);
    }
    join_event_line(out, size, prefix);
}

/*
 * Die Zeilen unter der Uhrzeit: zuerst der Termin (er laeuft ab), dann was
 * die Bruecke geschickt hat. Nur die Uhr legt die Labels an. Sie haengen
 * an der festen Stelle der Uhrzeit (CLOCK_FACE_Y) -- die Uhrzeit weicht
 * ihnen nicht aus, sondern bleibt stehen, ob Zeilen da sind oder nicht.
 */
#define FACE_LINE_TOP (CLOCK_FACE_Y + 100)
#define FACE_LINE_H 34

/*
 * Um wie viele Pixel ein zentriertes Label nach rechts muss (negativ: nach
 * links), damit die **Tinte** der Zeile mittig steht und nicht die Summe
 * der Vorschuebe. LVGL zentriert letztere; bei einer proportionalen Schrift
 * liegt die Tinte darin nicht mittig -- die "1" der 120er traegt rechts
 * 11 px Luft und links keine, "11:47" stand damit sichtbar links. Gerechnet
 * wird mit den Glyphmassen der Schrift (Vorschub, Kasten, Versatz, alles in
 * px, so wie lv_font_get_glyph_dsc sie liefert): linke Tintenkante ist der
 * kleinste, rechte der groesste Kastenrand. Nur fuer ASCII -- die Uhrzeit
 * besteht aus Ziffern, Doppelpunkt und Strich.
 */
static int32_t ink_center_shift(const lv_font_t *font, const char *text)
{
    int32_t pen = 0;
    int32_t ink_left = 0;
    int32_t ink_right = 0;
    bool any = false;
    for (size_t i = 0; text[i]; i++) {
        lv_font_glyph_dsc_t g;
        const uint32_t letter = (uint8_t)text[i];
        const uint32_t next = (uint8_t)text[i + 1];
        if (!lv_font_get_glyph_dsc(font, &g, letter, next)) {
            continue;
        }
        const int32_t left = pen + g.ofs_x;
        const int32_t right = left + g.box_w;
        if (!any || left < ink_left) {
            ink_left = left;
        }
        if (!any || right > ink_right) {
            ink_right = right;
        }
        any = true;
        pen += g.adv_w;
    }
    if (!any) {
        return 0;
    }
    /* pen ist die Zeilenbreite, die LVGL mittig setzt. */
    return (pen - ink_left - ink_right) / 2;
}

/*
 * Die grosse Uhrzeit setzen: Text, dann das Label so verschieben, dass die
 * Tinte mittig in der Spalte `x .. x + width` steht. Das Label wird dabei um
 * den doppelten Versatz schmaler, damit es die Spalte nicht verlaesst --
 * sonst scrollte der Schirm. Die Spalte merkt sich das Label fuer den
 * Minutentakt (clock_tick_cb), der nur den Text austauscht.
 */
static int32_t s_clock_col_x;
static int32_t s_clock_col_w;
static int32_t s_clock_col_y;

static bool s_clock_cells_shown;

#if UI_WATCH
/*
 * Die grosse Uhrzeit zellenweise (nur auf dem Ziffernblatt): Zellbreite ist
 * der breiteste Ziffern-Vorschub der Schrift, der Doppelpunkt bekommt seinen
 * eigenen; in jeder Zelle steht die Tinte der Ziffer mittig
 * (ink_center_shift je Zeichen). Fuenf Zeichen sind es immer -- "HH:MM" oder
 * "--:--" --, sonst faellt die Zeile auf das eine Label zurueck.
 */
static void place_clock_cells_locked(const char *text, const lv_font_t *font)
{
    int32_t cell = 0;
    for (char c = '0'; c <= '9'; c++) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)c, 0) && g.adv_w > cell) {
            cell = g.adv_w;
        }
    }
    lv_font_glyph_dsc_t colon;
    const int32_t colon_w = lv_font_get_glyph_dsc(font, &colon, ':', 0) ? colon.adv_w : cell / 2;
    const int32_t total = 4 * cell + colon_w;
    int32_t x = s_clock_col_x + (s_clock_col_w - total) / 2;
    for (int i = 0; i < CLOCK_CELLS; i++) {
        const char one[2] = { text[i], '\0' };
        const int32_t w = i == 2 ? colon_w : cell;
        lv_label_set_text(s_clock_cell[i], one);
        lv_obj_set_style_text_font(s_clock_cell[i], font, 0);
        lv_obj_set_width(s_clock_cell[i], w);
        lv_obj_align(s_clock_cell[i], LV_ALIGN_TOP_LEFT, x + ink_center_shift(font, one),
                     s_clock_col_y);
        lv_obj_remove_flag(s_clock_cell[i], LV_OBJ_FLAG_HIDDEN);
        x += w;
    }
}
#endif

static void place_clock_label_locked(const char *text)
{
    strlcpy(s_clock_text, text, sizeof(s_clock_text));
#if UI_WATCH
    if (s_clock_cells_shown && strlen(text) == CLOCK_CELLS && s_clock_cell[0]) {
        place_clock_cells_locked(text, UI_FONT_CLOCK_FACE);
        lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    for (int i = 0; i < CLOCK_CELLS; i++) {
        if (s_clock_cell[i]) {
            lv_obj_add_flag(s_clock_cell[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
    lv_label_set_text(s_clock_label, text);
    const lv_font_t *font = lv_obj_get_style_text_font(s_clock_label, LV_PART_MAIN);
    const int32_t shift = ink_center_shift(font, text);
    const int32_t inset = shift < 0 ? -shift : shift;
    lv_obj_set_width(s_clock_label, s_clock_col_w - 2 * inset);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_LEFT, s_clock_col_x + shift + inset, s_clock_col_y);
    lv_obj_remove_flag(s_clock_label, LV_OBJ_FLAG_HIDDEN);
}

/* Uhrzeit weg -- Label und Zellen. */
static void hide_clock_locked(void)
{
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_HIDDEN);
#if UI_WATCH
    for (int i = 0; i < CLOCK_CELLS; i++) {
        if (s_clock_cell[i]) {
            lv_obj_add_flag(s_clock_cell[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

static void render_face_lines_locked(bool visible, const char *event_line)
{
    if (!s_face_label[0]) {
        return;
    }
    int row = 0;
    for (int i = 0; i < FACE_LABELS; i++) {
        lv_obj_add_flag(s_face_label[i], LV_OBJ_FLAG_HIDDEN);
        s_face_label_kind[i] = UI_FACE_HIT_NONE;
    }
    if (!visible) {
        return;
    }
    if (s_face_locked) {
        /* Gesperrt: zwei Zeilen statt Termin und Zeilen -- nichts davon ist antippbar. */
        lv_label_set_text(s_face_label[0], "Gesperrt");
        lv_obj_set_style_text_color(s_face_label[0], lv_color_hex(0xe74c3c), 0);
        lv_obj_align(s_face_label[0], LV_ALIGN_TOP_MID, 0, FACE_LINE_TOP);
        lv_obj_remove_flag(s_face_label[0], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_face_label[1], "In der App neu koppeln");
        lv_obj_set_style_text_color(s_face_label[1],
                                    lv_color_hex(s_face_tint_shown ? NIGHT_TINT_MUTED : 0xc9d1d9), 0);
        lv_obj_align(s_face_label[1], LV_ALIGN_TOP_MID, 0, FACE_LINE_TOP + FACE_LINE_H);
        lv_obj_remove_flag(s_face_label[1], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (event_line[0]) {
        s_face_label_kind[row] = s_face_kind_count > 0 ? UI_FACE_HIT_EVENT : UI_FACE_HIT_NONE;
        lv_label_set_text(s_face_label[row], event_line);
        lv_obj_set_style_text_color(s_face_label[row],
                                    lv_color_hex(s_face_tint_shown ? NIGHT_TINT_TEXT : 0x5ec4ff), 0);
        lv_obj_align(s_face_label[row], LV_ALIGN_TOP_MID, 0, FACE_LINE_TOP);
        lv_obj_remove_flag(s_face_label[row], LV_OBJ_FLAG_HIDDEN);
        row++;
    }
    for (int i = 0; i < s_face_count && row < FACE_LABELS - 1; i++, row++) {
        s_face_label_kind[row] = i < s_face_kind_count ? s_face_kinds[i] : UI_FACE_HIT_NONE;
        lv_label_set_text(s_face_label[row], s_face[i]);
        lv_obj_set_style_text_color(s_face_label[row],
                                    lv_color_hex(s_face_tint_shown ? NIGHT_TINT_MUTED : 0xc9d1d9), 0);
        lv_obj_align(s_face_label[row], LV_ALIGN_TOP_MID, 0, FACE_LINE_TOP + row * FACE_LINE_H);
        lv_obj_remove_flag(s_face_label[row], LV_OBJ_FLAG_HIDDEN);
    }
    /* Akku knapp: die Restzeit rot als letzte Zeile (bei <= 20 %, main.c entscheidet). */
    if (s_battery_remaining[0] && row < FACE_LABELS) {
        lv_label_set_text(s_face_label[row], s_battery_remaining);
        lv_obj_set_style_text_color(s_face_label[row], lv_color_hex(0xe74c3c), 0);
        lv_obj_align(s_face_label[row], LV_ALIGN_TOP_MID, 0, FACE_LINE_TOP + row * FACE_LINE_H);
        lv_obj_remove_flag(s_face_label[row], LV_OBJ_FLAG_HIDDEN);
    }
}

#if UI_WATCH
/*
 * Die zwei anderen Layouts des Zifferblatts (0.4.4). "Minimal": die Uhrzeit
 * in Zellen wie gewohnt, nur tiefer, darunter das Datum ("So 1.11.") und der
 * Akku als Punkt -- keine Zeilen, kein Termin, kein Tecki. "Gross": Stunden
 * und Minuten untereinander in der 180er, darunter genau eine Zeile -- der
 * Termin, sonst die erste Zeile der Bruecke; der Akku bleibt klein oben.
 */
#define MINIMAL_CLOCK_Y 150
#define MINIMAL_DATE_Y (MINIMAL_CLOCK_Y + 87 + 18)
#define MINIMAL_DOT 16
#define MINIMAL_DOT_Y (MINIMAL_DATE_Y + 36 + 22)
#define LARGE_HH_Y (HEADER_H + 14)
#define LARGE_MM_Y (LARGE_HH_Y + 136)
#define LARGE_LINE_Y (LARGE_MM_Y + 130 + 24)

/* "So 1.11." in Ortszeit; leer, solange die Uhr nicht gestellt ist. */
static void format_face_date(char *out, size_t size)
{
    static const char *const WEEKDAYS[7] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };
    out[0] = '\0';
    if (!s_clock_valid) {
        return;
    }
    const time_t now = time(NULL) + (time_t)s_clock_offset_min * 60;
    struct tm parts;
    gmtime_r(&now, &parts);
    snprintf(out, size, "%s %u.%u.", WEEKDAYS[parts.tm_wday % 7], (unsigned)parts.tm_mday % 32u,
             (unsigned)(parts.tm_mon + 1) % 13u);
}

static void hide_alt_face_locked(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_large_label[i]) {
            lv_obj_add_flag(s_large_label[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_face_date) {
        lv_obj_add_flag(s_face_date, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_battery_dot) {
        lv_obj_add_flag(s_battery_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_alt_face_locked(ui_face_layout_t layout, const char *clock_text,
                                   const char *event_line)
{
    hide_alt_face_locked();
    render_face_lines_locked(false, "");
    if (!s_large_label[0] || !s_face_date || !s_battery_dot) {
        return;
    }
    if (layout == UI_FACE_MINIMAL) {
        s_clock_col_x = 0;
        s_clock_col_w = CONTENT_W;
        s_clock_col_y = MINIMAL_CLOCK_Y;
        s_clock_cells_shown = true;
        place_clock_label_locked(clock_text);
        static char date[24]; /* unter s_lvgl_lock */
        format_face_date(date, sizeof(date));
        if (s_face_locked) {
            /* Minimal hat keine Zeilen: das Datum weicht dem Hinweis. */
            strlcpy(date, "Gesperrt", sizeof(date));
        }
        lv_label_set_text(s_face_date, date);
        lv_obj_align(s_face_date, LV_ALIGN_TOP_MID, 0, MINIMAL_DATE_Y);
        if (date[0]) {
            lv_obj_remove_flag(s_face_date, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align(s_battery_dot, LV_ALIGN_TOP_MID, 0, MINIMAL_DOT_Y);
        if (s_battery_percent >= 0) {
            lv_obj_remove_flag(s_battery_dot, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    /* Gross: die Uhrzeit nicht in Zellen, sondern als zwei Zeilen "HH" und "MM". */
    hide_clock_locked();
    s_clock_cells_shown = false;
    strlcpy(s_clock_text, clock_text, sizeof(s_clock_text));
    const bool full = strlen(clock_text) == CLOCK_CELLS;
    for (int i = 0; i < 2; i++) {
        const size_t at = i == 0 ? 0 : 3;
        const char part[3] = { full ? clock_text[at] : '-', full ? clock_text[at + 1] : '-',
                               '\0' };
        lv_label_set_text(s_large_label[i], part);
        lv_obj_align(s_large_label[i], LV_ALIGN_TOP_MID,
                     ink_center_shift(UI_FONT_CLOCK_LARGE, part), i == 0 ? LARGE_HH_Y : LARGE_MM_Y);
        lv_obj_remove_flag(s_large_label[i], LV_OBJ_FLAG_HIDDEN);
    }
    const bool is_event = event_line[0] != '\0' && !s_face_locked;
    const char *line = s_face_locked ? "Gesperrt" : is_event ? event_line :
                       s_face_count > 0 ? s_face[0] : "";
    if (s_face_kind_count > 0 && !s_face_locked) {
        s_face_label_kind[0] = is_event ? UI_FACE_HIT_EVENT : s_face_kinds[0];
    }
    if (line[0] && s_face_label[0]) {
        lv_label_set_text(s_face_label[0], line);
        lv_obj_set_style_text_color(
            s_face_label[0],
            lv_color_hex(is_event ? (s_face_tint_shown ? NIGHT_TINT_TEXT : 0x5ec4ff)
                                  : (s_face_tint_shown ? NIGHT_TINT_MUTED : 0xc9d1d9)),
            0);
        lv_obj_align(s_face_label[0], LV_ALIGN_TOP_MID, 0, LARGE_LINE_Y);
        lv_obj_remove_flag(s_face_label[0], LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

/*
 * Welche Pose Tecki tatsaechlich zeigt.
 *
 * Auf dem Ziffernblatt und in der Bereit-Szene ist Tecki der Statustraeger
 * der Uhr, mit fester Rangfolge -- das Wichtigere verdeckt das Unwichtigere:
 *
 *   1. Update laeuft                 Augen zu, Punkte   (UPDATING)
 *   2. keine Verbindung zur Bruecke  schaut suchend     (PAIRING)
 *   3. Akku <= 15 %, ohne Laden      muede              (TIRED)
 *   4. Stimmung (Antwort, Wischen)   freut sich, schaut (HAPPY, LOOK_*)
 *   5. Szene (nachts schlaeft er)    wie gehabt
 *
 * Die 15 % sind dieselbe Schwelle, ab der das Energiesparen anspringt
 * (SAVER_AUTO_PERCENT in main.c). Der StickS3 behaelt sein bisheriges
 * Verhalten: Stimmung, dann Szene.
 */
#define TECKI_TIRED_PERCENT 15

static ui_status_icon_scene_t icon_scene_for(ui_status_icon_scene_t scene, bool clock_only)
{
    if (scene == UI_STATUS_ICON_IDLE || clock_only) {
#if UI_WATCH
        if (s_updating) {
            return UI_STATUS_ICON_UPDATING;
        }
        if (!s_link_connected) {
            return UI_STATUS_ICON_PAIRING;
        }
        if (s_battery_percent >= 0 && s_battery_percent <= TECKI_TIRED_PERCENT &&
            !s_battery_charging) {
            return UI_STATUS_ICON_TIRED;
        }
#endif
        switch (s_mood) {
        case UI_STATUS_MOOD_HAPPY:
            return UI_STATUS_ICON_HAPPY;
        case UI_STATUS_MOOD_LOOK_LEFT:
            return UI_STATUS_ICON_LOOK_LEFT;
        case UI_STATUS_MOOD_LOOK_RIGHT:
            return UI_STATUS_ICON_LOOK_RIGHT;
        default:
            break;
        }
    }
#if UI_WATCH
    /*
     * Nachts schlaeft er: Auf dem Ziffernblatt zwischen 22 und 6 Uhr hat
     * Tecki die Augen zu -- dieselbe Pose wie "Ruht", nur ohne den Schirm
     * abzudunkeln, den man ja gerade ansieht.
     */
    const int hour = local_hour();
    if (clock_only && hour >= 0 && (hour >= 22 || hour < 6)) {
        return UI_STATUS_ICON_RESTING;
    }
#endif
    return scene;
}

#if UI_WATCH
typedef struct {
    int32_t pitch;     /* Zeilenhoehe plus Abstand */
    int32_t per_page;  /* ganze Zeilen je Ausschnitt */
    uint8_t pages;     /* Ausschnitte, hoechstens UI_DOTS_MAX */
} step_lines_t;

/* Wie viele Zeilen der Text in der geplanten Schrift braucht und wie viele in `room` passen. */
static step_lines_t answer_lines(const char *text, const text_layout_t *plan, int32_t room)
{
    step_lines_t out;
    const text_step_t step = { plan->font, plan->line_space };
    out.pitch = lv_font_get_line_height(plan->font) + plan->line_space;
    if (out.pitch < 1) {
        out.pitch = 1;
    }
    /* n Zeilen brauchen n * Zeilenhoehe + (n - 1) * Abstand. */
    out.per_page = (room + plan->line_space) / out.pitch;
    if (out.per_page < 1) {
        out.per_page = 1;
    }
    const int32_t total = (text_height(text, &step, plan->width) + plan->line_space) / out.pitch;
    int32_t pages = (total + out.per_page - 1) / out.per_page;
    if (pages < 1) {
        pages = 1;
    }
    out.pages = (uint8_t)(pages > UI_DOTS_MAX ? UI_DOTS_MAX : pages);
    return out;
}
#endif

static void render_scene_locked(ui_status_icon_scene_t scene, const char *status,
                                const char *question, const char *hint)
{
    if (!s_ready) {
        return;
    }

    const bool with_clock = clock_scene(scene, s_text_kind);
    const bool clock_only = clock_only_scene(scene, s_text_kind, hint);
    /* Denk-Szene: ab fuenf Sekunden mit Zaehler ("Erledige ... 7 s"). */
    status = thinking_status_locked(scene, status ? status : "");
    /* Nachtlade-Modus: nur auf dem Ziffernblatt, nur die Uhrzeit, dunkelgrau. */
    const bool night = UI_WATCH && s_night_mode && clock_only;
    /* Nachtmodus-Fenster (0.4.4): das Zifferblatt in Bernstein; der Nachtlade-Modus auch. */
    const bool tint = UI_WATCH && s_night_tint && clock_only;
    /* Das Layout gilt nur fuer das Zifferblatt; der Nachtlade-Modus hat Vorrang. */
    const ui_face_layout_t layout = UI_WATCH && clock_only && !night ? s_face_layout : UI_FACE_INFO;
    s_main_clock_shown = with_clock;
    s_face_shown = clock_only;
    s_face_tint_shown = tint;

    const char *body_text = hint ? hint : "";
    /*
     * Was unten steht, bevor gemessen wird: Seitenpunkte im Verlauf, und --
     * wenn die Antwort nicht in zwei Zeilen passt -- die Kurzform samt
     * "Tippen fuer mehr". Rueckfragen haben Vorrang: Mit offenen
     * Antwortflaechen steht immer die ganze Frage da.
     */
    s_render_dots = UI_WATCH && !clock_only && s_position_text[0] != '\0';
    s_render_answer_dots = false;
    s_render_more = false;
    s_render_meta = false;
#if UI_WATCH
    /*
     * Verlaufseintrag: Marker-Zeile oben, Chips unten -- beides vor dem
     * Messen, weil es dem Text Platz nimmt. "Nochmal" nur bei einem Turn mit
     * Frage; "Rueckgaengig" solange die Bruecke den Turn (ref) oder die
     * Karten-Aktion noch zuruecknimmt: 15 Minuten ab dem Eintrag.
     */
    s_chip_count = 0;
    s_render_meta = !clock_only && s_meta_shown;
    if (s_render_meta) {
        const ui_history_entry_t *e = &s_meta_entry;
        const int64_t now = (int64_t)time(NULL);
        const bool fresh = e->at > 0 && s_clock_valid && now - e->at <= HISTORY_UNDO_WINDOW_S;
        if (e->kind == UI_HISTORY_TURN && e->question[0]) {
            s_chip_action[s_chip_count++] = UI_HISTORY_CHIP_REPEAT;
        }
        if (fresh && !e->undone) {
            if (e->kind == UI_HISTORY_CARD) {
                s_chip_action[s_chip_count++] = UI_HISTORY_CHIP_UNDO_CARD;
            } else if (e->ref != 0) {
                s_chip_action[s_chip_count++] = UI_HISTORY_CHIP_UNDO_TURN;
            }
        }
    }
    /* Statisch statt auf dem Stack: gerendert wird auch aus dem Bluetooth-Strang. */
    static char short_text[SHORT_FORM_MAX];
    if (!clock_only && s_shortable && !s_expanded && s_choice_count == 0 &&
        s_text_kind == UI_TEXT_MESSAGE && !fits_two_lines(body_text) &&
        make_short_form(short_text, sizeof(short_text), body_text)) {
        body_text = short_text;
        s_render_more = true;
    }
#endif

    /*
     * Flaeche und Ecke vor dem Zeichnen: Auf dem Ziffernblatt schrumpft Tecki
     * in die untere linke Ecke, sonst steht er in seiner Spalte. Erst danach
     * apply(), weil das die Punkte in die Objekte schreibt.
     */
    ui_status_icons_set_box(&s_icons,
                            clock_only ? UI_TECKI_BOX_SMALL : UI_TECKI_BOX,
                            clock_only ? CLOCK_FACE_TECKI_X : TECKI_X,
                            clock_only ? CLOCK_FACE_TECKI_Y : TECKI_TOP_Y);
    ui_status_icons_apply(&s_icons, icon_scene_for(scene, clock_only));
    if (tint) {
        ui_status_icons_tint(&s_icons, NIGHT_TINT_TEXT, NIGHT_TINT_MUTED);
    }

    const char *question_text = (question && question[0] && !clock_only) ? question : "";
    text_layout_t plan = plan_text_layout(status, question_text, body_text, s_text_kind);
#if UI_WATCH
    /*
     * Passt der Text selbst in der kleinsten erlaubten Stufe nicht, kommen
     * Ausschnitte mit Seitenpunkten (Punkt 42) -- deren Reihe nimmt dem Text
     * eine Zeile Platz, also danach noch einmal planen.
     */
    if (!clock_only && !with_clock && !plan.tecki) {
        const text_step_t step = { plan.font, plan.line_space };
        const int32_t top = plan.status_y + meta_h() + plan.status_h + TEXT_GAP;
        if (text_height(body_text, &step, plan.width) > text_bottom_y() - top) {
            s_render_answer_dots = true;
            plan = plan_text_layout(status, question_text, body_text, s_text_kind);
        }
    }
#endif

    /*
     * Ohne Tecki traegt die Statuszeile allein, dass etwas schiefging — die
     * Fehlerszene laesst sie sonst absichtlich leer, weil die rote Figur mit
     * den Kreuzaugen es sagt. Ist die weg, muss es jemand anderes sagen.
     */
    const char *status_shown = status ? status : "";
    if (!plan.tecki && !status_shown[0] && scene == UI_STATUS_ICON_ERROR) {
        status_shown = "Fehler";
    }

    /* Auf dem Zifferblatt steht Tecki nur im Layout "Info". */
    ui_status_icons_show(&s_icons,
                         (clock_only ? layout == UI_FACE_INFO : plan.tecki) && !night);

    /*
     * Die Zeile ueber dem Text: die Frage, wenn es eine gibt, sonst das
     * Zustandswort. Beides zugleich waere eine Zeile zu viel — die Frage sagt
     * ohnehin schon, dass eine Antwort da ist.
     *
     * Die Anfuehrungszeichen sind das einzige, was die Frage als Zitat
     * ausweist: "Pool rueckspuelen am Samstag" ueber "Aufgabe angelegt: ..."
     * liesse sonst offen, welcher der beiden Saetze vom Geraet kommt.
     */
    /*
     * static: Gerendert wird nur unter s_lvgl_lock, die Puffer brauchen
     * deshalb keinen eigenen Platz je Aufruf -- und render_scene_locked()
     * laeuft auf dem Stack des jeweiligen Aufrufers (LVGL-Task, Ereignis-
     * Task, app_main beim Start), wo gut 200 Byte lokale Puffer dazu
     * den Unterschied zwischen knapp und uebergelaufen machen koennen.
     */
    static char quoted[UI_QUESTION_MAX + 3];
    quoted[0] = '\0';
    if (question_text[0]) {
        snprintf(quoted, sizeof(quoted), "\"%s\"", question_text);
    }
    lv_label_set_text(s_status_label, clock_only ? "" :
                      (question_text[0] ? quoted : status_shown));
    lv_obj_set_style_text_font(s_status_label, plan.status_font, 0);
    lv_obj_set_style_text_line_space(s_status_label, QUESTION_LINE_SPACE, 0);
    lv_obj_set_width(s_status_label, plan.width);
    lv_obj_set_height(s_status_label, plan.status_font->line_height *
                      (question_text[0] ? QUESTION_LINES : 1) +
                      (question_text[0] ? QUESTION_LINE_SPACE : 0));
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, plan.x,
                 with_clock ? CLOCK_STATUS_Y : plan.status_y + meta_h());

    if (with_clock) {
        char clock_text[8];
        format_clock(clock_text, sizeof(clock_text));
        lv_obj_set_style_text_font(s_clock_label,
                                   clock_only ? UI_FONT_CLOCK_FACE : UI_FONT_CLOCK_SMALL, 0);
        static char event_line[UI_STATUS_FACE_LEN + 40]; /* unter s_lvgl_lock, siehe quoted */
        format_event(event_line, sizeof(event_line));
        const bool face_info = clock_only && !night && s_face_label[0] != NULL &&
                               (s_face_count > 0 || event_line[0] || s_battery_remaining[0] || s_face_locked);
#if UI_WATCH
        if (layout != UI_FACE_INFO) {
            render_alt_face_locked(layout, clock_text, event_line);
        } else
#endif
        {
#if UI_WATCH
            hide_alt_face_locked();
#endif
            s_clock_col_x = clock_only ? 0 : plan.x;
            s_clock_col_w = clock_only ? CONTENT_W : plan.width;
            s_clock_col_y = clock_only ? CLOCK_FACE_Y : CLOCK_Y;
            s_clock_cells_shown = UI_WATCH && clock_only;
            place_clock_label_locked(clock_text);
            render_face_lines_locked(face_info, event_line);
        }
    } else {
        hide_clock_locked();
        render_face_lines_locked(false, "");
#if UI_WATCH
        hide_alt_face_locked();
#endif
    }

    lv_label_set_text(s_hint_label, body_text);
    lv_obj_set_style_text_font(s_hint_label, plan.font, 0);
    lv_obj_set_style_text_line_space(s_hint_label, plan.line_space, 0);
    lv_obj_set_width(s_hint_label, plan.width);
    if (clock_only) {
        /* Das Ziffernblatt traegt keinen Hinweis mehr — siehe CLOCK_FACE_Y. */
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    } else if (with_clock) {
        /* Unter der Uhr, klein: der Hinweis ist hier das Beiwerk zum Zifferblatt. */
        lv_obj_set_height(s_hint_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_hint_label, LV_ALIGN_TOP_LEFT, plan.x, CLOCK_HINT_Y);
    } else if (plan.tecki) {
        lv_obj_set_height(s_hint_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_hint_label, LV_ALIGN_TOP_LEFT, plan.x,
                     plan.status_y + meta_h() + plan.status_h + TEXT_GAP);
    } else {
        /*
         * Feste Hoehe mit LV_LABEL_LONG_DOT: Was selbst in der kleinen
         * Schrift nicht mehr auf den Schirm passt, endet mit Auslassungs-
         * punkten statt mitten im Wort abgeschnitten zu sein.
         */
        const int32_t top = plan.status_y + meta_h() + plan.status_h + TEXT_GAP;
#if UI_WATCH
        if (s_render_answer_dots) {
            /*
             * Ausschnitte zu ganzen Zeilen: Ein Ausschnitt ist so hoch, wie
             * ganze Zeilen in die Flaeche passen, und das Blaettern rueckt um
             * genau diese Hoehe -- keine Zeile steht halb da.
             */
            const step_lines_t lines = answer_lines(body_text, &plan, text_bottom_y() - top);
            s_answer_page_h = lines.per_page * lines.pitch;
            s_answer_pages = lines.pages;
            if (s_answer_page >= s_answer_pages) {
                s_answer_page = (uint8_t)(s_answer_pages - 1);
            }
            lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_height(s_hint_label, s_answer_page_h - plan.line_space);
            lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(s_hint_label, LV_ALIGN_TOP_LEFT, plan.x, top);
            lv_obj_scroll_to_y(s_hint_label, s_answer_page * s_answer_page_h, LV_ANIM_OFF);
        } else
#endif
        {
            lv_obj_set_height(s_hint_label, text_bottom_y() - top);
            lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(s_hint_label, LV_ALIGN_TOP_LEFT, plan.x, top);
        }
    }
#if UI_WATCH
    if (s_render_answer_dots) {
        ui_dots_set(&s_answer_dots, s_answer_pages, s_answer_page);
        lv_obj_align(s_answer_dots.box, LV_ALIGN_TOP_MID, 0,
                     text_bottom_y() + (DOTS_ROW_H - UI_DOTS_SIZE) / 2);
    } else {
        s_answer_pages = 0;
        ui_dots_set(&s_answer_dots, 0, 0);
        if (lv_obj_get_scroll_y(s_hint_label) != 0) {
            lv_obj_scroll_to_y(s_hint_label, 0, LV_ANIM_OFF);
        }
    }
#endif

    if (!clock_only) {
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * Antwortflaechen nebeneinander am unteren Rand, gleich breit. Die
     * erste ist gefuellt (die Zustimmung, die man meistens will), die
     * uebrigen nur umrandet -- so trifft der Daumen nicht aus Versehen die
     * falsche, weil beide gleich aussehen.
     */
#if UI_WATCH
    const bool stacked = choices_stacked();
#endif
    for (int i = 0; i < UI_STATUS_CHOICES_MAX; i++) {
        if (!s_choice_btn[i]) {
            continue;
        }
        if (i >= s_choice_count || clock_only) {
            lv_obj_add_flag(s_choice_btn[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
#if UI_WATCH
        if (stacked) {
            /* Untereinander, die erste oben -- volle Breite, zwei Zeilen in der 20er. */
            const int32_t top = TEXT_BOTTOM_Y - (s_choice_count * CHOICE_STACK_H +
                                                 (s_choice_count - 1) * CHOICE_STACK_GAP);
            lv_obj_set_size(s_choice_btn[i], CONTENT_W, CHOICE_STACK_H);
            lv_obj_set_style_radius(s_choice_btn[i], CHOICE_STACK_H / 2, 0);
            lv_obj_align(s_choice_btn[i], LV_ALIGN_TOP_LEFT, 0,
                         top + i * (CHOICE_STACK_H + CHOICE_STACK_GAP));
            lv_obj_set_style_text_font(s_choice_label[i], CHOICE_STACK_FONT, 0);
            lv_label_set_long_mode(s_choice_label[i], LV_LABEL_LONG_DOT);
            lv_obj_set_size(s_choice_label[i], CONTENT_W - 48,
                            2 * lv_font_get_line_height(CHOICE_STACK_FONT));
            lv_obj_set_style_text_align(s_choice_label[i], LV_TEXT_ALIGN_CENTER, 0);
            /* Passt der Text in eine Zeile, steht er mittig statt oben. */
            if (lv_text_get_width(s_choices[i], (uint32_t)strlen(s_choices[i]),
                                  CHOICE_STACK_FONT, 0) <= CONTENT_W - 48) {
                lv_obj_set_height(s_choice_label[i], lv_font_get_line_height(CHOICE_STACK_FONT));
            }
        } else {
            lv_obj_set_style_radius(s_choice_btn[i], CHOICE_H / 2, 0);
            lv_obj_set_style_text_font(s_choice_label[i], UI_FONT_STATUS, 0);
            lv_label_set_long_mode(s_choice_label[i], LV_LABEL_LONG_WRAP);
            lv_obj_set_size(s_choice_label[i], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        }
        if (!stacked)
#endif
        {
            const int32_t w = (CONTENT_W - (s_choice_count - 1) * CHOICE_GAP) / s_choice_count;
            lv_obj_set_size(s_choice_btn[i], w, CHOICE_H);
            lv_obj_align(s_choice_btn[i], LV_ALIGN_TOP_LEFT, i * (w + CHOICE_GAP),
                         TEXT_BOTTOM_Y - CHOICE_H);
        }
        lv_obj_center(s_choice_label[i]);
        const bool primary = i == 0;
        lv_obj_set_style_bg_opa(s_choice_btn[i], primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_choice_btn[i], lv_color_hex(0x2ecc71), 0);
        lv_obj_set_style_border_width(s_choice_btn[i], primary ? 0 : 3, 0);
        lv_obj_set_style_border_color(s_choice_btn[i], lv_color_hex(0x9aa3ad), 0);
        lv_label_set_text(s_choice_label[i], s_choices[i]);
        lv_obj_set_style_text_color(s_choice_label[i],
                                    primary ? lv_color_hex(0x000000) : lv_color_hex(0xf2f2f2), 0);
        lv_obj_remove_flag(s_choice_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

#if UI_WATCH
    /*
     * Auf der Uhr steht der Platz im Verlauf als Punktreihe unten statt als
     * "2/5" in der Kopfzeile -- dort steht jetzt die Uhrzeit. Der juengste
     * Eintrag ist der rechte Punkt: Der Verlauf liegt links vom Zifferblatt,
     * nach rechts wischen holt die aeltere Antwort von links herein (11/2026).
     */
    lv_obj_add_flag(s_position_label, LV_OBJ_FLAG_HIDDEN);
    /* Die Bilanz-Seite ist der rechte Punkt (zum Zifferblatt hin), die Eintraege links davon. */
    if (s_render_dots && s_history_cursor >= 0) {
        ui_dots_set(&s_history_dots, (uint8_t)(s_history_count + HISTORY_FIRST),
                    (uint8_t)(s_history_count + HISTORY_FIRST - 1 - s_history_cursor));
    } else {
        ui_dots_set(&s_history_dots, 0, 0);
    }
    /* Chips in der Inhalt-, Hinweise in der Meta-Stufe der Typo-Skala (Paket J). */
    lv_obj_set_style_text_font(s_more_label, UI_TYPO(UI_TYPO_META), 0);
    lv_obj_set_style_text_font(s_meta_label, UI_TYPO(UI_TYPO_META), 0);
    lv_obj_set_height(s_meta_label, lv_font_get_line_height(UI_TYPO(UI_TYPO_META)));
    int32_t bottom = TEXT_BOTTOM_Y - (s_render_dots ? DOTS_ROW_H : 0);
    for (int i = 0; i < HISTORY_CHIPS; i++) {
        lv_obj_set_style_text_font(s_chip_label[i], UI_TYPO(UI_TYPO_BODY), 0);
        if (i >= s_chip_count) {
            lv_obj_add_flag(s_chip_btn[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int32_t row_w = s_chip_count * CHIP_W + (s_chip_count - 1) * CHIP_GAP;
        lv_obj_align(s_chip_btn[i], LV_ALIGN_TOP_LEFT,
                     (CONTENT_W - row_w) / 2 + i * (CHIP_W + CHIP_GAP), bottom - CHIP_H);
        const bool undo = s_chip_action[i] != UI_HISTORY_CHIP_REPEAT;
        lv_label_set_text(s_chip_label[i], undo ? "Rückgängig" : "Nochmal");
        /* Rueckgaengig hell gefuellt wie auf den Karten, Nochmal nur umrandet. */
        lv_obj_set_style_bg_opa(s_chip_btn[i], undo ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_chip_btn[i], undo ? 0 : 2, 0);
        lv_obj_set_style_text_color(s_chip_label[i],
                                    undo ? lv_color_hex(0x000000) : lv_color_hex(0xf2f2f2), 0);
        lv_obj_remove_flag(s_chip_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_chip_count > 0) {
        bottom -= CHIP_H + CHIP_GAP;
    }
    if (s_render_more) {
        lv_obj_align(s_more_btn, LV_ALIGN_TOP_MID, 0, bottom - MORE_H);
        lv_obj_remove_flag(s_more_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_more_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_render_meta) {
        /* Punkt in der Farbe des Ergebnisses, dahinter Wort und Zeit: "erledigt · vor 12 min". */
        static char when[32]; /* unter s_lvgl_lock, siehe quoted */
        static char meta[64];
        ui_history_format_when(when, sizeof(when), s_clock_valid ? s_meta_entry.at : 0,
                               (int64_t)time(NULL), s_clock_offset_min);
        const char *word = ui_outcome_word((ui_outcome_t)s_meta_entry.outcome);
        snprintf(meta, sizeof(meta), "%s%s%s", word, word[0] && when[0] ? " · " : "", when);
        const int32_t y = plan.status_y + (META_H - META_DOT) / 2 - 2;
        lv_obj_set_style_bg_color(s_meta_dot,
                                  lv_color_hex(ui_outcome_colour((ui_outcome_t)s_meta_entry.outcome)),
                                  0);
        lv_obj_align(s_meta_dot, LV_ALIGN_TOP_LEFT, plan.x, y);
        lv_label_set_text(s_meta_label, meta);
        lv_obj_set_width(s_meta_label, plan.width - META_DOT - 8);
        lv_obj_align(s_meta_label, LV_ALIGN_TOP_LEFT, plan.x + META_DOT + 8, plan.status_y - 2);
        if (word[0] || when[0]) {
            lv_obj_remove_flag(s_meta_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meta_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_meta_entry.outcome != UI_OUTCOME_NONE) {
            lv_obj_remove_flag(s_meta_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meta_dot, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_meta_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_meta_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_transcript_active && scene == UI_STATUS_ICON_TRANSCRIBING) {
        lv_obj_remove_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_cancel_track, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cancel_track, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_header_refresh_locked();
#else
    /* Der Platz im Verlauf ("2/5") steht in der sonst leeren Kopfzeile. */
    lv_label_set_text(s_position_label, s_position_text);
    if (s_position_text[0]) {
        lv_obj_remove_flag(s_position_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_position_label, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    const bool resting = scene == UI_STATUS_ICON_RESTING;
    const bool error = scene == UI_STATUS_ICON_ERROR;
    const bool recording = scene == UI_STATUS_ICON_RECORDING;

    if (recording && plan.tecki) {
        lv_obj_remove_flag(s_meter_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_time_track, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_meter_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_track, LV_OBJ_FLAG_HIDDEN);
    }

#if UI_DARK_THEME
    /*
     * AMOLED: immer schwarzer Grund. "Ruht" unterscheidet sich nur noch
     * durch gedaempfte Schrift -- ein dunkles Blau kostete hier Strom, ohne
     * etwas zu sagen, was die geschlossenen Augen nicht schon sagen.
     */
    lv_color_t bg = lv_color_hex(0x000000);
    lv_color_t text = resting ? lv_color_hex(0xa8bad2) : lv_color_hex(0xf2f2f2);
    lv_color_t muted = resting ? lv_color_hex(0x6f7f95) : lv_color_hex(0x9aa3ad);
    lv_color_t hint_color = resting ? lv_color_hex(0xa8bad2) : muted;
#else
    lv_color_t bg = resting ? lv_color_hex(0x1b2430) : lv_color_hex(0xfff7ed);
    lv_color_t text = resting ? lv_color_hex(0xe8eef7) : lv_color_hex(0x3f3440);
    lv_color_t muted = resting ? lv_color_hex(0xa8bad2) : lv_color_hex(0x7f7180);
    lv_color_t hint_color = resting ? lv_color_hex(0xdfe9f8) : muted;
#endif
    /*
     * Gefuellt gruen heisst verbunden, ein leerer Ring heisst getrennt — der
     * Unterschied ist auch am Handgelenk und aus dem Augenwinkel zu sehen,
     * anders als zwei aehnlich helle Farbtoene.
     */
    lv_color_t ble = error ? lv_color_hex(0xf97373) :
                     tint ? lv_color_hex(NIGHT_TINT_MUTED) :
                     s_link_connected ? lv_color_hex(0x55c98a) :
                     lv_color_hex(0x8fb8ff);

    lv_obj_set_style_bg_color(s_screen, bg, 0);
    lv_obj_set_style_text_color(s_screen, text, 0);
    lv_obj_set_style_bg_color(s_ble_dot, ble, 0);
    lv_obj_set_style_bg_opa(s_ble_dot, s_link_connected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ble_dot, s_link_connected ? 0 : 2, 0);
    lv_obj_set_style_border_color(s_ble_dot, ble, 0);
    lv_obj_set_style_text_color(s_status_label, question_text[0] ? muted : text, 0);
    const lv_color_t clock_colour = night ? lv_color_hex(tint ? NIGHT_TINT_MUTED : 0x3a4656) :
                                    tint ? lv_color_hex(NIGHT_TINT_TEXT) : text;
    lv_obj_set_style_text_color(s_clock_label, clock_colour, 0);
#if UI_WATCH
    for (int i = 0; i < CLOCK_CELLS; i++) {
        lv_obj_set_style_text_color(s_clock_cell[i], clock_colour, 0);
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_color(s_large_label[i], clock_colour, 0);
    }
    lv_obj_set_style_text_color(s_face_date, s_face_locked ? lv_color_hex(0xe74c3c) :
                                tint ? lv_color_hex(NIGHT_TINT_MUTED) : muted, 0);
    lv_obj_set_style_text_color(s_meta_label, muted, 0);
#endif
    /*
     * Nachts am Kabel bleibt auch die Kopfzeile dunkel: nur die Ziffern.
     * Im Layout "Minimal" steht der Akku als Punkt unter dem Datum statt als Zahl oben.
     */
    if (night) {
        lv_obj_add_flag(s_ble_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ble_dot, LV_OBJ_FLAG_HIDDEN);
        if (layout == UI_FACE_MINIMAL) {
            lv_obj_add_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_battery_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_text_color(s_hint_label, plan.tecki ? hint_color : text, 0);
    s_muted_colour = muted;
    lv_obj_set_style_text_color(s_position_label, muted, 0);
    apply_battery_colour_locked();

    ui_status_icons_start_anim(&s_icons, scene);
}

static void render_current_locked(void)
{
    if (s_dimmed) {
        /*
         * Der Ruhezustand zeigt nur die Uhr — auch dann, wenn gerade im
         * Verlauf geblaettert wurde. Beides wird nur fuer diesen einen
         * Durchgang beiseitegelegt und danach zurueckgesetzt: Beim Aufwachen
         * soll wieder das stehen, was vorher da war.
         */
        const ui_text_kind_t kind = s_text_kind;
        char position[sizeof(s_position_text)];
        strlcpy(position, s_position_text, sizeof(position));
        s_text_kind = UI_TEXT_HINT;
        s_position_text[0] = '\0';
        render_scene_locked(UI_STATUS_ICON_RESTING, "Ruht", "", "");
        s_text_kind = kind;
        strlcpy(s_position_text, position, sizeof(s_position_text));
    } else {
        render_scene_locked(s_scene, s_status_text, s_question_text, s_hint_text);
    }
}

/*
 * Nachziehen der Minute.
 *
 * Neu gezeichnet wird nur, wenn sich die Ziffern wirklich geaendert haben:
 * `lv_label_set_text` wuerde sonst alle zehn Sekunden eine Flaeche als
 * ungueltig melden, die genauso aussieht wie vorher — auf einem Geraet, das
 * seinen Akku in Stunden misst, ist das kein Detail.
 */
/*
 * Die kleine Uhrzeit in der Kopfzeile (nur die Uhr). Sie liegt auf
 * lv_layer_top() und damit auch ueber Karten und Kontrollzentrum, die als
 * eigene Ebenen auf dem Schirm liegen. Versteckt ist sie, wo die Uhrzeit
 * ohnehin gross steht (Ziffernblatt, "Koppeln") -- es sei denn, eine dieser
 * Ebenen deckt die grosse gerade zu -- und bei der Taschenlampe, die den
 * ganzen Schirm weiss braucht. ui_cards.c und ui_control.c rufen das nach
 * jedem Ein- und Ausblenden, mit gehaltener Sperre.
 */
void ui_status_header_refresh_locked(void)
{
#if UI_WATCH
    if (!s_header_clock) {
        return;
    }
    ui_chrome_refresh_locked();
    const bool overlay = ui_cards_visible() || ui_control_visible();
    if (ui_torch_on() || (!overlay && s_main_clock_shown)) {
        lv_obj_add_flag(s_header_clock, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char now[8];
    format_clock(now, sizeof(now));
    if (strcmp(now, lv_label_get_text(s_header_clock)) != 0) {
        lv_label_set_text(s_header_clock, now);
    }
    lv_obj_remove_flag(s_header_clock, LV_OBJ_FLAG_HIDDEN);
#endif
}

static void clock_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ready) {
        return;
    }
    ui_status_header_refresh_locked();
    if (!s_clock_label || !s_main_clock_shown) {
        return;
    }
    char now[8];
    format_clock(now, sizeof(now));
    if (strcmp(now, s_clock_text) != 0) {
#if UI_WATCH
        /*
         * Einmal je Minute alles nachziehen: der Termin-Countdown laeuft,
         * um 22 Uhr schlaeft Tecki ein, und das Datum im Layout "Minimal"
         * wechselt um Mitternacht. Das Zeichnen setzt auch die Ziffern --
         * in jedem Layout an ihrer Stelle (seit 0.4.4 nicht mehr immer in
         * Zellen).
         */
        render_current_locked();
#else
        /* Neue Ziffern, neue Tintenkanten: Lage mit dem Text nachziehen. */
        place_clock_label_locked(now);
#endif
    }
}

/*
 * Die Zeile der Denk-Szene, ab fuenf Sekunden mit Zaehler: "Erledige ... 7 s".
 * `base` ist die Zeile ohne Zaehler (schon Latin-1-tauglich).
 */
static const char *thinking_status_locked(ui_status_icon_scene_t scene, const char *base)
{
    static char line[UI_STATUS_TEXT_MAX + 16]; /* unter s_lvgl_lock */
    if (s_thinking_since_us <= 0 || scene != UI_STATUS_ICON_TRANSCRIBING) {
        return base;
    }
    const int64_t elapsed_s = (esp_timer_get_time() - s_thinking_since_us) / 1000000;
    if (elapsed_s < THINKING_COUNTER_FROM_S) {
        return base;
    }
    snprintf(line, sizeof(line), "%s %u s", base, (unsigned)(elapsed_s % 1000));
    return line;
}

/*
 * Sekundentakt der Denk-Szene: nur die Statuszeile neu, nicht die ganze
 * Szene. Bei dunklem oder ausgeschaltetem Schirm zaehlt er still weiter --
 * der naechste Blick zeigt den richtigen Stand.
 */
static void thinking_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ready || s_dimmed || s_display_is_off || s_thinking_since_us <= 0 ||
        s_scene != UI_STATUS_ICON_TRANSCRIBING || s_question_text[0]) {
        return;
    }
    const char *line = thinking_status_locked(s_scene, s_status_text);
    if (strcmp(line, lv_label_get_text(s_status_label)) != 0) {
        lv_label_set_text(s_status_label, line);
    }
}

static void create_status_ui(void)
{
    s_screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xfff7ed), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(0x3f3440), 0);
    lv_obj_set_style_pad_all(s_screen, UI_SCREEN_PAD, 0);

    s_ble_dot = create_blob(s_screen, UI_DOT_SIZE, UI_DOT_SIZE, lv_color_hex(0x8fb8ff));
    lv_obj_align(s_ble_dot, LV_ALIGN_TOP_LEFT, UI_HEADER_INSET_X, UI_DOT_Y);

    create_battery_ui(s_screen);
    create_meters(s_screen);
    ui_status_icons_create(&s_icons, s_screen);

#if UI_WATCH
    for (int i = 0; i < FACE_LABELS; i++) {
        s_face_label[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_font(s_face_label[i], &todoteck_watch_font_23, 0);
        lv_label_set_long_mode(s_face_label[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_face_label[i], CONTENT_W - 2 * UI_HEADER_INSET_X);
        lv_obj_set_style_text_align(s_face_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_face_label[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < UI_STATUS_CHOICES_MAX; i++) {
        s_choice_btn[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_choice_btn[i]);
        lv_obj_set_style_radius(s_choice_btn[i], CHOICE_H / 2, 0);
        lv_obj_add_flag(s_choice_btn[i], LV_OBJ_FLAG_HIDDEN);
        s_choice_label[i] = lv_label_create(s_choice_btn[i]);
        lv_obj_set_style_text_font(s_choice_label[i], UI_FONT_STATUS, 0);
        lv_obj_center(s_choice_label[i]);
    }

    /* "Tippen fuer mehr" unter der Kurzform: umrandet, nicht gefuellt -- ein Hinweis. */
    s_more_btn = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_more_btn);
    lv_obj_remove_flag(s_more_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_more_btn, MORE_W, MORE_H);
    lv_obj_set_style_radius(s_more_btn, MORE_H / 2, 0);
    lv_obj_set_style_border_width(s_more_btn, 2, 0);
    lv_obj_set_style_border_color(s_more_btn, lv_color_hex(0x3a4656), 0);
    s_more_label = lv_label_create(s_more_btn);
    lv_label_set_text(s_more_label, "Tippen für mehr");
    lv_obj_set_style_text_font(s_more_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_more_label, lv_color_hex(0x9aa3ad), 0);
    lv_obj_center(s_more_label);
    lv_obj_add_flag(s_more_btn, LV_OBJ_FLAG_HIDDEN);

    /*
     * Transkript-Szene: "Abbrechen" ueber die volle Breite (abzueglich Rand,
     * damit die Rundung der unteren Ecken nichts abschneidet) und darueber
     * der Balken, der die Frist ablaufen laesst.
     */
    s_cancel_btn = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_cancel_btn);
    lv_obj_remove_flag(s_cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_cancel_btn, CONTENT_W - 2 * UI_SCREEN_PAD, CANCEL_H);
    lv_obj_set_style_radius(s_cancel_btn, CANCEL_H / 2, 0);
    lv_obj_set_style_bg_opa(s_cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cancel_btn, lv_color_hex(0x1b2430), 0);
    lv_obj_set_style_border_width(s_cancel_btn, 3, 0);
    lv_obj_set_style_border_color(s_cancel_btn, lv_color_hex(0xf97373), 0);
    lv_obj_align(s_cancel_btn, LV_ALIGN_TOP_MID, 0, TEXT_BOTTOM_Y - CANCEL_H);
    lv_obj_t *cancel_label = lv_label_create(s_cancel_btn);
    lv_label_set_text(cancel_label, "Abbrechen");
    lv_obj_set_style_text_font(cancel_label, UI_FONT_STATUS, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xf2f2f2), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);

    s_cancel_track = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_cancel_track);
    lv_obj_remove_flag(s_cancel_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_cancel_track, CONTENT_W - 2 * UI_SCREEN_PAD, CANCEL_BAR_H);
    lv_obj_set_style_radius(s_cancel_track, CANCEL_BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(s_cancel_track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cancel_track, lv_color_hex(0x3a4656), 0);
    lv_obj_align(s_cancel_track, LV_ALIGN_TOP_MID, 0,
                 TEXT_BOTTOM_Y - CANCEL_H - CANCEL_GAP - CANCEL_BAR_H);
    lv_obj_add_flag(s_cancel_track, LV_OBJ_FLAG_HIDDEN);
    s_cancel_fill = lv_obj_create(s_cancel_track);
    lv_obj_remove_style_all(s_cancel_fill);
    lv_obj_set_size(s_cancel_fill, CONTENT_W - 2 * UI_SCREEN_PAD, CANCEL_BAR_H);
    lv_obj_set_style_radius(s_cancel_fill, CANCEL_BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(s_cancel_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cancel_fill, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(s_cancel_fill, LV_ALIGN_LEFT_MID, 0, 0);

    /* Seitenpunkte des Verlaufs, ganz unten mittig. */
    ui_dots_create(&s_history_dots, s_screen);
    lv_obj_align(s_history_dots.box, LV_ALIGN_TOP_MID, 0, TEXT_BOTTOM_Y - DOT_SIZE - 4);
    ui_dots_create(&s_answer_dots, s_screen);

    /* Verlauf: Marker-Zeile (Punkt + Wort + Zeit) und die zwei Chips; Lage setzt render_scene_locked(). */
    s_meta_dot = create_blob(s_screen, META_DOT, META_DOT, lv_color_hex(0x9aa3ad));
    lv_obj_add_flag(s_meta_dot, LV_OBJ_FLAG_HIDDEN);
    s_meta_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_meta_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_meta_label, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_meta_label, lv_font_get_line_height(UI_FONT_SMALL));
    lv_obj_add_flag(s_meta_label, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < HISTORY_CHIPS; i++) {
        s_chip_btn[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_chip_btn[i]);
        lv_obj_remove_flag(s_chip_btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_chip_btn[i], CHIP_W, CHIP_H);
        lv_obj_set_style_radius(s_chip_btn[i], CHIP_H / 2, 0);
        lv_obj_set_style_bg_color(s_chip_btn[i], lv_color_hex(0xf2f2f2), 0);
        lv_obj_set_style_border_color(s_chip_btn[i], lv_color_hex(0x9aa3ad), 0);
        lv_obj_add_flag(s_chip_btn[i], LV_OBJ_FLAG_HIDDEN);
        s_chip_label[i] = lv_label_create(s_chip_btn[i]);
        lv_obj_set_style_text_font(s_chip_label[i], &todoteck_watch_font_23, 0);
        lv_obj_center(s_chip_label[i]);
    }

    /* Die Zellen der grossen Uhrzeit; Schrift, Breite und Lage setzt place_clock_cells_locked(). */
    for (int i = 0; i < CLOCK_CELLS; i++) {
        s_clock_cell[i] = lv_label_create(s_screen);
        lv_label_set_text(s_clock_cell[i], "");
        lv_obj_set_style_text_font(s_clock_cell[i], UI_FONT_CLOCK_FACE, 0);
        lv_obj_set_style_text_align(s_clock_cell[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_clock_cell[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Layout "Gross": Stunden und Minuten; "Minimal": Datum und Akku-Punkt (0.4.4). */
    for (int i = 0; i < 2; i++) {
        s_large_label[i] = lv_label_create(s_screen);
        lv_label_set_text(s_large_label[i], "");
        lv_obj_set_style_text_font(s_large_label[i], UI_FONT_CLOCK_LARGE, 0);
        lv_obj_add_flag(s_large_label[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_face_date = lv_label_create(s_screen);
    lv_label_set_text(s_face_date, "");
    lv_obj_set_style_text_font(s_face_date, UI_FONT_STATUS, 0);
    lv_obj_add_flag(s_face_date, LV_OBJ_FLAG_HIDDEN);
    s_battery_dot = create_blob(s_screen, MINIMAL_DOT, MINIMAL_DOT, lv_color_hex(0x2ecc71));
    lv_obj_add_flag(s_battery_dot, LV_OBJ_FLAG_HIDDEN);

    /* Die kleine Uhrzeit oben mittig; lv_layer_top() kennt keinen Rand, daher + Rand. */
    s_header_clock = lv_label_create(lv_layer_top());
    lv_label_set_text(s_header_clock, "--:--");
    lv_obj_set_style_text_font(s_header_clock, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_header_clock, lv_color_hex(0xc9d1d9), 0);
    lv_obj_align(s_header_clock, LV_ALIGN_TOP_MID, 0, UI_SCREEN_PAD + UI_HEADER_LABEL_Y);
    lv_obj_add_flag(s_header_clock, LV_OBJ_FLAG_HIDDEN);
    /* Lautlos-Zeichen, Mikrofon-Punkt und Renderzeit (Paket J, ui_chrome.c). */
    ui_chrome_create_locked(s_display);
#endif

    s_position_label = lv_label_create(s_screen);
    lv_label_set_text(s_position_label, "");
    lv_obj_set_style_text_font(s_position_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_position_label, lv_color_hex(0x7f7180), 0);
    lv_obj_align(s_position_label, LV_ALIGN_TOP_LEFT, UI_POSITION_X, UI_HEADER_LABEL_Y);
    lv_obj_add_flag(s_position_label, LV_OBJ_FLAG_HIDDEN);

    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, s_status_text);
    /*
     * Genau eine Zeile, der Rest mit Auslassungspunkten.
     *
     * Solange dort nur Zustandswoerter standen ("Bereit", "Koppeln"), war das
     * egal. Im Verlauf traegt die Zeile die Frage, und die passt regelmaessig
     * nicht: Ohne feste Hoehe wuerde sie umbrechen und in den Antworttext
     * darunter laufen — plan_text_layout() rechnet mit genau einer Zeile.
     */
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_status_label, UI_FONT_STATUS->line_height);
    lv_obj_set_style_text_font(s_status_label, UI_FONT_STATUS, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x3f3440), 0);
    lv_obj_set_width(s_status_label, COL_W);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, COL_X, STATUS_Y_WITH_TECKI);

    s_clock_label = lv_label_create(s_screen);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_set_style_text_font(s_clock_label, UI_FONT_CLOCK_SMALL, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0x3f3440), 0);
    lv_obj_set_width(s_clock_label, COL_W);
    lv_obj_set_style_text_align(s_clock_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_LEFT, COL_X, CLOCK_Y);
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_HIDDEN);

    s_hint_label = lv_label_create(s_screen);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
#if UI_WATCH
    /* Lange Antworten scrollen im Label selbst (Ausschnitte, Punkt 42) -- ohne Leiste. */
    lv_obj_set_scrollbar_mode(s_hint_label, LV_SCROLLBAR_MODE_OFF);
#endif
    lv_obj_set_style_text_line_space(s_hint_label, TEXT_STEP_SMALLEST.line_space, 0);
    lv_obj_set_width(s_hint_label, COL_W);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_text(s_hint_label, s_hint_text);
    lv_obj_align(s_hint_label, LV_ALIGN_TOP_LEFT, COL_X, CLOCK_HINT_Y);

    /*
     * Die Uhr zieht von selbst nach. Der Takt laeuft im LVGL-Zeitgeber und
     * damit im selben Strang wie alles andere an der Anzeige — er braucht die
     * Sperre also nicht selbst zu nehmen.
     */
    lv_timer_create(clock_tick_cb, CLOCK_TICK_MS, NULL);
    /* Der Sekundentakt der Denk-Szene schlaeft, bis gedacht wird (ui_status_set_thinking). */
    s_thinking_timer = lv_timer_create(thinking_tick_cb, 1000, NULL);
    lv_timer_pause(s_thinking_timer);

    s_ready = true;
    render_current_locked();
}

static size_t utf8_decode(const unsigned char *p, uint32_t *codepoint)
{
    if (p[0] < 0x80) {
        *codepoint = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                     (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                     ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    /* Kaputtes Byte: eins weiter, sonst kaeme die Schleife nicht voran. */
    *codepoint = 0xFFFD;
    return 1;
}

/*
 * Deckungsbereich der beiden Schriften: ASCII ab dem Leerzeichen und Latin-1
 * ohne dessen Steuerbereich. Steht als Bedingung hier, weil die Alternative
 * — jede Schrift nach jedem Zeichen fragen — teurer ist als die zwei
 * Vergleiche und dasselbe Ergebnis liefert.
 */
static bool font_has_codepoint(uint32_t codepoint)
{
    return (codepoint >= 32 && codepoint <= 126) ||
           (codepoint >= 160 && codepoint <= 172) ||
           (codepoint >= 174 && codepoint <= 255);
}

/*
 * Typografie, die keine der beiden Schriften enthaelt, die aber in jedem Text
 * von draussen steckt: Gedankenstrich, typografische Anfuehrungszeichen,
 * Auslassungspunkte. Ersatzlos zu streichen ergaebe "Aufgabe heute" statt
 * "Aufgabe - heute" — der ASCII-Ersatz sagt dasselbe wie das Original.
 */
static const char *ascii_substitute(uint32_t codepoint)
{
    switch (codepoint) {
    case 0x2010: /* Bindestrich */
    case 0x2011:
    case 0x2012:
    case 0x2013: /* Halbgeviertstrich */
    case 0x2014: /* Geviertstrich */
    case 0x2015:
    case 0x2212: /* Minus */
        return "-";
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
        return "'";
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
        return "\"";
    case 0x2022: /* Aufzaehlungspunkt */
    case 0x2027:
        return "·";
    case 0x2026: /* Auslassungspunkte */
        return "...";
    case 0x2192: /* Pfeil */
        return "->";
    default:
        return NULL;
    }
}

/*
 * Kopiert Text so, wie das Display ihn zeigen kann: hoechstens `size - 1`
 * Byte, nie mitten in ein UTF-8-Zeichen — und ohne Zeichen, die die Schrift
 * nicht kennt.
 *
 * Beide Regeln stammen aus der Praxis. `strlcpy` schnitte einen Umlaut an der
 * Bytegrenze mittendurch; LVGL laeuft beim Dekodieren dann ins Leere. Und die
 * Antworten der Bruecke tragen Emoji ("Aufgabe angelegt · Faellig: ..." kommt
 * mit Haken-, Kalender- und Ordnersymbol an), die keine der beiden Schriften
 * enthaelt: Auf dem Geraet standen dort leere Rechtecke mitten im Satz. Sie
 * fliegen raus, samt der Luecke, die sie hinterlassen — sonst bliebe der
 * doppelte Abstand stehen, den sie umgab.
 */
static void copy_utf8_display(char *dst, const char *src, size_t size)
{
    if (size == 0) {
        return;
    }
    size_t out = 0;
    bool pending_space = false;
    const unsigned char *p = (const unsigned char *)(src ? src : "");

    while (*p != '\0') {
        uint32_t codepoint = 0;
        const size_t len = utf8_decode(p, &codepoint);
        const unsigned char *start = p;
        p += len;

        if (codepoint == '\n') {
            if (out + 1 >= size) {
                break;
            }
            dst[out++] = '\n';
            pending_space = false;
            continue;
        }
        /* 0x00A0 ist ein Leerzeichen, auch wenn die Schrift es kennt. */
        if (codepoint == ' ' || codepoint == '\t' || codepoint == 0x00A0) {
            pending_space = out > 0;
            continue;
        }

        const char *substitute = NULL;
        if (!font_has_codepoint(codepoint)) {
            substitute = ascii_substitute(codepoint);
            if (substitute == NULL) {
                continue;
            }
        }
        const char *from = substitute ? substitute : (const char *)start;
        const size_t bytes = substitute ? strlen(substitute) : len;

        if (out + (pending_space ? 1 : 0) + bytes + 1 > size) {
            break;
        }
        if (pending_space) {
            dst[out++] = ' ';
            pending_space = false;
        }
        memcpy(dst + out, from, bytes);
        out += bytes;
    }
    dst[out] = '\0';
}

#if UI_WATCH
static void cancel_bar_anim_cb(void *var, int32_t value)
{
    lv_obj_set_width((lv_obj_t *)var, value);
}
#endif

/* Die Transkript-Szene endet mit jeder anderen Szene -- auch ihr Balken. */
static void end_transcript_locked(void)
{
    if (!s_transcript_active) {
        return;
    }
    s_transcript_active = false;
#if UI_WATCH
    lv_anim_delete(s_cancel_fill, cancel_bar_anim_cb);
#endif
}

/* Alles, was eine Szene ausmacht -- ohne zu zeichnen. Sperre gehalten. */
static void assign_scene_locked(ui_status_icon_scene_t scene, const char *status,
                                const char *question, const char *hint, ui_text_kind_t kind,
                                const char *position, bool shortable)
{
    s_empty_history_shown = false;
    s_choice_count = 0;
#if UI_WATCH
    s_meta_shown = false;
#endif
    s_mood = UI_STATUS_MOOD_NONE;
    s_shortable = shortable;
    s_expanded = false;
#if UI_WATCH
    s_answer_page = 0;
#endif
    end_transcript_locked();
    s_thinking_since_us = 0;
    if (s_thinking_timer) {
        lv_timer_pause(s_thinking_timer);
    }
    s_scene = scene;
    s_text_kind = kind;
    copy_utf8_display(s_status_text, status ? status : "", sizeof(s_status_text));
    copy_utf8_display(s_question_text, question ? question : "", sizeof(s_question_text));
    copy_utf8_display(s_hint_text, hint ? hint : "", sizeof(s_hint_text));
    strlcpy(s_position_text, position ? position : "", sizeof(s_position_text));
}

static void set_scene_ex(ui_status_icon_scene_t scene, const char *status,
                         const char *question, const char *hint, ui_text_kind_t kind,
                         const char *position, bool shortable)
{
    _lock_acquire(&s_lvgl_lock);
    assign_scene_locked(scene, status, question, hint, kind, position, shortable);
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

static void set_scene_full(ui_status_icon_scene_t scene, const char *status,
                           const char *question, const char *hint, ui_text_kind_t kind,
                           const char *position)
{
    set_scene_ex(scene, status, question, hint, kind, position, false);
}

/*
 * Jede Szene ausser dem Blaettern raeumt Frage und Verlaufsposition weg: Sie
 * stehen fuer "du siehst eine Antwort" beziehungsweise "du bist im Verlauf",
 * und sobald etwas anderes auf dem Schirm ist, stimmt beides nicht mehr.
 */
static void set_scene(ui_status_icon_scene_t scene, const char *status, const char *hint,
                      ui_text_kind_t kind)
{
    set_scene_full(scene, status, "", hint, kind, "");
}

esp_err_t ui_status_init(void)
{
#if UI_WATCH
    lv_init();
    ESP_RETURN_ON_ERROR(ui_display_watch206_create(&s_display), TAG, "create display");
#else
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        // RC_FAST clock remains active during light sleep, preventing backlight flicker
        .clk_cfg = LEDC_USE_RC_FAST_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "configure backlight timer");

    const ledc_channel_config_t backlight_channel = {
        .gpio_num = STICK_S3_PIN_LCD_BL,
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        .flags.output_invert = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "configure backlight channel");

    spi_bus_config_t bus_config = {
        .sclk_io_num = STICK_S3_PIN_LCD_SCK,
        .mosi_io_num = STICK_S3_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "initialize lcd spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = STICK_S3_PIN_LCD_DC,
        .cs_gpio_num = STICK_S3_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io),
                        TAG, "create lcd panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = STICK_S3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &panel),
                        TAG, "create st7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "invert panel colors");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, true), TAG, "swap panel axes");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, LCD_LANDSCAPE_FLIP, !LCD_LANDSCAPE_FLIP),
                        TAG, "mirror panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP), TAG, "set panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "turn display on");

    lv_init();
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_display, ESP_ERR_NO_MEM, TAG, "create lvgl display");

    const size_t draw_buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "allocate lvgl draw buffers");

    lv_display_set_buffers(s_display, buf1, buf2, draw_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_display, panel);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, s_display),
                        TAG, "register lcd callbacks");
#endif /* UI_WATCH */

    lv_tick_set_cb(lvgl_tick_get);

    _lock_acquire(&s_lvgl_lock);
    create_status_ui();
    _lock_release(&s_lvgl_lock);
#if UI_WATCH
    history_load();
#endif

    BaseType_t task_ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE,
                                     NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create lvgl task");

    ESP_RETURN_ON_ERROR(ui_status_set_brightness(LCD_BACKLIGHT_DEFAULT), TAG, "set backlight");
    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}

esp_err_t ui_status_set_brightness(uint8_t brightness)
{
#if UI_WATCH
    return ui_display_watch206_set_brightness(brightness);
#else
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE,
                                      LCD_BACKLIGHT_LEDC_CHANNEL,
                                      brightness),
                        TAG, "set backlight duty");
    return ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL);
#endif
}

void ui_status_set_display_off(bool off)
{
#if UI_WATCH
    if (!s_display) {
        return;
    }
    _lock_acquire(&s_lvgl_lock);
    s_display_is_off = off;
    lv_timer_t *refr = lv_display_get_refr_timer(s_display);
    if (off) {
        ui_display_watch206_sleep();
        /*
         * Ohne Bild braucht es kein Neuzeichnen: Der Refresh-Zeitgeber
         * (33 ms) pausiert, und die LVGL-Task schlaeft bis zum naechsten
         * eigenen Zeitgeber statt dreissigmal je Sekunde aufzuwachen.
         */
        if (refr) {
            lv_timer_pause(refr);
        }
    } else {
        if (refr) {
            lv_timer_resume(refr);
        }
        lv_obj_invalidate(lv_screen_active());
        ui_display_watch206_wake();
    }
    _lock_release(&s_lvgl_lock);
#else
    (void)off;
#endif
}

void ui_status_prepare_deep_sleep(void)
{
    (void)ui_status_set_brightness(0);
#if UI_WATCH
    /* Was noch nicht im Flash steht, jetzt -- nach dem Schlaf startet die Firmware neu. */
    if (s_history_dirty) {
        if (s_history_save_timer) {
            (void)esp_timer_stop(s_history_save_timer);
        }
        history_save_now();
    }
#endif

    _lock_acquire(&s_lvgl_lock);
#if UI_WATCH
    ui_display_watch206_sleep();
#else
    if (s_display) {
        esp_lcd_panel_handle_t panel = lv_display_get_user_data(s_display);
        if (panel) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel, false));
        }
    }
#endif
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_device_name(const char *device_name)
{
    copy_utf8_display(s_device_name, device_name && device_name[0] ? device_name : "BLE",
                      sizeof(s_device_name));

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_advertising(void)
{
    ESP_LOGD(TAG, "advertising");
    set_scene(UI_STATUS_ICON_PAIRING, "Koppeln", "Brücke starten", UI_TEXT_HINT);
}

void ui_status_set_pairing(const char *device_name)
{
    ESP_LOGD(TAG, "pairing %s", device_name ? device_name : "");
    ui_status_set_device_name(device_name);
    set_scene(UI_STATUS_ICON_PAIRING, "Koppeln", device_name ? device_name : "VS-0000",
              UI_TEXT_HINT);
}

/*
 * Der Bereitzustand sagt nichts mehr — er zeigt die Uhrzeit.
 *
 * Bis 08/2026 stand hier "Halten zum Sprechen" (oder "Tippen zum Sprechen",
 * je nach Bedienart). Der Satz stammt aus der Zeit, als das Geraet in diesem
 * Zustand nichts anderes anzuzeigen hatte. Am Handgelenk liest man ihn genau
 * zweimal und danach nie wieder, und er kostete die Zeile, die jetzt die
 * Ziffern tragen. Welche Taste spricht, beantwortet der erste Druck.
 */
void ui_status_set_idle(void)
{
    ESP_LOGD(TAG, "idle");
    set_scene(UI_STATUS_ICON_IDLE, "Bereit", "", UI_TEXT_HINT);
}

/*
 * Wie ui_status_set_idle(), zeigt aber die Antwort der Bruecke statt des
 * Standard-Hinweises. Der Text bleibt stehen, bis der naechste Turn beginnt —
 * wer ihn verpasst hat, soll ihn noch lesen koennen.
 *
 * Ohne das bliebe die eigentliche Auskunft ("Aufgabe angelegt: Pool
 * rueckspuelen") unsichtbar: sie kam schon immer ueber die Funkstrecke, wurde
 * bei ready aber verworfen.
 *
 * `question` ist das, was die Bruecke verstanden hat. Sie steht in
 * Anfuehrungszeichen ueber der Antwort — und zwar sofort, nicht erst im
 * Verlauf: Wer eine falsch verstandene Frage gleich sieht, sagt sie noch
 * einmal, statt sich spaeter ueber die Aufgabe zu wundern. Schickt die
 * Bruecke keine, steht dort wie bisher "Bereit".
 */
#if UI_WATCH
/*
 * Der Verlauf im NVS (V7): die Eintraege als ein Block, aeltester zuerst,
 * mit Kennung und Satzgroesse davor -- ein Block aus einer anderen Fassung
 * wird verworfen statt falsch gelesen. Geschrieben wird gebuendelt: Jede
 * Aenderung setzt die Marke und startet den 30-s-Zeitgeber neu; erst dann
 * geht der Block ins Flash (und vor dem Tiefschlaf sofort).
 */
typedef struct {
    uint32_t magic;
    uint16_t entry_size;
    uint8_t count;
    uint8_t reserved;
} history_blob_header_t;

static void history_save_now(void)
{
    history_blob_header_t header = {
        .magic = HISTORY_NVS_MAGIC,
        .entry_size = sizeof(ui_history_entry_t),
    };
    /* Im PSRAM, nicht auf dem Stack des Zeitgeber-Tasks (der hat 3,5 KB) und nicht statisch im internen RAM. */
    const size_t blob_max = sizeof(header) + sizeof(s_history);
    uint8_t *blob = heap_caps_malloc(blob_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        blob = malloc(blob_max);
    }
    if (!blob) {
        ESP_LOGW(TAG, "Verlauf nicht gesichert: kein Speicher");
        return;
    }
    _lock_acquire(&s_lvgl_lock);
    s_history_dirty = false;
    header.count = s_history_count;
    memcpy(blob, &header, sizeof(header));
    for (uint8_t i = 0; i < s_history_count; i++) {
        /* Aeltester zuerst: der liegt `count` Plaetze vor dem naechsten freien. */
        const uint8_t index = (uint8_t)((s_history_next + UI_HISTORY_MAX - s_history_count + i) %
                                        UI_HISTORY_MAX);
        memcpy(blob + sizeof(header) + (size_t)i * sizeof(ui_history_entry_t), &s_history[index],
               sizeof(ui_history_entry_t));
    }
    _lock_release(&s_lvgl_lock);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(HISTORY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, HISTORY_NVS_KEY, blob,
                           sizeof(header) + (size_t)header.count * sizeof(ui_history_entry_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    free(blob);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Verlauf nicht gesichert: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Verlauf gesichert: %u Eintraege", (unsigned)header.count);
    }
}

/*
 * Der Zeitgeber schreibt nicht selbst: Der esp_timer-Task hat 3,5 KB Stack,
 * und nvs_set_blob/nvs_commit laufen durch den Flash-Treiber mit Cache-
 * Sperre -- fuer den Sichern-Lauf gibt es den Ereignis-Task von main.c, der
 * auch das Akku-Protokoll in den NVS schreibt. Deshalb nur ein Ruf an den
 * Abnehmer (ui_status_set_history_save_cb); der reiht ein Ereignis ein und
 * ruft ui_status_history_save() aus seinem Task. Ohne Abnehmer wird wie
 * bis 0.4.1 direkt geschrieben.
 *
 * Ein Callback statt einer Abfrage im Akku-Takt, weil der Akku-Takt am Kabel
 * und je Zustand anders laeuft und der Verlauf dann irgendwann zwischen 30 s
 * und zehn Minuten im Flash laege -- so bleiben es 30 s nach der Aenderung.
 */
static ui_status_history_save_cb_t s_history_save_cb;

static void history_save_timer_cb(void *arg)
{
    (void)arg;
    if (s_history_save_cb) {
        s_history_save_cb();
    } else {
        history_save_now();
    }
}

void ui_status_set_history_save_cb(ui_status_history_save_cb_t cb)
{
    s_history_save_cb = cb;
}

void ui_status_history_save(void)
{
    if (s_history_dirty) {
        history_save_now();
    }
}

/* Sperre gehalten: Aenderung merken, Schreiben in 30 s. */
static void history_mark_dirty_locked(void)
{
    s_history_dirty = true;
    if (!s_history_save_timer) {
        const esp_timer_create_args_t args = {
            .callback = history_save_timer_cb,
            .name = "history_nvs",
        };
        if (esp_timer_create(&args, &s_history_save_timer) != ESP_OK) {
            return;
        }
    }
    (void)esp_timer_stop(s_history_save_timer);
    (void)esp_timer_start_once(s_history_save_timer, HISTORY_SAVE_DELAY_US);
}

static void history_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* Voller oder alter Speicher: voice_ble_init() loescht und legt neu an -- dann ohne Verlauf. */
        ESP_LOGW(TAG, "NVS fuer den Verlauf nicht bereit: %s", esp_err_to_name(err));
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(HISTORY_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    const size_t blob_max = sizeof(history_blob_header_t) + sizeof(s_history);
    uint8_t *blob = heap_caps_malloc(blob_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        blob = malloc(blob_max);
    }
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    size_t size = blob_max;
    err = nvs_get_blob(nvs, HISTORY_NVS_KEY, blob, &size);
    nvs_close(nvs);
    history_blob_header_t header = { 0 };
    if (err == ESP_OK && size >= sizeof(header)) {
        memcpy(&header, blob, sizeof(header));
    }
    if (err != ESP_OK || header.magic != HISTORY_NVS_MAGIC ||
        header.entry_size != sizeof(ui_history_entry_t) || header.count > UI_HISTORY_MAX ||
        size < sizeof(header) + (size_t)header.count * sizeof(ui_history_entry_t)) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Verlauf im NVS passt nicht zu dieser Fassung, verworfen");
        }
        free(blob);
        return;
    }
    _lock_acquire(&s_lvgl_lock);
    memcpy(s_history, blob + sizeof(header), (size_t)header.count * sizeof(ui_history_entry_t));
    for (uint8_t i = 0; i < header.count; i++) {
        /* Zeichenketten sicher abschliessen, was auch immer im Flash stand. */
        s_history[i].question[sizeof(s_history[i].question) - 1] = '\0';
        s_history[i].answer[sizeof(s_history[i].answer) - 1] = '\0';
    }
    s_history_count = header.count;
    s_history_next = (uint8_t)(header.count % UI_HISTORY_MAX);
    s_history_cursor = -1;
    _lock_release(&s_lvgl_lock);
    free(blob);
    ESP_LOGI(TAG, "Verlauf aus dem NVS: %u Eintraege", (unsigned)header.count);
}
#endif /* UI_WATCH */

/* Sperre gehalten: naechsten Platz im Ring belegen und leeren. */
static ui_history_entry_t *history_push_locked(void)
{
    ui_history_entry_t *entry = &s_history[s_history_next];
    memset(entry, 0, sizeof(*entry));
    entry->at = s_clock_valid ? (int64_t)time(NULL) : 0;
    s_history_next = (uint8_t)((s_history_next + 1) % UI_HISTORY_MAX);
    if (s_history_count < UI_HISTORY_MAX) {
        s_history_count++;
    }
    /* Ein neuer Eintrag beendet das Blaettern — der naechste Druck faengt vorn an. */
    s_history_cursor = -1;
#if UI_WATCH
    history_mark_dirty_locked();
#endif
    return entry;
}

void ui_status_set_idle_text(const char *text, const char *question, ui_outcome_t outcome,
                             uint16_t ref)
{
    if (!text || !text[0]) {
        ui_status_set_idle();
        return;
    }
    ESP_LOGD(TAG, "idle mit Text");

    _lock_acquire(&s_lvgl_lock);
    ui_history_entry_t *entry = history_push_locked();
    copy_utf8_display(entry->question, question ? question : "", sizeof(entry->question));
    copy_utf8_display(entry->answer, text, sizeof(entry->answer));
    entry->kind = UI_HISTORY_TURN;
    entry->outcome = (uint8_t)outcome;
    entry->ref = ref;
    _lock_release(&s_lvgl_lock);

    set_scene_ex(UI_STATUS_ICON_IDLE, "Bereit", question ? question : "", text,
                 UI_TEXT_MESSAGE, "", true);
}

void ui_status_history_add_card(const char *line, uint8_t card_kind, uint16_t card_rev,
                                uint8_t card_index)
{
    _lock_acquire(&s_lvgl_lock);
    ui_history_entry_t *entry = history_push_locked();
    copy_utf8_display(entry->answer, line ? line : "", sizeof(entry->answer));
    entry->kind = UI_HISTORY_CARD;
    entry->outcome = UI_OUTCOME_DONE;
    entry->card_kind = card_kind;
    entry->card_rev = card_rev;
    entry->card_index = card_index;
    _lock_release(&s_lvgl_lock);
}

bool ui_status_history_chip_at(int16_t x, int16_t y, ui_history_chip_hit_t *out)
{
#if UI_WATCH
    bool hit = false;
    _lock_acquire(&s_lvgl_lock);
    for (int i = 0; s_meta_shown && i < s_chip_count && i < HISTORY_CHIPS; i++) {
        if (lv_obj_has_flag(s_chip_btn[i], LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(s_chip_btn[i], &area);
        if (x < area.x1 - UI_HIT_TOLERANCE || x > area.x2 + UI_HIT_TOLERANCE ||
            y < area.y1 - UI_HIT_TOLERANCE || y > area.y2 + UI_HIT_TOLERANCE) {
            continue;
        }
        hit = true;
        if (out) {
            memset(out, 0, sizeof(*out));
            out->chip = s_chip_action[i];
            strlcpy(out->question, s_meta_entry.question, sizeof(out->question));
            out->ref = s_meta_entry.ref;
            out->card_kind = s_meta_entry.card_kind;
            out->card_rev = s_meta_entry.card_rev;
            out->card_index = s_meta_entry.card_index;
        }
        break;
    }
    _lock_release(&s_lvgl_lock);
    return hit;
#else
    (void)x;
    (void)y;
    (void)out;
    return false;
#endif
}

void ui_status_history_mark_undone(void)
{
#if UI_WATCH
    _lock_acquire(&s_lvgl_lock);
    const int index = s_history_cursor - HISTORY_FIRST;
    if (s_meta_shown && index >= 0 && index < (int)s_history_count) {
        const uint8_t slot = (uint8_t)((s_history_next + UI_HISTORY_MAX - 1 - index) %
                                       UI_HISTORY_MAX);
        s_history[slot].undone = 1;
        s_meta_entry.undone = 1;
        history_mark_dirty_locked();
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
#endif
}

/*
 * Ein Schritt zurueck durch die letzten Fragen und Antworten.
 *
 * Der Fall dahinter ist derselbe wie frueher bei der Seitentaste (die
 * inzwischen die sprechende ist): Das Display
 * dimmt nach 15 Sekunden und schlaeft nach fuenf Minuten ganz ein; wer den
 * Stick erst danach ansieht, hat die Antwort nie gelesen. Sie noch einmal zu
 * zeigen kostet nichts — sie erneut zu erfragen kostet Transkription, Intent
 * und womoeglich eine zweite Aufgabe. Neu ist nur, dass es nicht bei der
 * letzten aufhoert.
 *
 * Der Ring hat einen Ausgang und keinen Rundlauf: Nach der aeltesten Antwort
 * steht wieder das Ziffernblatt da. Ein Zyklus ohne Ende wuesste nie, wann er
 * fertig ist — so ist der Weg zurueck zur Uhr immer nur ein Druck mehr, und
 * die Position in der Kopfzeile sagt, wie viele es noch sind.
 */
/*
 * Wohin das Blaettern zurueckkehrt: auf das Ziffernblatt, wenn eine Bruecke
 * verbunden ist -- sonst auf "Koppeln".
 *
 * Bis 10/2026 war es immer das Ziffernblatt. Ohne Bruecke sagte das "Bereit"
 * zu einem Geraet, das gar nicht sprechen konnte; aufgefallen ist es an der
 * Waveshare-Uhr, die nach dem ersten Druck auf PWR (dem Einschalten) im
 * Verlauf stand und von dort nicht mehr nach "Koppeln" zurueckfand.
 */
void ui_status_show_home(void)
{
    char name[sizeof(s_device_name)];
    _lock_acquire(&s_lvgl_lock);
    const bool connected = s_link_connected;
    strlcpy(name, s_device_name, sizeof(name));
    _lock_release(&s_lvgl_lock);
    if (connected) {
        ui_status_set_idle();
    } else {
        ui_status_set_pairing(name);
    }
}

/*
 * Ein Schritt durch den Verlauf: +1 aelter, -1 neuer.
 *
 * Aelter ist die Richtung der Blaettertaste; neuer gibt es seit dem Wischen
 * auf der Uhr. Wer ueber das Ende hinaus blaettert -- in die eine wie in die
 * andere Richtung --, landet wieder zu Hause (show_home).
 */
static void browse_step(int dir)
{
    char question[UI_QUESTION_MAX];
    char answer[UI_HINT_TEXT_MAX];
    char position[sizeof(s_position_text)];

    _lock_acquire(&s_lvgl_lock);
#if !UI_WATCH
    if (s_history_count == 0) {
        /*
         * Leerer Verlauf: einmal sagen, dass da nichts ist -- und beim
         * naechsten Druck zurueck. Vorher blieb der Schirm hier stehen, und
         * jeder weitere Druck zeigte dasselbe noch einmal: ein Geraet, das
         * auf Tasten scheinbar nicht mehr reagiert.
         */
        const bool again = s_empty_history_shown;
        _lock_release(&s_lvgl_lock);
        if (again || dir < 0) {
            if (again) {
                ui_status_show_home();
            }
            return;
        }
        set_scene(UI_STATUS_ICON_IDLE, "Verlauf", "Noch keine Antwort", UI_TEXT_HINT);
        _lock_acquire(&s_lvgl_lock);
        s_empty_history_shown = true;
        _lock_release(&s_lvgl_lock);
        return;
    }
#endif
    if (dir < 0 && s_history_cursor < 0) {
        /* Nicht im Verlauf und "neuer" gewuenscht: neuer als jetzt gibt es nicht. */
        _lock_release(&s_lvgl_lock);
        return;
    }
    const int next = s_history_cursor + dir;
    if (next < 0 || next >= (int)s_history_count + HISTORY_FIRST) {
        s_history_cursor = -1;
        _lock_release(&s_lvgl_lock);
        ui_status_show_home();
        return;
    }
    s_history_cursor = (int8_t)next;
#if UI_WATCH
    if (next == 0) {
        /*
         * Die Bilanz-Seite (V9): die erste Seite rechts vom Zifferblatt.
         * Gezaehlt wird lokal ab Mitternacht Ortszeit aus den Eintraegen --
         * Befehle sind die Turns des Tages, erledigt sind angelegte und
         * erledigte Turns samt Karten-Aktionen, Fragen die Antworten.
         */
        unsigned commands = 0;
        unsigned done = 0;
        unsigned answers = 0;
        unsigned errors = 0;
        const int64_t now = (int64_t)time(NULL);
        const int64_t today = ui_history_local_day(now, s_clock_offset_min);
        for (uint8_t i = 0; i < s_history_count; i++) {
            const ui_history_entry_t *e = &s_history[i];
            if (!s_clock_valid || e->at <= 0 ||
                ui_history_local_day(e->at, s_clock_offset_min) != today) {
                continue;
            }
            if (e->kind == UI_HISTORY_CARD) {
                done++;
                continue;
            }
            commands++;
            switch ((ui_outcome_t)e->outcome) {
            case UI_OUTCOME_CREATED:
            case UI_OUTCOME_DONE:
                done++;
                break;
            case UI_OUTCOME_ANSWER:
                answers++;
                break;
            case UI_OUTCOME_ERROR:
                errors++;
                break;
            default:
                break;
            }
        }
        const bool empty = s_history_count == 0;
        snprintf(position, sizeof(position), "0/%u", (unsigned)s_history_count % 100u);
        _lock_release(&s_lvgl_lock);
        if (empty) {
            /* Leerer Verlauf: Tecki schaut zur Seite, der Satz sagt, wie es losgeht. */
            set_scene_full(UI_STATUS_ICON_LOOK_LEFT, "Verlauf", "",
                           "Noch keine Antwort. Sprechtaste halten und fragen.", UI_TEXT_HINT,
                           position);
            return;
        }
        ui_history_format_balance(answer, sizeof(answer), commands, done, answers, errors);
        set_scene_full(UI_STATUS_ICON_IDLE, "Heute", "", answer, UI_TEXT_MESSAGE, position);
        return;
    }
#endif
    /*
     * `s_history_next` zeigt auf den naechsten freien Platz, die juengste
     * Antwort liegt also davor. Das `+ UI_HISTORY_MAX` haelt den Ausdruck
     * positiv, bevor der Rest gebildet wird — in C ist der Rest einer
     * negativen Zahl negativ, und der Zugriff laege ausserhalb des Feldes.
     */
    const int entry_index = s_history_cursor - HISTORY_FIRST;
    const uint8_t index = (uint8_t)((s_history_next + UI_HISTORY_MAX - 1 - entry_index) %
                                    UI_HISTORY_MAX);
    const ui_history_entry_t *entry = &s_history[index];
    strlcpy(question, entry->question, sizeof(question));
    strlcpy(answer, entry->answer, sizeof(answer));
    snprintf(position, sizeof(position), "%u/%u", (unsigned)(entry_index + 1) % 100u,
             (unsigned)s_history_count % 100u);
#if UI_WATCH
    const ui_history_entry_t meta = *entry;
#endif
    _lock_release(&s_lvgl_lock);

    ESP_LOGD(TAG, "Verlauf %s", position);
    _lock_acquire(&s_lvgl_lock);
    assign_scene_locked(UI_STATUS_ICON_IDLE, "Verlauf", question, answer, UI_TEXT_MESSAGE,
                        position, true);
#if UI_WATCH
    /* Marker, Zeit und Chips gehoeren zu genau diesem Eintrag -- nach assign, das sie wegraeumt. */
    s_meta_entry = meta;
    s_meta_shown = true;
#endif
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_browse_history(void)
{
    browse_step(1);
}

bool ui_status_in_history(void)
{
    _lock_acquire(&s_lvgl_lock);
    const bool in_history = s_history_cursor >= 0 || s_empty_history_shown;
    _lock_release(&s_lvgl_lock);
    return in_history;
}

void ui_status_browse_newer(void)
{
    browse_step(-1);
}

/*
 * Abbruch: die Aufnahme laeuft nicht weiter und es geht nichts an Todoteck.
 * Bisher kehrte das Geraet dabei stumm in den Bereitzustand zurueck — nicht
 * zu unterscheiden von "gesendet und nichts gefunden".
 */
void ui_status_set_cancelled(void)
{
    ESP_LOGI(TAG, "abgebrochen");
    set_scene(UI_STATUS_ICON_IDLE, "Abgebrochen", "Nichts gesendet", UI_TEXT_HINT);
}

/*
 * Transkript zuerst: Was die Bruecke verstanden hat, steht da, bevor es
 * ausgefuehrt wird -- mit einer Frist, in der "Abbrechen" es noch aufhaelt.
 * Die Szene endet mit dem naechsten ui_state (jede andere Szene raeumt sie
 * weg, end_transcript_locked). Nach Ablauf der Frist bleibt der Text stehen,
 * nur die Flaeche gilt nicht mehr: Die Bruecke fuehrt dann schon aus.
 */
void ui_status_set_transcript(const char *text, uint32_t cancel_ms)
{
    if (cancel_ms == 0) {
        cancel_ms = 2000;
    } else if (cancel_ms > 10000) {
        cancel_ms = 10000;
    }
    ESP_LOGI(TAG, "Transkript, %u ms zum Abbrechen", (unsigned)cancel_ms);
    _lock_acquire(&s_lvgl_lock);
    assign_scene_locked(UI_STATUS_ICON_TRANSCRIBING, "Verstanden", "", text ? text : "",
                        UI_TEXT_MESSAGE, "", false);
    s_transcript_active = true;
    s_transcript_deadline_us = esp_timer_get_time() + (int64_t)cancel_ms * 1000;
#if UI_WATCH
    const int32_t full = CONTENT_W - 2 * UI_SCREEN_PAD;
    lv_anim_delete(s_cancel_fill, cancel_bar_anim_cb);
    lv_obj_set_width(s_cancel_fill, full);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_cancel_fill);
    lv_anim_set_values(&anim, full, 0);
    lv_anim_set_duration(&anim, cancel_ms);
    lv_anim_set_exec_cb(&anim, cancel_bar_anim_cb);
    lv_anim_start(&anim);
#endif
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

bool ui_status_transcript_shown(void)
{
    _lock_acquire(&s_lvgl_lock);
    const bool shown = s_transcript_active;
    _lock_release(&s_lvgl_lock);
    return shown;
}

bool ui_status_transcript_cancellable(void)
{
    _lock_acquire(&s_lvgl_lock);
    const bool ok = s_transcript_active && esp_timer_get_time() < s_transcript_deadline_us;
    _lock_release(&s_lvgl_lock);
    return ok;
}

bool ui_status_transcript_cancel_at(int16_t x, int16_t y)
{
#if UI_WATCH
    bool hit = false;
    _lock_acquire(&s_lvgl_lock);
    if (s_transcript_active && esp_timer_get_time() < s_transcript_deadline_us &&
        !lv_obj_has_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t area;
        lv_obj_get_coords(s_cancel_btn, &area);
        hit = x >= area.x1 - UI_HIT_TOLERANCE && x <= area.x2 + UI_HIT_TOLERANCE &&
              y >= area.y1 - UI_HIT_TOLERANCE && y <= area.y2 + UI_HIT_TOLERANCE;
    }
    _lock_release(&s_lvgl_lock);
    return hit;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

void ui_status_set_transcript_cancelled(void)
{
    ESP_LOGI(TAG, "Transkript abgebrochen");
    set_scene(UI_STATUS_ICON_IDLE, "Abgebrochen", "Nichts ausgeführt", UI_TEXT_HINT);
}

/*
 * Tippen auf eine Antwort: Kurzform -> ganze Antwort, ganze Antwort ->
 * wieder Kurzform. `false`, wenn es dort nichts umzuschalten gibt; dann
 * entscheidet main.c, was das Tippen sonst tut.
 */
bool ui_status_toggle_more(void)
{
#if UI_WATCH
    bool consumed = false;
    _lock_acquire(&s_lvgl_lock);
    if (s_render_more) {
        s_expanded = true;
        consumed = true;
    } else if (s_expanded && s_shortable) {
        s_expanded = false;
        consumed = true;
    }
    if (consumed) {
        s_answer_page = 0;
    }
    if (consumed) {
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
    return consumed;
#else
    return false;
#endif
}

bool ui_status_choices_open(void)
{
    _lock_acquire(&s_lvgl_lock);
    const bool open = s_choice_count > 0;
    _lock_release(&s_lvgl_lock);
    return open;
}

void ui_status_set_font_level(uint8_t level)
{
    if (level > 2) {
        level = 1;
    }
    _lock_acquire(&s_lvgl_lock);
    if (s_font_level != level) {
        s_font_level = level;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

uint8_t ui_status_font_level(void)
{
    return s_font_level;
}

void ui_status_set_updating(bool updating)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_updating != updating) {
        s_updating = updating;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

/*
 * Ein Druck, der zu kurz war, um etwas zu sagen: die Aufnahme wird verworfen,
 * nichts geht an Todoteck. Die Zeile sagt gleich, was stattdessen zu tun ist —
 * "Abgebrochen" allein liesse offen, warum.
 */
void ui_status_lock(void)
{
    _lock_acquire(&s_lvgl_lock);
}

void ui_status_unlock(void)
{
    _lock_release(&s_lvgl_lock);
}

lv_obj_t *ui_status_screen(void)
{
    return s_screen;
}

void ui_status_format_clock(char *out, size_t size)
{
    format_clock(out, size);
}

void ui_status_format_date(char *out, size_t size)
{
#if UI_WATCH
    format_face_date(out, size);
#else
    if (size > 0) {
        out[0] = '\0';
    }
#endif
}

void ui_status_copy_display_text(char *dst, const char *src, size_t size)
{
    copy_utf8_display(dst, src, size);
}

void ui_status_set_face(const char (*lines)[UI_STATUS_FACE_LEN], uint8_t count,
                        const char *event_title, int64_t event_start,
                        const uint8_t *kinds, uint8_t kind_count)
{
    if (count > UI_STATUS_FACE_LINES) {
        count = UI_STATUS_FACE_LINES;
    }
    _lock_acquire(&s_lvgl_lock);
    for (uint8_t i = 0; i < count; i++) {
        copy_utf8_display(s_face[i], lines[i], sizeof(s_face[i]));
    }
    s_face_count = count;
    /* `kinds` gilt nur, wenn es zu den Zeilen passt (gleiche Laenge) -- sonst wie bis 0.4.6. */
    s_face_kind_count = kinds && kind_count == count ? count : 0;
    for (uint8_t i = 0; i < s_face_kind_count; i++) {
        s_face_kinds[i] = kinds[i];
    }
    copy_utf8_display(s_event_title, event_title ? event_title : "", sizeof(s_event_title));
    s_event_start = event_start;
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

static void mood_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    s_mood_timer = NULL;
    s_mood = UI_STATUS_MOOD_NONE;
    render_current_locked();
}

void ui_status_set_mood(ui_status_mood_t mood, uint32_t duration_ms)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_mood_timer) {
        lv_timer_delete(s_mood_timer);
        s_mood_timer = NULL;
    }
    s_mood = mood;
    if (mood != UI_STATUS_MOOD_NONE && duration_ms > 0) {
        s_mood_timer = lv_timer_create(mood_expired_cb, duration_ms, NULL);
        lv_timer_set_repeat_count(s_mood_timer, 1);
    }
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_choices(const char (*choices)[UI_STATUS_CHOICE_LEN], uint8_t count)
{
#if UI_WATCH
    if (count > UI_STATUS_CHOICES_MAX) {
        count = UI_STATUS_CHOICES_MAX;
    }
    _lock_acquire(&s_lvgl_lock);
    for (uint8_t i = 0; i < count; i++) {
        copy_utf8_display(s_choices[i], choices[i], sizeof(s_choices[i]));
        strlcpy(s_choice_raw[i], choices[i], sizeof(s_choice_raw[i]));
    }
    s_choice_count = count;
    render_current_locked();
    _lock_release(&s_lvgl_lock);
#else
    (void)choices;
    (void)count;
#endif
}

int ui_status_choice_at(int16_t x, int16_t y, char *out, size_t out_size)
{
    int hit = -1;
    _lock_acquire(&s_lvgl_lock);
    for (int i = 0; i < s_choice_count && i < UI_STATUS_CHOICES_MAX; i++) {
        if (!s_choice_btn[i] || lv_obj_has_flag(s_choice_btn[i], LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(s_choice_btn[i], &area);
        /*
         * Etwas grosszuegiger als die Flaeche selbst: Ein Daumen trifft am
         * Handgelenk selten genau, und daneben liegt nur der Text.
         */
        if (x >= area.x1 - 8 && x <= area.x2 + 8 && y >= area.y1 - 16 && y <= area.y2 + 16) {
            hit = i;
            if (out && out_size) {
                strlcpy(out, s_choice_raw[i][0] ? s_choice_raw[i] : s_choices[i], out_size);
            }
            break;
        }
    }
    _lock_release(&s_lvgl_lock);
    return hit;
}

void ui_status_set_not_connected(void)
{
    ESP_LOGI(TAG, "Sprechtaste ohne Bruecke");
    set_scene(UI_STATUS_ICON_PAIRING, "Nicht verbunden",
              "Todoteck-Brücke auf dem Handy oder PC starten", UI_TEXT_HINT);
}

void ui_status_set_press_too_short(void)
{
    ESP_LOGI(TAG, "Druck zu kurz");
    set_scene(UI_STATUS_ICON_IDLE, "Zu kurz", "Taste halten, solange du sprichst",
              UI_TEXT_HINT);
}

/*
 * Gesendet wurde etwas, gesprochen aber nicht: entweder war die Aufnahme
 * still, oder die Erkennung hat aus der Stille einen Satz erfunden (der
 * Server faengt beides ab und schickt diesen Zustand statt einer Antwort).
 *
 * Bewusst kein Fehler-Bild: Nichts ist kaputt, der Ausloeser hat nur in der
 * Tasche gedrueckt. Wer hier ein rotes Kreuz sieht, sucht den Fehler in der
 * Einrichtung -- und findet keinen.
 */
void ui_status_set_no_speech(const char *hint)
{
    ESP_LOGI(TAG, "nichts gehoert");
    set_scene(UI_STATUS_ICON_IDLE, "Nichts gehört",
              (hint && hint[0]) ? hint : "Halten und sprechen", UI_TEXT_HINT);
}

/*
 * Der echte Verbindungszustand zur Bruecke. Faerbt nur den Punkt in der
 * Kopfzeile — die Szene bleibt, wie sie ist.
 */
/*
 * Die Zeit kommt von der Bruecke, weil das Geraet keine eigene hat.
 *
 * `epoch_utc` ist die uebliche Sekundenzaehlung seit 1970 in UTC,
 * `tz_offset_min` der Abstand der Ortszeit dazu in Minuten (also 120 fuer
 * MESZ). Gestellt wird die Systemuhr; die laeuft im Tiefschlaf weiter, die
 * Uhrzeit ueberlebt also das Aufwachen. Genau nimmt sie es dabei nicht — der
 * Modul-Oszillator driftet, deshalb schickt die Bruecke die Zeit bei jedem
 * Verbinden neu und die Anzeige zeigt bewusst keine Sekunden.
 */
void ui_status_set_clock(int64_t epoch_utc, int32_t tz_offset_min)
{
    if (epoch_utc <= 0) {
        return;
    }
    const struct timeval now = { .tv_sec = (time_t)epoch_utc, .tv_usec = 0 };
    settimeofday(&now, NULL);

    _lock_acquire(&s_lvgl_lock);
    s_clock_offset_min = tz_offset_min;
    s_clock_valid = true;
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

/*
 * Nach dem Aufwachen aus dem Tiefschlaf: Die Systemuhr laeuft weiter, der
 * Abstand zur UTC lag aber im normalen RAM und ist weg. Er kommt aus dem
 * RTC-Speicher zurueck (main.c), und erst damit darf wieder eine Uhrzeit auf
 * dem Schirm stehen.
 */
void ui_status_restore_clock(int32_t tz_offset_min)
{
    _lock_acquire(&s_lvgl_lock);
    s_clock_offset_min = tz_offset_min;
    s_clock_valid = true;
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_link(bool connected)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_link_connected != connected) {
        s_link_connected = connected;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

/*
 * Pegel und Restzeit waehrend der Aufnahme, beide in Prozent. Wird vom
 * Aufnahme-Takt in main.c gespeist; ausserhalb der Aufnahme sind die Balken
 * ausgeblendet, der Aufruf ist dann folgenlos.
 */
void ui_status_set_recording_meter(uint8_t level_percent, uint8_t remaining_percent)
{
    if (level_percent > 100) {
        level_percent = 100;
    }
    if (remaining_percent > 100) {
        remaining_percent = 100;
    }

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
        lv_obj_set_width(s_meter_fill, MAX(2, (METER_WIDTH * level_percent) / 100));
        lv_obj_set_width(s_time_fill, MAX(1, (METER_WIDTH * remaining_percent) / 100));
        lv_obj_set_style_bg_color(s_time_fill,
                                  remaining_percent <= 20 ? lv_color_hex(0xf97373) :
                                  lv_color_hex(0xf2b23c),
                                  0);
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_idle_dimmed(bool dimmed)
{
    ESP_LOGD(TAG, "idle dimmed=%d", dimmed);
    _lock_acquire(&s_lvgl_lock);
    if (s_dimmed != dimmed) {
        s_dimmed = dimmed;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_recording(uint32_t session_id)
{
    ESP_LOGD(TAG, "recording session %" PRIu32, session_id);
    (void)session_id;

    _lock_acquire(&s_lvgl_lock);
    end_transcript_locked();
    s_shortable = false;
    s_expanded = false;
    s_thinking_since_us = 0;
    if (s_thinking_timer) {
        lv_timer_pause(s_thinking_timer);
    }
    s_scene = UI_STATUS_ICON_RECORDING;
    s_text_kind = UI_TEXT_HINT;
    /* "Höre zu" seit 0.4.4 (Paket I) -- die Uhr spricht in der ersten Person wie "Verstehe ...". */
    copy_utf8_display(s_status_text, "Höre zu", sizeof(s_status_text));
    strlcpy(s_hint_text, "Sprich jetzt", sizeof(s_hint_text));
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_battery(int level_percent, bool charging, bool usb_powered)
{
    if (level_percent < 0) {
        level_percent = 0;
    } else if (level_percent > 100) {
        level_percent = 100;
    }

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
        /*
         * Die Zahl traegt jetzt auch die Farbe: rot unter 20 Prozent, blau
         * am Ladegeraet, sonst gedaempft. Vorher sagte das der Fuellstand
         * des Symbols — das Symbol ist weg, die Aussage bleibt.
         */
        s_battery_low = level_percent <= 10;
        const bool was_tired = s_battery_percent >= 0 &&
                               s_battery_percent <= TECKI_TIRED_PERCENT && !s_battery_charging;
        s_battery_charging = charging || usb_powered;
        s_battery_percent = level_percent;
        lv_label_set_text_fmt(s_battery_label, "%d%%", level_percent);
        apply_battery_colour_locked();
        /* Teckis Statuspose haengt am Akku -- nur bei einem Wechsel neu zeichnen. */
        const bool tired = level_percent <= TECKI_TIRED_PERCENT && !s_battery_charging;
        if (UI_WATCH && tired != was_tired) {
            render_current_locked();
        }
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_battery_remaining(const char *line)
{
    char copy[sizeof(s_battery_remaining)];
    copy_utf8_display(copy, line ? line : "", sizeof(copy));
    _lock_acquire(&s_lvgl_lock);
    if (strcmp(copy, s_battery_remaining) != 0) {
        strlcpy(s_battery_remaining, copy, sizeof(s_battery_remaining));
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_night_mode(bool on)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_night_mode != on) {
        s_night_mode = on;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

bool ui_status_night_mode(void)
{
    return s_night_mode;
}

void ui_status_set_night_tint(bool on)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_night_tint != on) {
        s_night_tint = on;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_face_layout(ui_face_layout_t layout)
{
    if ((int)layout < 0 || layout >= UI_FACE_COUNT) {
        layout = UI_FACE_INFO;
    }
    _lock_acquire(&s_lvgl_lock);
    if (s_face_layout != layout) {
        s_face_layout = layout;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

bool ui_status_face_shown(void)
{
    _lock_acquire(&s_lvgl_lock);
    const bool shown = s_face_shown && !s_dimmed;
    _lock_release(&s_lvgl_lock);
    return shown;
}

void ui_status_set_thinking(const char *status, const char *text, int64_t since_us)
{
    ESP_LOGD(TAG, "thinking: %s", status ? status : "");
    _lock_acquire(&s_lvgl_lock);
    assign_scene_locked(UI_STATUS_ICON_TRANSCRIBING, status ? status : "Denkt nach", "",
                        text ? text : "", UI_TEXT_MESSAGE, "", false);
    s_thinking_since_us = since_us;
    if (s_thinking_timer && since_us > 0) {
        lv_timer_resume(s_thinking_timer);
    }
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_partial_text(const char *text)
{
    ESP_LOGD(TAG, "partial: %s", text ? text : "");
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "Denkt nach", text ? text : "", UI_TEXT_MESSAGE);
}

void ui_status_set_ota_progress(uint32_t written, uint32_t size)
{
    char hint[32];
    uint32_t percent = 0;
    if (size > 0) {
        percent = MIN(100, (written * 100) / size);
    }
    snprintf(hint, sizeof(hint), "%" PRIu32 "%%", percent);
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "Aktualisiert", hint, UI_TEXT_HINT);
}

void ui_status_set_ota_rebooting(void)
{
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "Neustart", "Firmware aktualisiert", UI_TEXT_HINT);
}

void ui_status_set_error(const char *message)
{
    ESP_LOGE(TAG, "%s", message ? message : "unknown error");
    set_scene(UI_STATUS_ICON_ERROR, "", message ? message : "Unbekannter Fehler",
              UI_TEXT_MESSAGE);
}

bool ui_status_answer_scroll(int dir)
{
#if UI_WATCH
    bool moved = false;
    _lock_acquire(&s_lvgl_lock);
    if (s_answer_pages > 1) {
        const int next = (int)s_answer_page + (dir > 0 ? 1 : -1);
        if (next >= 0 && next < (int)s_answer_pages) {
            s_answer_page = (uint8_t)next;
            /* Mit Animation -- der "Schwung" einer Geste, die LVGL selbst nie sieht. */
            lv_obj_scroll_to_y(s_hint_label, next * s_answer_page_h, LV_ANIM_ON);
            ui_dots_set(&s_answer_dots, s_answer_pages, s_answer_page);
            moved = true;
        }
    }
    _lock_release(&s_lvgl_lock);
    return moved;
#else
    (void)dir;
    return false;
#endif
}

/* ── Paket J, Teil 2 (0.5.0) ───────────────────────────────────────────── */

ui_face_hit_t ui_face_hit_from_name(const char *name)
{
    static const char *const NAMES[] = { "task", "weather", "waste", "event" };
    for (int i = 0; i < 4; i++) {
        if (name && strcmp(name, NAMES[i]) == 0) {
            return (ui_face_hit_t)(UI_FACE_HIT_TASK + i);
        }
    }
    return UI_FACE_HIT_OTHER;
}

/*
 * Tippen auf eine Zifferblatt-Zeile (Punkt 13). Getroffen ist das Label,
 * dessen Mitte dem Finger am naechsten liegt -- senkrecht mit einem halben
 * Zeilenabstand Toleranz, damit die 34-px-Zeilen nahtlos aneinander
 * grenzen. Nur, solange das Zifferblatt steht und die Zeile eine Art traegt.
 */
ui_face_hit_t ui_status_face_hit_at(int16_t x, int16_t y)
{
    ui_face_hit_t hit = UI_FACE_HIT_NONE;
#if UI_WATCH
    _lock_acquire(&s_lvgl_lock);
    int32_t best = -1;
    for (int i = 0; i < FACE_LABELS; i++) {
        if (!s_face_label[i] || s_face_label_kind[i] == UI_FACE_HIT_NONE ||
            lv_obj_has_flag(s_face_label[i], LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(s_face_label[i], &area);
        const int32_t cy = (area.y1 + area.y2) / 2;
        const int32_t dy = y > cy ? y - cy : cy - y;
        if (x < area.x1 - UI_HIT_TOLERANCE || x > area.x2 + UI_HIT_TOLERANCE ||
            dy > FACE_LINE_H / 2 + UI_HIT_TOLERANCE) {
            continue;
        }
        if (best < 0 || dy < best) {
            best = dy;
            hit = (ui_face_hit_t)s_face_label_kind[i];
        }
    }
    _lock_release(&s_lvgl_lock);
#else
    (void)x;
    (void)y;
#endif
    return hit;
}

void ui_status_set_locked(bool locked)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_face_locked != locked) {
        s_face_locked = locked;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_show_latest(void)
{
    _lock_acquire(&s_lvgl_lock);
    /* Ein Schritt "aelter" von hier aus landet genau auf der juengsten Antwort. */
    s_history_cursor = (int8_t)(s_history_count > 0 ? HISTORY_FIRST - 1 : -1);
    s_empty_history_shown = false;
    _lock_release(&s_lvgl_lock);
    browse_step(1);
}

void ui_status_history_add_note(const char *line)
{
    _lock_acquire(&s_lvgl_lock);
    ui_history_entry_t *entry = history_push_locked();
    copy_utf8_display(entry->answer, line ? line : "", sizeof(entry->answer));
    entry->kind = UI_HISTORY_TURN;
    entry->outcome = UI_OUTCOME_ANSWER;
    _lock_release(&s_lvgl_lock);
}

/* Sperre gehalten (wie ui_status_format_clock fuer die Vorschau): liest nur die Uhr-Felder. */
void ui_status_format_time_of_locked(int64_t epoch_utc, char *out, size_t size)
{
    const bool valid = s_clock_valid;
    const int32_t offset = s_clock_offset_min;
    if (!valid || epoch_utc <= 0 || size == 0) {
        if (size) {
            out[0] = '\0';
        }
        return;
    }
    const time_t local = (time_t)epoch_utc + (time_t)offset * 60;
    struct tm parts;
    gmtime_r(&local, &parts);
    snprintf(out, size, "%02u:%02u", (unsigned)parts.tm_hour % 24u, (unsigned)parts.tm_min % 60u);
}
