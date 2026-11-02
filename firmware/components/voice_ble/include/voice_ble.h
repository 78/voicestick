#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VOICE_BLE_DEVICE_NAME_PREFIX "VS"

#define VOICE_BLE_FLAG_START 0x01
#define VOICE_BLE_FLAG_END   0x02

#define VOICE_BLE_OTA_TYPE_BEGIN 0x20
#define VOICE_BLE_OTA_TYPE_DATA  0x21
#define VOICE_BLE_OTA_TYPE_END   0x22
#define VOICE_BLE_OTA_TYPE_ABORT 0x23
#define VOICE_BLE_OTA_TYPE_STATE 0x30

typedef void (*voice_ble_connection_cb_t)(bool connected);
typedef void (*voice_ble_control_cb_t)(const char *json);

typedef enum {
    VOICE_BLE_OTA_EVENT_BEGIN,
    VOICE_BLE_OTA_EVENT_PROGRESS,
    VOICE_BLE_OTA_EVENT_DONE,
    VOICE_BLE_OTA_EVENT_ERROR,
    VOICE_BLE_OTA_EVENT_ABORT,
} voice_ble_ota_event_t;

typedef void (*voice_ble_ota_cb_t)(voice_ble_ota_event_t event,
                                   uint32_t written,
                                   uint32_t size);

esp_err_t voice_ble_init(void);
void voice_ble_set_connection_callback(voice_ble_connection_cb_t callback);
void voice_ble_set_control_callback(voice_ble_control_cb_t callback);
void voice_ble_set_ota_callback(voice_ble_ota_cb_t callback);
const char *voice_ble_device_id(void);
const char *voice_ble_device_name(void);
bool voice_ble_is_connected(void);
bool voice_ble_is_ready(void);
bool voice_ble_ota_is_active(void);
esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                               const uint8_t *opus_payload, size_t len);
/*
 * Verbindungsintervall: schnell (15-30 ms) fuers Aufnehmen, OTA und
 * Listen-/Wetter-Uebertragung, langsam (400-500 ms, Latency 4) im Leerlauf.
 */
esp_err_t voice_ble_request_fast_interval(void);
esp_err_t voice_ble_request_slow_interval(void);
/*
 * Unverbunden: die schnellen 30 Sekunden Werbung (60-120 ms) neu starten,
 * etwa nach einem Tastendruck. Verbunden oder schon schnell: tut nichts.
 */
void voice_ble_advertise_fast(void);
esp_err_t voice_ble_send_device_info(void);
esp_err_t voice_ble_send_button_down(const char *button, uint32_t session_id);
esp_err_t voice_ble_send_button_up(const char *button, uint32_t duration_ms,
                                   uint32_t session_id);
esp_err_t voice_ble_send_button_click(const char *button, uint32_t duration_ms,
                                      uint32_t session_id);

/*
 * Sagt der Gegenstelle, dass diese Aufnahme verworfen wird.
 *
 * Ohne das Ereignis waere ein Abbruch nicht von einem normalen Ende zu
 * unterscheiden: der Abbau des Aufnahmepfads schickt in beiden Faellen einen
 * Ende-Rahmen, und die Bruecke wuerde ihn wie sonst auch an Todoteck senden.
 * Das Ereignis geht vor dem Ende-Rahmen raus.
 *
 * Bewusst nicht "cancel": diesen Namen hat das Protokoll abgekuendigt, weil
 * er eine App-Bedeutung transportierte. Hier wird eine Geraetetatsache
 * gemeldet — diese Sitzung ist verworfen —, und was die Gegenstelle daraus
 * macht, bleibt ihre Sache.
 */
/* Antwort auf eine Rueckfrage, angetippt auf dem Schirm (siehe ui_state `choices`). */
esp_err_t voice_ble_send_choice(uint8_t index, const char *text);
/* Zeile einer Karte angetippt (Uhr): Art ("today", "shopping", "switches"), Stand, Position. */
esp_err_t voice_ble_send_item(const char *kind, uint16_t rev, uint8_t index);
/*
 * Auswahl eines input_select (Zeile mit Flag `s`, 0.4.5): erst die Bitte um
 * die Optionen der Zeile -- die Bruecke antwortet mit `options` auf
 * `control_rx` --, dann die gewaehlte Option als Index in deren `items`.
 * Koordinaten wie bei `item`. docs/protocol.md, 0.4.5.
 */
esp_err_t voice_ble_send_options_request(const char *kind, uint16_t rev, uint8_t index);
esp_err_t voice_ble_send_option(const char *kind, uint16_t rev, uint8_t index, uint8_t choice);
/* Bitte an die Bruecke, Zifferblatt und Karten jetzt neu zu schicken. */
esp_err_t voice_ble_send_glance_request(void);
/* Aktionstaste der Uhr (PWR lang): was sie tut, legt die Bruecke fest. */
esp_err_t voice_ble_send_action(void);
/*
 * Zifferblatt-Layout an der Uhr gewaehlt ("info", "minimal", "large"): die
 * Bruecke speichert es beim Server und schickt `watch_settings` neu (0.4.4).
 */
esp_err_t voice_ble_send_face_layout(const char *face);
/* "Abbrechen" in der Transkript-Szene der Uhr: die Bruecke fuehrt den Satz nicht aus. */
esp_err_t voice_ble_send_cancel_transcript(void);
/*
 * "Rueckgaengig" einer Karten-Aktion: Art, Stand und Position wie beim `item`
 * davor -- die Bruecke haelt die letzten Aktionen 15 Minuten und findet sie
 * daran wieder. Aeltere Bruecken lesen nur das Ereignis und nehmen die
 * letzte Aktion zurueck.
 */
esp_err_t voice_ble_send_undo(const char *kind, uint16_t rev, uint8_t index);
/* "Rueckgaengig" eines Turns aus dem Verlauf: `ref` kam mit dem `ui_state` der Antwort. */
esp_err_t voice_ble_send_undo_turn(uint16_t ref);
/* "Nochmal" aus dem Verlauf: die Bruecke fuehrt die Frage als Text-Turn erneut aus. */
esp_err_t voice_ble_send_repeat(const char *text);
/* Rahmen an `speaker_rx` (Typbyte + Nutzlast), aus dem Bluetooth-Strang gerufen. */
typedef void (*voice_ble_speaker_cb_t)(const uint8_t *data, size_t len);
void voice_ble_set_speaker_callback(voice_ble_speaker_cb_t cb);

esp_err_t voice_ble_send_recording_discarded(uint32_t session_id);
/*
 * Ein fertiges State-Ereignis (JSON, hoechstens 240 Byte) an `state_tx` --
 * fuer Antworten, die main.c selbst zusammensetzt, etwa die Teile des
 * Akku-Protokolls (`battery_log`, docs/protocol.md).
 */
esp_err_t voice_ble_send_state_json(const char *json);
