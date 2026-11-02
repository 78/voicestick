#pragma once

/*
 * Layout der Waveshare-Uhr (AMOLED 410x502, hochkant am Handgelenk).
 *
 * Dieselben Namen wie die Masse in ui_status.c fuer den StickS3, damit die
 * Logik dort -- Verlauf, Ziffernblatt, "passt der Text neben/unter Tecki?" --
 * fuer beide Geraete dieselbe bleibt. Was sich unterscheidet, ist die
 * Richtung: Beim StickS3 steht Tecki **neben** dem Text in einer Spalte, hier
 * steht er **darueber**. Die Textspalte ist deshalb die ganze Breite
 * (COL_X 0, COL_W CONTENT_W), und "neben Tecki" heisst "unter Tecki" -- die
 * Entscheidung in plan_text_layout() faellt am gemessenen Text wie gehabt: Passt
 * die Auskunft unter die Figur, bleibt sie stehen, sonst weicht sie.
 *
 * Die Schriftgroessen sind nicht einfach hochgerechnet. Der Schirm hat rund
 * 315 ppi gegenueber gut 240 beim StickS3; 16 px dort sind 1,7 mm, 30 px hier
 * 2,4 mm. Die Uhr bekommt also auch physisch die groessere Schrift -- sie
 * hat die Flaeche dafuer.
 */

#include "lvgl.h"

LV_FONT_DECLARE(todoteck_watch_font_30);
LV_FONT_DECLARE(todoteck_watch_font_26);
LV_FONT_DECLARE(todoteck_watch_font_23);
LV_FONT_DECLARE(todoteck_watch_font_20);
LV_FONT_DECLARE(todoteck_watch_font_18);
LV_FONT_DECLARE(todoteck_watch_clock_120);
LV_FONT_DECLARE(todoteck_watch_clock_180);
LV_FONT_DECLARE(todoteck_clock_64);

#define LCD_H_RES 410
#define LCD_V_RES 502

/*
 * 16 Pixel Rand statt 8: Die Ecken des AMOLED sind gerundet, und am Rand
 * abgeschnittene Buchstaben liest niemand.
 */
#define UI_SCREEN_PAD 16
#define CONTENT_W (LCD_H_RES - 2 * UI_SCREEN_PAD)
#define CONTENT_H (LCD_V_RES - 2 * UI_SCREEN_PAD)

/*
 * Kopfzeile: Verbindungspunkt links, die Uhrzeit klein in der Mitte (ausser
 * auf dem Ziffernblatt und bei der Taschenlampe), Akku rechts. Die Position
 * im Verlauf steht auf der Uhr als Punktreihe unten, nicht mehr hier.
 */
#define HEADER_H 34
#define UI_DOT_SIZE 14
#define UI_DOT_Y 12
#define UI_HEADER_LABEL_Y 8
#define UI_BATTERY_W 70
/*
 * Die Ecken des Glases sind stark gerundet: Am ersten Geraet war von der
 * Akkuzahl in der rechten oberen Ecke nur die erste Ziffer zu sehen. Punkt,
 * Verlaufsposition und Akku ruecken deshalb um so viel nach innen, wie die
 * Rundung auf Hoehe der Kopfzeile wegnimmt.
 */
#define UI_HEADER_INSET_X 34
#define UI_POSITION_X (UI_HEADER_INSET_X + 24)

/* Tecki mittig oben; die Flaeche selbst steht in ui_status_icons.h. */
#define TECKI_BOX UI_TECKI_BOX
#define TECKI_X ((CONTENT_W - TECKI_BOX) / 2)
#define TECKI_TOP_Y 36

/* "Neben Tecki" heisst hier "unter Tecki", ueber die volle Breite. */
#define COL_X 0
#define COL_W CONTENT_W

#define METER_WIDTH 300
#define METER_LEVEL_HEIGHT 12
#define METER_TIME_HEIGHT 6
#define METER_X ((CONTENT_W - METER_WIDTH) / 2)

#define TEXT_BOTTOM_Y (CONTENT_H - 4)
#define STATUS_Y_WITH_TECKI (TECKI_TOP_Y + TECKI_BOX + 10)
#define STATUS_Y_TEXT_ONLY (HEADER_H + 6)
#define TEXT_GAP 8

/* Pegel und Restzeit unter der Statuszeile ("Hoert zu"). */
#define METER_LEVEL_Y (STATUS_Y_WITH_TECKI + 36 + 20)
#define METER_TIME_Y (METER_LEVEL_Y + METER_LEVEL_HEIGHT + 12)

/* "Koppeln": Wort und kleine Uhr unter der grossen Figur, Hinweis unten. */
#define CLOCK_STATUS_Y STATUS_Y_WITH_TECKI
#define CLOCK_Y (STATUS_Y_WITH_TECKI + 44)
#define CLOCK_HINT_Y (CONTENT_H - 40)

/*
 * Das Ziffernblatt: Tecki klein unten mittig, die Uhrzeit oben an **einer**
 * festen Stelle -- 36 px unter der Kopfzeile, ob nun Zeilen (Termin,
 * Aufgabe, Wetter) darunter stehen oder nicht. Bis 0.3.5 rueckte sie ohne
 * Zeilen in die Mitte zwischen Kopfzeile und Figur und sprang damit um gut
 * 120 px, sobald die Bruecke eine Zeile schickte oder wieder zuruecknahm.
 * Ohne Zeilen bleibt der Platz darunter frei. "04:44" -- die breiteste
 * Uhrzeit -- misst in der 120er rund 354 der 378 Pixel.
 */
#define CLOCK_FACE_TECKI_X ((CONTENT_W - UI_TECKI_BOX_SMALL) / 2)
#define CLOCK_FACE_TECKI_Y (CONTENT_H - UI_TECKI_BOX_SMALL)
#define CLOCK_FACE_Y (HEADER_H + 36)
#define CLOCK_TICK_MS 10000

#define UI_FONT_STATUS (&todoteck_watch_font_30)
#define UI_FONT_QUESTION (&todoteck_watch_font_20)
#define UI_FONT_SMALL (&todoteck_watch_font_18)
#define UI_FONT_CLOCK_FACE (&todoteck_watch_clock_120)
/*
 * Layout "Gross" (0.4.4): Stunden und Minuten zweizeilig in 180 px -- nur
 * Ziffern, Doppelpunkt und Strich wie die 120er, "00" misst rund 242 px.
 */
#define UI_FONT_CLOCK_LARGE (&todoteck_watch_clock_180)
#define UI_FONT_CLOCK_SMALL (&todoteck_clock_64)

/* Zeilenabstand je Stufe auf rund das 1,35-fache der Schriftgroesse. */
#define UI_TEXT_STEPS_INIT                 \
    {                                      \
        { &todoteck_watch_font_30, 4 },    \
        { &todoteck_watch_font_26, 3 },    \
        { &todoteck_watch_font_23, 3 },    \
        { &todoteck_watch_font_20, 2 },    \
        { &todoteck_watch_font_18, 1 },    \
    }

#define QUESTION_LINES 2
#define QUESTION_LINE_SPACE 2

/*
 * AMOLED: Schwarz kostet keinen Strom, jedes helle Pixel schon. Die Uhr
 * zeigt deshalb in jeder Szene dunklen Grund -- der cremeweisse des StickS3
 * waere hier die teuerste aller Farben.
 */
#define UI_DARK_THEME 1

/*
 * Trefferflaechen der Uhr -- jede tippbare Flaeche, geprueft gegen zwei
 * Regeln: mindestens 48 px in beiden Richtungen, und kein Mittelpunkt in den
 * Eckrundungen des Glases (rund UI_HEADER_INSET_X = 34 px von beiden
 * Raendern, also x < 34 oder x > 376 bei zugleich y < 34 oder y > 468).
 * Getroffen wird mit mindestens UI_HIT_TOLERANCE Zugabe ringsum; wo sich
 * zwei Zugaben im Zwischenraum ueberlappen, zaehlt die naehere Mitte.
 * Koordinaten absolut (Schirm 410 x 502), Mitte als (x, y).
 *
 *   Flaeche                Datei / Funktion               Groesse (B x H)   Mitte            Zugabe
 *   ---------------------  -----------------------------  ----------------  ---------------  ---------
 *   Auswahl Ja/Nein (1-3)  ui_status.c choice_at          119..378 x 72     y 446; x 75..333 8 / 16
 *   Abbrechen (Transkript) ui_status.c transcript_cancel  346 x 64          (205, 450)       6
 *   "Tippen fuer mehr"     ui_status.c toggle_more        280 x 52          (205, 436..456)  ganzer
 *                                                                                            Schirm
 *   Nochmal / Rueckgaengig ui_status.c history_chip_at    170 x 56          y 438; x 115 /   6
 *   (Verlauf, 1-2 Chips)                                                    295 (einer: 205)
 *   Kartenzeile  klein     ui_cards.c ui_cards_touch      378 x 59          x 205            6
 *   Kartenzeile  normal    ui_cards.c ui_cards_touch      378 x 72          x 205            6
 *   Kartenzeile  gross     ui_cards.c ui_cards_touch      378 x 72          x 205            6
 *   Rueckgaengig           ui_cards.c ui_cards_touch      240 x 56          (205, 434)       6
 *   OK - erledigt /        ui_cards.c ui_cards_touch      150 x 59/72/72    x 319, in der    6, Vorrang
 *   OK - schalten                                                           markierten Zeile vor Zeilen
 *   (Heute; Schalter `c`)                                                   (y 72..462)      bei gleicher
 *                                                                                            Mitte
 *   Kachel (2 x 3)         ui_control.c tile_at           183 x 134         x 107 / 302,     6
 *                                                                           y 127/273/419
 *   Ziffernblatt: Tippen   main.c handle_touch            ganzer Schirm     --               --
 *   Ziffernblatt: lange    main.c handle_touch            ganzer Schirm     --               --
 *   druecken (0.4.4)
 *   Layout-Auswahl: Tippen ui_face_picker.c               ganzer Schirm     --               --
 *   Option (input_select,  ui_options.c ui_options_touch  378 x 56..59      x 205            6
 *   0.4.5, bis 8 Zeilen)
 *   Wetterkarte            ui_weather.c                   keine (reine Anzeige)
 *
 * Die schmalste Flaeche ist die "Tippen fuer mehr"-Flaeche mit 52 px. Die
 * Kartenzeilen sind seit 0.3.6 auf zwei Textzeilen ausgelegt und damit
 * hoeher als die Regel verlangt: Ihre Hoehe faellt aus dem sichtbaren
 * Ausschnitt (ROWS_BOTTOM - ROWS_TOP = 390) geteilt durch die Zeilen, die
 * hineinpassen (klein 6, normal 5, gross 5 -- ui_cards.c rows_per_view),
 * minus 6 px Luecke: 390 / 6 - 6 = 59 bzw. 390 / 5 - 6 = 72. Seit 0.4.0
 * scrollt die Liste; getroffen werden die gerade sichtbaren Zeilen (eine am
 * Rand angeschnittene mit), Trennzeilen der Gruppen (28 px) sind kein Ziel.
 * Die Ecken sind ueberall frei: Selbst die aeusseren Ja/Nein-Flaechen liegen
 * mit x 75 bzw. 333 weit innerhalb. Der OK-Chip liegt rechts in der
 * markierten Zeile und ist so hoch wie sie; seine rechte Kante (x 394)
 * reicht an den Rand, die Mitte (x 319) bleibt weit vor der Rundung. Die
 * zwei Verlaufs-Chips (170 x 56, 10 px Luecke) beginnen bei x 30 und enden
 * bei x 380; ihre Mitten liegen bei 115 und 295.
 */
#define UI_HIT_TOLERANCE 6
