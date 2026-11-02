#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "cJSON.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"
#include "hal/gpio_types.h"
#include "iot_button.h"

#include "audio_pipeline.h"
#include "audio_tone.h"
#include "battery_estimate.h"
#include "battery_log.h"
#include "voice_board.h"
#include "esp_ota_ops.h"
#include "ui_cards.h"
#include "ui_control.h"
#include "ui_history_text.h"
#include "ui_options.h"
#include "ui_status.h"
#include "ui_weather.h"
#include "voice_ble.h"
#include "wrist_wake.h"

static const char *TAG = "voice_stick";

#define BATTERY_REFRESH_FALLBACK_MS (10 * 1000)
/*
 * Abdunkeln nach 15 statt 30 Sekunden (08/2026). Der helle Schirm ist der
 * teuerste Zustand des Geraets, und abgedunkelt bleibt die Uhrzeit lesbar --
 * das Fenster, in dem beides gilt, darf deshalb kurz sein. Der Tiefschlaf
 * bleibt bei fuenf Minuten: Aus ihm zurueck kostet einen Neustart samt neuer
 * BLE-Verbindung, das Aufhellen dagegen nur einen Tastendruck.
 */
#define DISPLAY_DIM_TIMEOUT_MS (15 * 1000)
/*
 * Aus statt nur gedimmt (Paket H, Punkt 3, nur die Uhr): 15 Sekunden nach
 * dem Abdunkeln geht das AMOLED ganz aus (Sleep-In des Controllers). Ein
 * gedimmtes AMOLED leuchtet weiter pixelweise; aus kostet es fast nichts.
 * Handgelenk heben, Tippen oder Taste holen es zurueck -- der Touch bleibt
 * wach, nur der Controller schlaeft. Im Nachtlade-Modus bleibt die dunkle
 * Uhrzeit stehen, dort gibt es kein Aus.
 */
#define DISPLAY_OFF_TIMEOUT_MS (15 * 1000)
#define DISPLAY_OFF_TIMEOUT_US (DISPLAY_OFF_TIMEOUT_MS * 1000ULL)
#define DISPLAY_ACTIVE_BRIGHTNESS 128
#define DISPLAY_DIM_BRIGHTNESS 32
#define DISPLAY_DIM_TIMEOUT_US (DISPLAY_DIM_TIMEOUT_MS * 1000ULL)
#define BATTERY_REFRESH_FALLBACK_US (BATTERY_REFRESH_FALLBACK_MS * 1000ULL)
#define DEEP_SLEEP_TIMEOUT_MS (5 * 60 * 1000)
#define DEEP_SLEEP_TIMEOUT_US (DEEP_SLEEP_TIMEOUT_MS * 1000ULL)
/*
 * Handgelenk-Geste stillgelegt (08/2026).
 *
 * Aus dem gedunkelten Schirm soll nur ein Tastendruck in den hellen Zustand
 * fuehren, keine Bewegung. Aus dem Tiefschlaf war das ohnehin schon so (ext1
 * auf beide Tasten); der Schritt "gedunkelt -> hell" ist der einzige, den die
 * Geste je bedient hat.
 *
 * Stillgelegt statt entfernt: Die Komponente wrist_wake bleibt vollstaendig
 * im Baum. Auf 1 gesetzt ist die Geste wieder da. Auf 0 wird der BMI270 gar
 * nicht erst aufgesetzt — kein Abfrage-Task, kein Sensorstrom, und die
 * uebrigen Aufrufe (watch(false), prepare_deep_sleep) laufen ohne Sensor
 * folgenlos ins Leere.
 */
#if CONFIG_VOICE_BOARD_WATCH206
/*
 * Auf der Waveshare-Uhr ist sie an (10/2026, ausdruecklich gewuenscht): Eine
 * Uhr, die man zum Ablesen erst antippen muss, ist keine. Der Sensor ist dort
 * ein QMI8658; die Komponente waehlt ihn selbst. Seit Paket H weckt er die
 * Task per Interrupt (Wake on Motion, GPIO21) statt alle 100 ms abgefragt zu
 * werden -- siehe wrist_wake.c.
 */
#define WRIST_WAKE_ENABLED 1
#else
#define WRIST_WAKE_ENABLED 0
#endif
/*
 * Dieselbe Grenze, die der Server zieht (MAX_AUDIO_SECONDS in
 * voiceTurnService.ts). Sie hier zu kennen heisst: der Balken kann sie
 * anzeigen, und eine zu lange Aufnahme wird abgeschickt statt verworfen.
 */
#define RECORDING_MAX_MS (30 * 1000)
#define RECORDING_TICK_MS 200
#define RECORDING_TICK_US (RECORDING_TICK_MS * 1000ULL)
/*
 * Wie lange die Sprechtaste mindestens gehalten werden muss, damit die
 * Aufnahme zaehlt. Derselbe Wert wie MIN_TURN_MILLIS in der Bruecke
 * (VoiceBridgeService.kt): Kuerzeres wirft die ohnehin weg, dann soll das
 * Geraet es gleich selbst tun — und es sagen, statt stumm nichts zu liefern.
 *
 * Der Anlass: Ein Antippen startete die Aufnahme wie ein Halten, und wer
 * danach zu sprechen anfing, sprach ins Leere. "Halten zum Sprechen" steht
 * seit jeher auf dem Schirm; jetzt stimmt es auch.
 */
#define PRIMARY_MIN_HOLD_MS 500
/* Wie lange der Abbruch-Ton auf den Abbau des Aufnahmepfads wartet. */
#define CANCEL_TONE_WAIT_MS 600

static bool s_recording;
/*
 * Die Uhr ueberlebt den Tiefschlaf — halb von selbst, halb von Hand.
 *
 * Die Systemuhr laeuft im Tiefschlaf weiter, der RTC-Zeitgeber haelt sie. Der
 * Abstand zur Ortszeit lag dagegen im normalen RAM und waere weg; im
 * RTC-Speicher ueberlebt er. Ohne ihn zeigte das Geraet nach jedem Aufwachen
 * UTC — und das faellt in Deutschland erst im Winter auf, wenn es stimmt.
 */
static RTC_DATA_ATTR int32_t s_clock_offset_min;
static RTC_DATA_ATTR bool s_clock_valid;
static bool s_ota_updating;
static bool s_display_dimmed;
#if CONFIG_VOICE_BOARD_WATCH206
/* Nur die Uhr: der Controller schlaeft (impliziert s_display_dimmed). */
static bool s_display_off;
#endif
static esp_timer_handle_t s_display_off_timer;
static bool s_recording_pm_locked;
static bool s_ota_pm_locked;
static bool s_battery_charging;
static bool s_usb_powered;
/* Was apply_light_sleep() zuletzt eingestellt hat (Flag 16 im Akku-Protokoll). */
static bool s_light_sleep_on;
/*
 * Der naechste Termin, wie ihn die Bruecke mit dem Ziffernblatt schickt
 * (UTC-Sekunden, 0 = keiner) -- und ob fuer ihn schon vibriert wurde. Zehn
 * Minuten vorher meldet sich die Uhr am Handgelenk, einmal je Termin.
 */
/*
 * Was im Kontrollzentrum der Uhr steht. Im RTC-Speicher, damit es den
 * Tiefschlaf ueberlebt; nach einem Neustart gilt wieder die Voreinstellung.
 *
 *   brightness  0 = Hell, 1 = Mittel (Voreinstellung), 2 = Dunkel
 *   silent      keine Toene -- die Vibration bleibt, sie ist leise genug
 *   cinema      Handgelenk weckt nicht, Antworten wecken den Schirm nicht
 *   saver       Energiesparen: dunkler, frueher gedimmt, keine Geste.
 *               Schaltet sich unter SAVER_AUTO_PERCENT von selbst ein.
 *               An der Taktfrequenz aendert es nichts -- die regelt esp_pm
 *               ohnehin nach Bedarf (siehe init_power_management).
 */
static RTC_DATA_ATTR uint8_t s_brightness_level = 1;
static RTC_DATA_ATTR bool s_silent;
static RTC_DATA_ATTR bool s_cinema;
static RTC_DATA_ATTR bool s_saver;
/* Einmal je Ladezyklus automatisch -- wer es wieder ausschaltet, bleibt dabei. */
static RTC_DATA_ATTR bool s_saver_auto_done;
static int s_battery_percent = -1;
#define SAVER_AUTO_PERCENT 15
#if CONFIG_VOICE_BOARD_WATCH206
static const uint8_t BRIGHTNESS_STEPS[3] = { 255, 128, 48 };
static const char *const BRIGHTNESS_NAMES[3] = { "Hell", "Mittel", "Dunkel" };
/*
 * Einstellungen der Uhr aus der App (`watch_settings`, docs/protocol.md).
 * Im RTC-Speicher wie das Kontrollzentrum: Die Bruecke schickt sie bei jeder
 * Verbindung, aber nach dem Tiefschlaf soll die Uhr nicht erst bis dahin in
 * der Voreinstellung stehen.
 *
 *   font_level    0 klein, 1 normal (Voreinstellung), 2 gross
 *   home_after_s  zurueck aufs Ziffernblatt nach so viel Ruhe; 0 = nie
 *   tiles         sichtbare Kacheln des Kontrollzentrums in Reihenfolge;
 *                 tile_count 0 = Voreinstellung (ui_control.c)
 */
static RTC_DATA_ATTR uint8_t s_font_level = 1;
static RTC_DATA_ATTR uint16_t s_home_after_s = 30;
static RTC_DATA_ATTR uint8_t s_tiles[UI_CONTROL_COUNT];
static RTC_DATA_ATTR uint8_t s_tile_count;
#define HOME_AFTER_MIN_S 10
#define HOME_AFTER_MAX_S 600
/*
 * Eine neue Antwort, die noch niemand angesehen hat -- angesehen heisst:
 * seit ihrer Ankunft wurde getippt, gewischt oder eine Taste gedrueckt --
 * bleibt mindestens zwei Minuten stehen, auch wenn die Rueckkehr frueher
 * faellig waere. Wer die Uhr erst nach einer Minute ansieht, soll die
 * Antwort noch finden und nicht das Ziffernblatt.
 */
#define UNREAD_ANSWER_HOLD_US (120 * 1000 * 1000LL)
static esp_timer_handle_t s_home_timer;
static bool s_answer_unread;
static int64_t s_answer_at_us;
/* Ab hier zaehlt ein Wischen nach unten als "vom oberen Rand" (absolute y). */
#define TOP_EDGE_PX 80
/* Bis hierher zaehlt ein Wischen nach rechts als "vom linken Rand": zurueck (absolute x). */
#define BACK_EDGE_PX 30
/*
 * Akku-Restzeit (Paket G, 16): Stichproben alle zehn Minuten im RTC-Speicher,
 * damit sie den Tiefschlaf ueberleben; nur ohne Kabel, am Kabel wird der
 * Ring geleert. Die Rechnung steht in battery_estimate.h.
 */
static RTC_DATA_ATTR battery_ring_t s_battery_ring;
static char s_battery_remaining[24];
#define BATTERY_FACE_HINT_PERCENT 20
/*
 * Akku-Protokoll (Paket H, 1): alle zehn Minuten Zeit, Prozent und Zustand
 * in einen Ring von 288 Proben im RTC-Speicher (48 h, ueberlebt den
 * Tiefschlaf). Stuendlich gehen die letzten sechs in den NVS und kommen nach
 * einem Kaltstart zurueck. Die Bruecke holt den Ring mit
 * `battery_log_request` ab (docs/protocol.md) und laedt ihn zum Server, der
 * daraus die Entladerate je Firmware-Fassung rechnet -- zuerst messen, dann
 * sparen. Die Logik steht in battery_log.h.
 */
static RTC_DATA_ATTR battery_log_t s_battery_log;
static RTC_DATA_ATTR uint32_t s_battery_log_saved_t;
/* Seit der letzten Probe wurde aufgenommen (Flag 32). */
static bool s_recorded_since_sample;
#define BATTERY_LOG_NVS_NAMESPACE "akku"
#define BATTERY_LOG_NVS_KEY "battery_log"
#define BATTERY_LOG_NVS_INTERVAL_S (60 * 60)
/* Schreibtakt zwischen den Teilen der Antwort, damit die Bruecke nachkommt. */
#define BATTERY_LOG_PART_GAP_MS 30
/* Vor diesem Zeitpunkt ist die Uhr nicht gestellt; eine Probe ohne Zeit nuetzt niemandem. */
#define BATTERY_LOG_MIN_EPOCH 1700000000u
/*
 * Nachtlade-Modus (Paket G, 17): am USB-Kabel zwischen 22 und 6 Uhr Ortszeit
 * zeigt das Zifferblatt nur die dunkelgraue Uhrzeit bei Helligkeit 20,
 * nichts vibriert, kein Ton. Jede Beruehrung zeigt zehn Sekunden das normale
 * Zifferblatt (s_night_peek). Ende bei Kabel ab oder um 6 Uhr -- geprueft im
 * Akku-Takt, alle zehn Sekunden.
 */
static bool s_night_mode;
static bool s_night_peek;
static esp_timer_handle_t s_night_peek_timer;
#define NIGHT_BRIGHTNESS 20
#define NIGHT_PEEK_US (10 * 1000 * 1000ULL)
#define NIGHT_FROM_HOUR 22
#define NIGHT_TO_HOUR 6
/*
 * Nachtmodus (Paket I, 0.4.4): im Fenster `watch_settings.night` (Ortszeit,
 * darf ueber Mitternacht gehen; Voreinstellung 21:00-06:30) zeigt das
 * Zifferblatt alles in gedaempftem Rot, und die Helligkeit ist hoechstens
 * "Dunkel" -- in jeder Szene. Stellt der Nutzer im Kontrollzentrum hoeher,
 * gilt das bis zum naechsten Schirm-aus (s_night_bright_override). Geprueft
 * im Akku-Takt (zehn Sekunden) und wenn die Uhr gestellt wird. Nicht zu
 * verwechseln mit dem Nachtlade-Modus oben: der gilt nur am Kabel und hat
 * Vorrang; im Fenster ist er ebenfalls rot.
 *
 * Im RTC-Speicher wie die anderen `watch_settings`. `s_night_from_min ==
 * s_night_to_min` heisst aus (`night: ""`).
 */
static RTC_DATA_ATTR uint16_t s_night_from_min = 21 * 60;
static RTC_DATA_ATTR uint16_t s_night_to_min = 6 * 60 + 30;
static bool s_night_window;
static bool s_night_bright_override;
/* Zifferblatt-Layout (`watch_settings.face` oder langer Druck), ui_face_layout_t. */
static RTC_DATA_ATTR uint8_t s_face_layout = UI_FACE_INFO;
static void update_night_window(void);
static void apply_brightness_now(void);
#endif
/* Langer Druck auf die Blaettertaste hat die Aktion ausgeloest -- das Loslassen blaettert nicht. */
static bool s_action_fired;

static volatile int64_t s_next_event_start;
static volatile bool s_event_reminded;
#define EVENT_REMINDER_LEAD_S (10 * 60)
/* Erst nach dem Start darf der Akku-Takt die Schlafart umstellen. */
static bool s_pm_ready;
static esp_pm_lock_handle_t s_cpu_freq_lock;
#if CONFIG_VOICE_BOARD_WATCH206
/* 80 MHz, solange der Schirm hell ist (ESP_PM_APB_FREQ_MAX); siehe init_power_management. */
static esp_pm_lock_handle_t s_display_apb_lock;
static bool s_display_apb_locked;
#endif
static esp_timer_handle_t s_display_dim_timer;
static esp_timer_handle_t s_deep_sleep_timer;
static esp_timer_handle_t s_battery_refresh_timer;
static esp_timer_handle_t s_host_response_timer;
static esp_timer_handle_t s_recording_tick_timer;
/*
 * Bluetooth gestaffelt (Paket H, Punkt 4): Waehrend die Bruecke Zifferblatt,
 * Listen oder Wetter schickt, laeuft die Verbindung schnell; drei Sekunden
 * nach dem letzten dieser Schreibvorgaenge faellt sie auf langsam zurueck
 * (400-500 ms, Latency 4, voice_ble.c). Aufnahme und OTA halten sie selbst
 * schnell. `ready`/`no_speech`/`error` schalten zusaetzlich sofort langsam.
 */
static esp_timer_handle_t s_ble_settle_timer;
#define BLE_TRANSFER_SETTLE_US (3 * 1000 * 1000ULL)
static int64_t s_recording_started_us;
/* Wieviele Aufnahme-Takte hintereinander die Taste los gemeldet haben. */
static uint8_t s_release_ticks;
/* Wann der Pin das Loslassen zuerst meldete, falls das Ereignis ausblieb. */
static int64_t s_primary_release_us;
/*
 * Der Name aus Todoteck, sobald die Bruecke ihn geschickt hat. Er ueberlebt
 * einen Verbindungsabbruch — sonst stuende beim naechsten Blick wieder
 * VS-5290 in der Kopfzeile, und bei zwei Sticks wuesste man nicht, welcher
 * in der Hand liegt.
 */
static char s_display_name[24];
static uint32_t s_session_id = 1;
static QueueHandle_t s_app_event_queue;
static button_handle_t s_talk_button;
static button_handle_t s_browse_button;
static int64_t s_primary_down_us;
static int64_t s_secondary_down_us;
static uint32_t s_primary_session_id;

typedef enum {
    APP_UI_STATE_READY,
    APP_UI_STATE_RECORDING,
    APP_UI_STATE_THINKING,
    APP_UI_STATE_PENDING_CONFIRMATION,
    APP_UI_STATE_ERROR,
    /*
     * Der Turn lief durch, es wurde nur nichts gesagt: Die Aufnahme war
     * still, oder die Erkennung hat aus der Stille einen Satz erfunden. Der
     * Server faengt beides ab und schickt diesen Zustand statt einer
     * Antwort.
     *
     * Ein eigener Zustand und kein Fehler, weil beides verschieden ist: Ein
     * Fehler heisst "etwas ist kaputt", das hier heisst "die Taste hat in
     * der Tasche ausgeloest". Bruecken, die ihn nicht kennen, schicken
     * weiterhin "error" -- das Geraet zeigt dann eben den Fehler-Zustand.
     */
    APP_UI_STATE_NO_SPEECH,
} app_ui_state_t;

static app_ui_state_t s_app_ui_state = APP_UI_STATE_READY;

/*
 * Stufen des Nachdenkens (Paket I, 0.4.4): `stage` an `ui_state:thinking`.
 * "Verstehe ..." solange die Bruecke transkribiert, "Erledige ..." sobald sie
 * ausfuehrt; ohne `stage` (aeltere Bruecke) wie bisher "Denkt nach".
 */
typedef enum {
    THINK_STAGE_NONE,
    THINK_STAGE_UNDERSTAND,
    THINK_STAGE_ACT,
} think_stage_t;
/*
 * Seit wann in diesem Turn gedacht wird (esp_timer, 0 = gerade nicht) --
 * gezaehlt ab dem ersten `thinking`, nicht je Stufe. Ab fuenf Sekunden
 * zeigt die Zeile einen Sekundenzaehler (ui_status_set_thinking).
 */
static int64_t s_thinking_since_us;

typedef enum {
    INTERACTION_MODE_HOLD_TO_TALK,
    INTERACTION_MODE_CLICK_TO_TALK,
} interaction_mode_t;

static interaction_mode_t s_interaction_mode = INTERACTION_MODE_HOLD_TO_TALK;

static const char *app_ui_state_name(app_ui_state_t state)
{
    switch (state) {
    case APP_UI_STATE_READY:
        return "ready";
    case APP_UI_STATE_RECORDING:
        return "recording";
    case APP_UI_STATE_THINKING:
        return "thinking";
    case APP_UI_STATE_PENDING_CONFIRMATION:
        return "pending_confirmation";
    case APP_UI_STATE_ERROR:
        return "error";
    case APP_UI_STATE_NO_SPEECH:
        return "no_speech";
    }
    return "unknown";
}

/*
 * Die Tasten heissen nach ihrer Rolle, nicht nach ihrer Lage: `TALK` ist die
 * Sprechtaste, `BROWSE` die zum Blaettern. Welcher Pin daran haengt, steht in
 * stick_s3_board.h (StickS3, hat 08/2026 gewechselt) beziehungsweise in
 * voice_board.h (Waveshare-Uhr).
 */
typedef enum {
    APP_EVENT_TALK_DOWN,
    APP_EVENT_TALK_UP,
    APP_EVENT_BROWSE_DOWN,
    APP_EVENT_BROWSE_UP,
    APP_EVENT_UI_STATE,
    APP_EVENT_BLE_CONNECTED,
    APP_EVENT_BLE_DISCONNECTED,
    APP_EVENT_POWER_IRQ,
    APP_EVENT_BATTERY_REFRESH,
    APP_EVENT_ENTER_DEEP_SLEEP,
    APP_EVENT_OTA_BEGIN,
    APP_EVENT_OTA_PROGRESS,
    APP_EVENT_OTA_DONE,
    APP_EVENT_OTA_END,
    APP_EVENT_HOST_RESPONSE_TIMEOUT,
    APP_EVENT_RECORDING_TICK,
    APP_EVENT_DEVICE_NAME,
    APP_EVENT_TIME,
    APP_EVENT_WRIST_RAISE,
    /* Tippen oder Wischen auf dem Schirm der Uhr. */
    APP_EVENT_TOUCH,
    /* Langer Druck auf die Blaettertaste der Uhr: die frei belegte Aktion. */
    APP_EVENT_ACTION,
    /* Transkript zuerst (nur die Uhr): Text in `text`, Frist in ms in `size`. */
    APP_EVENT_TRANSCRIPT,
    /* Rueckkehr-Zeitgeber der Uhr abgelaufen. */
    APP_EVENT_HOME_TIMEOUT,
    /* Die zehn Sekunden normales Zifferblatt im Nachtlade-Modus sind vorbei. */
    APP_EVENT_NIGHT_PEEK_END,
    /* Die Bruecke will das Akku-Protokoll ab `epoch` (Uhr). */
    APP_EVENT_BATTERY_LOG_REQUEST,
    /* Verlauf in den NVS schreiben (ui_status meldet, der Ereignis-Task schreibt). */
    APP_EVENT_HISTORY_SAVE,
    /* 15 Sekunden gedimmt vorbei: Schirm aus (Uhr). */
    APP_EVENT_DISPLAY_OFF,
    /* Drei Sekunden ohne Uebertragung der Bruecke: Verbindung wieder langsam. */
    APP_EVENT_BLE_SETTLE,
    /* `options` mit leerer Liste (Karte inzwischen neu): Auswahl zu, Fehlerton (Uhr, 0.4.5). */
    APP_EVENT_OPTIONS_FAILED,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    uint32_t written;
    uint32_t size;
    /* Nur fuer APP_EVENT_TIME: UTC-Sekunden und Abstand der Ortszeit.
       Bei APP_EVENT_BATTERY_LOG_REQUEST das `since` der Bruecke. */
    int64_t epoch;
    int32_t tz_offset_min;
    char state[32];
    /*
     * 192 statt 96 Byte: die Funkstrecke traegt 244 je Schreibvorgang
     * (MTU 247 abzueglich ATT-Kopf), die Grenze lag also frueher hier und
     * nicht am Uebertragungsweg. In UTF-8 kostet ein Umlaut zwei Byte — mit
     * 96 waeren deutsche Antworten regelmaessig mitten im Wort geendet.
     */
    char text[192];
    /*
     * Nur bei `ready`: das, was die Bruecke verstanden hat -- die Frage zur
     * Antwort daneben. Sie steht im selben Steuerbefehl, damit Frage und
     * Antwort nicht in zwei Schreibvorgaengen auseinanderlaufen koennen.
     *
     * 64 Byte, nicht 192: Im Verlauf ist sie die Ueberschrift, nicht der
     * Inhalt -- und jedes Byte hier kostet zwoelffach, so viele Plaetze hat
     * die Warteschlange.
     */
    char question[64];
    /*
     * Nur bei `ready`: Antwortmoeglichkeiten auf eine Rueckfrage ("Ja",
     * "Nein"). Die Uhr zeigt sie als Touch-Flaechen; der StickS3 ignoriert
     * sie und bleibt beim gesprochenen "ja".
     */
    char choices[UI_STATUS_CHOICES_MAX][UI_STATUS_CHOICE_LEN];
    uint8_t choice_count;
    /*
     * Nur bei `ready`: das Ergebnis des Turns fuer den Marker im Verlauf
     * (`outcome`) und die Kennung, unter der die Bruecke ihn 15 Minuten lang
     * zuruecknimmt (`ref`, 0 = nicht ruecknehmbar). docs/protocol.md.
     */
    uint8_t outcome;
    uint16_t ref;
    /* Nur bei `thinking`: think_stage_t aus `stage` (0.4.4). */
    uint8_t stage;
    /* Nur fuer APP_EVENT_TOUCH. */
    uint8_t gesture;
    int16_t touch_x;
    int16_t touch_y;
} app_event_t;

static void update_battery_status(void);
static void queue_app_event(app_event_type_t type);
static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size);
/*
 * Kopiert hoechstens `size - 1` Byte und schneidet dabei **nie** mitten in
 * ein UTF-8-Zeichen. `strlcpy` wuerde genau das tun: ein halbierter Umlaut
 * ergibt auf dem Display ein Kaestchen, und LVGL laeuft beim Dekodieren ins
 * Leere.
 */
static void copy_utf8_clipped(char *dst, const char *src, size_t size)
{
    if (size == 0) {
        return;
    }
    size_t len = strlen(src);
    if (len >= size) {
        len = size - 1;
        /* Fortsetzungsbytes tragen 10xxxxxx — bis zum Anfang zuruecklaufen. */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void queue_ui_state_event(const char *state, const char *text,
                                 const char *question, const cJSON *choices,
                                 ui_outcome_t outcome, uint16_t ref, uint8_t stage);
static void queue_device_name_event(const char *name);
static void queue_time_event(int64_t epoch, int32_t tz_offset_min);
static void apply_interaction_mode(interaction_mode_t mode);
static void apply_app_ui_state_choices(const char *state, const char *text, const char *question,
                                       const char (*choices)[UI_STATUS_CHOICE_LEN],
                                       uint8_t choice_count, ui_outcome_t outcome, uint16_t ref,
                                       uint8_t stage);

static bool is_external_powered(void)
{
    return s_battery_charging || s_usb_powered;
}

/*
 * Jede Vibration der Uhr geht hier durch: im Nachtlade-Modus bleibt der
 * Motor still (auf dem StickS3 gibt es ohnehin keinen).
 */
static void haptic_pattern(uint8_t count, uint32_t on_ms, uint32_t off_ms)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (s_night_mode) {
        return;
    }
#endif
    voice_board_haptic_pattern(count, on_ms, off_ms);
}

/*
 * Light Sleep im Leerlauf -- ausser bei der Uhr am USB-Kabel.
 *
 * Die Waveshare-Uhr spricht USB ueber den eingebauten USB-Serial/JTAG-Block
 * des S3, und der schlaeft im Light Sleep mit ein. Windows meldet dann "Ein an
 * das System angeschlossenes Geraet funktioniert nicht", esptool kommt nicht
 * an den Port, und weil die Uhr am Akku haengt, startet sie auch beim
 * Umstecken nicht neu. Am Kabel kostet Wachbleiben nichts, was zaehlt.
 * Der StickS3 bleibt beim bisherigen Verhalten: Er wird ueber Bluetooth
 * aktualisiert und hat das Problem in der Praxis nicht gezeigt.
 */
static void apply_light_sleep(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    const bool light_sleep = !s_usb_powered;
#else
    const bool light_sleep = true;
#endif
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = light_sleep,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(err));
    } else {
        s_light_sleep_on = light_sleep;
    }
    ESP_LOGI(TAG, "light sleep %s", light_sleep ? "an" : "aus (USB)");
}

/*
 * Taktfrequenz (Paket H, Punkt 5) -- drei Stufen, alle ueber esp_pm:
 *
 *   XTAL (40 MHz)  ohne Sperre, also im Leerlauf; dazu Light Sleep (apply_light_sleep).
 *   80 MHz         solange der Schirm hell ist (Uhr): s_display_apb_lock,
 *                  eine ESP_PM_APB_FREQ_MAX-Sperre. Das ist die Stufe, auf
 *                  der LVGL zeichnet und der Finger scrollt.
 *   Maximum        nur mit s_cpu_freq_lock (ESP_PM_CPU_FREQ_MAX) beim
 *                  Aufnehmen (Opus) und bei OTA. Das sind 160 MHz, auf beiden
 *                  Geraeten (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ). Die Uhr lief in
 *                  0.4.1 mit 240 und startete damit nicht mehr -- siehe
 *                  sdkconfig.defaults.watch206; seit 0.4.2 wieder 160.
 *
 * Warum nicht "beim Touch kurz 160": esp_pm kennt keine Zwischenstufe. Eine
 * Sperre hebt entweder auf APB-Maximum (80) oder auf das konfigurierte
 * Maximum; eine dritte Stufe waere nur zu haben, indem das Maximum selbst
 * sie wird -- dann aber auch fuers Aufnehmen. Sollte Scrollen bei 80 MHz
 * ruckeln, ist das Maximum in sdkconfig.defaults.watch206 die Stellschraube,
 * nicht eine weitere Sperre.
 */
static esp_err_t init_power_management(void)
{
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "recording_cpu", &s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_VOICE_BOARD_WATCH206
    err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_apb", &s_display_apb_lock);
    if (err != ESP_OK) {
        return err;
    }
#endif
    return ESP_OK;
}

/* Schirm hell: 80 MHz halten; gedimmt oder aus: loslassen. Nur die Uhr. */
static void set_display_apb_lock(bool on)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (!s_display_apb_lock || s_display_apb_locked == on) {
        return;
    }
    const esp_err_t err = on ? esp_pm_lock_acquire(s_display_apb_lock)
                             : esp_pm_lock_release(s_display_apb_lock);
    if (err == ESP_OK) {
        s_display_apb_locked = on;
    }
#else
    (void)on;
#endif
}

static esp_err_t acquire_recording_pm_locks(void)
{
    if (s_recording_pm_locked) {
        return ESP_OK;
    }

    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_recording_pm_locked = true;
    return ESP_OK;
}

static void release_recording_pm_locks(void)
{
    if (!s_recording_pm_locked) {
        return;
    }

    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_recording_pm_locked = false;
}

static esp_err_t acquire_ota_pm_locks(void)
{
    if (s_ota_pm_locked) {
        return ESP_OK;
    }

    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_ota_pm_locked = true;
    return ESP_OK;
}

static void release_ota_pm_locks(void)
{
    if (!s_ota_pm_locked) {
        return;
    }

    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_ota_pm_locked = false;
}

static void restart_display_dim_timer(void)
{
    if (!s_display_dim_timer) {
        return;
    }

    (void)esp_timer_stop(s_display_dim_timer);
    if (s_display_off_timer) {
        (void)esp_timer_stop(s_display_off_timer);
    }
    if (!s_recording && !s_ota_updating) {
        /* Beim Sparen nach sechs statt fuenfzehn Sekunden. */
        esp_err_t err = esp_timer_start_once(s_display_dim_timer,
                                             s_saver ? 6 * 1000 * 1000ULL : DISPLAY_DIM_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start dim timer failed: %s", esp_err_to_name(err));
        }
    }
}

static void restart_deep_sleep_timer(void)
{
    if (!s_deep_sleep_timer) {
        return;
    }

    (void)esp_timer_stop(s_deep_sleep_timer);
    if (!s_recording && !s_ota_updating && !is_external_powered()) {
        esp_err_t err = esp_timer_start_once(s_deep_sleep_timer, DEEP_SLEEP_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start deep sleep timer failed: %s", esp_err_to_name(err));
        }
    } else if (is_external_powered()) {
        ESP_LOGD(TAG, "deep sleep timer paused while external power is present");
    }
}

/* Helligkeit im wachen Zustand: gewaehlte Stufe, beim Sparen hoechstens "Dunkel". */
static uint8_t active_brightness(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    uint8_t level = BRIGHTNESS_STEPS[s_brightness_level % 3];
    if (s_saver && level > BRIGHTNESS_STEPS[2]) {
        level = BRIGHTNESS_STEPS[2];
    }
    /* Im Nachtfenster hoechstens "Dunkel" -- es sei denn, der Nutzer hat eben hochgestellt. */
    if (s_night_window && !s_night_bright_override && level > BRIGHTNESS_STEPS[2]) {
        level = BRIGHTNESS_STEPS[2];
    }
    return level;
#else
    return DISPLAY_ACTIVE_BRIGHTNESS;
#endif
}

#if WRIST_WAKE_ENABLED
static bool wrist_wake_allowed(void)
{
    return !s_cinema && !s_saver;
}
#endif

#if CONFIG_VOICE_BOARD_WATCH206
/* Rueckkehr zum Ziffernblatt: bei jeder Bedienung von vorn, 0 = nie. */
static void restart_home_timer(void)
{
    if (!s_home_timer) {
        return;
    }
    (void)esp_timer_stop(s_home_timer);
    if (s_home_after_s == 0) {
        return;
    }
    esp_err_t err = esp_timer_start_once(s_home_timer, (uint64_t)s_home_after_s * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start home timer failed: %s", esp_err_to_name(err));
    }
}
#endif

/* Schirm aus oder an -- nur die Uhr, der StickS3 kennt nur das Dimmen. */
static void set_display_off(bool off)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (s_display_off == off) {
        return;
    }
    s_display_off = off;
    /* Nachts hochgestellte Helligkeit gilt bis zum naechsten Schirm-aus. */
    if (off) {
        s_night_bright_override = false;
    }
    ui_status_set_display_off(off);
    ESP_LOGI(TAG, "Schirm %s", off ? "aus" : "an");
#else
    (void)off;
#endif
}

static void note_activity(void)
{
    if (s_display_dimmed) {
        set_display_apb_lock(true);
        set_display_off(false);
        esp_err_t err = ui_status_set_brightness(active_brightness());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore brightness failed: %s", esp_err_to_name(err));
        } else {
            s_display_dimmed = false;
            ui_status_set_idle_dimmed(false);
            /* Heller Schirm: nichts aufzuwecken, also auch nicht messen. */
            wrist_wake_watch(false);
        }
    }
    restart_display_dim_timer();
    restart_deep_sleep_timer();
#if CONFIG_VOICE_BOARD_WATCH206
    restart_home_timer();
#endif
}

/* Eine Bedienung durch den Menschen: wie note_activity(), und die Antwort gilt als gesehen. */
static void note_user_input(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    s_answer_unread = false;
#endif
    note_activity();
    /* Unverbunden: Wer das Geraet anfasst, will es auch finden -- 30 s schnelle Werbung. */
    voice_ble_advertise_fast();
}

/*
 * Die Bruecke schickt gerade etwas Groesseres (Zifferblatt, Liste, Wetter)
 * oder wird es gleich (Verbindung, glance_request): Verbindung schnell, und
 * den Rueckfall auf langsam von vorn zaehlen.
 */
static void note_ble_transfer(void)
{
    (void)voice_ble_request_fast_interval();
    if (s_ble_settle_timer) {
        (void)esp_timer_stop(s_ble_settle_timer);
        (void)esp_timer_start_once(s_ble_settle_timer, BLE_TRANSFER_SETTLE_US);
    }
}

static void stop_host_response_timer(void)
{
    if (s_host_response_timer) {
        (void)esp_timer_stop(s_host_response_timer);
    }
}

/*
 * Liegt eine der Weck-Tasten gerade gedrueckt? Beim StickS3 heisst das "low"
 * auf beiden Pins; bei der Uhr ist die Blaettertaste aktiv high (siehe
 * voice_board.h). Die Board-Schicht kennt den Pegel, hier wird nur gefragt.
 */
static bool any_wake_button_pressed(void)
{
    return voice_board_talk_button_pressed() || voice_board_browse_button_pressed();
}

static void enter_deep_sleep(void)
{
    if (s_recording || s_ota_updating || voice_ble_ota_is_active()) {
        restart_deep_sleep_timer();
        return;
    }

    if (is_external_powered()) {
        ESP_LOGI(TAG, "skip deep sleep while charging or USB powered");
        restart_deep_sleep_timer();
        return;
    }

    bool charging = false;
    bool usb_powered = false;
    esp_err_t power_err = voice_board_battery_charging(&charging);
    if (power_err == ESP_OK) {
        power_err = voice_board_usb_powered(&usb_powered);
    }
    if (power_err == ESP_OK && (charging || usb_powered)) {
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ESP_LOGI(TAG, "skip deep sleep after fresh power check charging=%d usb=%d",
                 charging, usb_powered);
        restart_deep_sleep_timer();
        return;
    }

    /*
     * Beide Tasten wecken, nicht nur die sprechende.
     *
     * Bis 08/2026 weckte allein die vordere -- damals war sie die
     * Sprechtaste, und "druecken, um zu reden" war zugleich das Aufwachen.
     * Seit die Rollen getauscht sind, waere eine der beiden im Tiefschlaf
     * tot: Wer nach fuenf Minuten den Verlauf durchblaettern will, drueckt
     * eine Taste, die nichts tut, und haelt das Geraet fuer defekt. ext1
     * nimmt ohnehin eine Maske, zwei Pins kosten also nichts.
     */
    const gpio_num_t wake_gpios[] = { VOICE_BOARD_PIN_BUTTON_TALK, VOICE_BOARD_PIN_BUTTON_BROWSE };
    const size_t wake_gpio_count = sizeof(wake_gpios) / sizeof(wake_gpios[0]);

    for (size_t i = 0; i < wake_gpio_count; i++) {
        if (!esp_sleep_is_valid_wakeup_gpio(wake_gpios[i])) {
            ESP_LOGE(TAG, "GPIO%d cannot wake from deep sleep", wake_gpios[i]);
            restart_deep_sleep_timer();
            return;
        }
    }
    if (any_wake_button_pressed()) {
        ESP_LOGI(TAG, "skip deep sleep: a wake button is pressed");
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "entering deep sleep, wake on GPIO%d/GPIO%d",
             wake_gpios[0], wake_gpios[1]);
    release_recording_pm_locks();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_status_set_brightness(0));
    /* Erst hier, nicht oben: Wird der Schlaf doch abgesagt (Aufnahme, OTA,
       Netzteil), soll die Geste weiterlaufen statt bis zum naechsten
       Abdunkeln tot zu sein. */
    wrist_wake_prepare_deep_sleep();
    ui_status_prepare_deep_sleep();
    voice_board_prepare_deep_sleep();

    /* Clear any stale wakeup source bits left over from light sleep / esp_pm
       configuration (e.g. gpio_wakeup_enable on the PMIC IRQ line). Without
       this the chip can wake immediately from an unrelated trigger. */
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* Keep RTC peripherals powered so the internal pull-up on the wake pin
       remains effective in deep sleep; combined with the explicit RTC pull-up
       below this prevents GPIO%d from floating low and self-waking. */
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#if CONFIG_VOICE_BOARD_WATCH206
    /*
     * Die Uhr weckt mit zwei Pegeln: Sprechen (BOOT, GPIO0) zieht nach unten,
     * Blaettern (PWR ueber den BSS138, GPIO10) nach oben. ext1 nimmt auf dem
     * S3 nur einen Pegel fuer alle Pins, also traegt ext0 den zweiten.
     *
     * Interne Widerstaende bleiben aus: Beide Pins haben 10 kOhm auf der
     * Platine. Ein interner Pull-up an GPIO10 zoege im Schlaf dauerhaft gegen
     * den durchgeschalteten Transistor -- Strom fuer nichts.
     */
    for (size_t i = 0; i < wake_gpio_count; i++) {
        (void)rtc_gpio_pulldown_dis(wake_gpios[i]);
        (void)rtc_gpio_pullup_dis(wake_gpios[i]);
    }
    esp_err_t err = esp_sleep_enable_ext0_wakeup(VOICE_BOARD_PIN_BUTTON_BROWSE,
                                                 VOICE_BOARD_BUTTON_BROWSE_ACTIVE_LEVEL);
    if (err == ESP_OK) {
        err = esp_sleep_enable_ext1_wakeup_io(1ULL << VOICE_BOARD_PIN_BUTTON_TALK,
                                              ESP_EXT1_WAKEUP_ANY_LOW);
    }
#else
    const uint64_t wake_mask = (1ULL << VOICE_BOARD_PIN_BUTTON_TALK) |
                               (1ULL << VOICE_BOARD_PIN_BUTTON_BROWSE);
    for (size_t i = 0; i < wake_gpio_count; i++) {
        (void)rtc_gpio_pulldown_dis(wake_gpios[i]);
        (void)rtc_gpio_pullup_en(wake_gpios[i]);
    }

    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        restart_deep_sleep_timer();
        return;
    }

    /* Wait for the wake pin to settle high (button release bounce, parasitic
       capacitance, etc.). If it stays low we would just wake up immediately
       after esp_deep_sleep_start(), so abort and retry later. */
    int wait_ms = 0;
    while (any_wake_button_pressed() && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (any_wake_button_pressed()) {
        ESP_LOGW(TAG, "wake pin still low after %d ms, abort deep sleep", wait_ms);
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "deep sleep go (wait_ms=%d)", wait_ms);
    esp_deep_sleep_start();
}

/*
 * Ein Ton nur, wenn der Aufnahmepfad die I2S-Leitungen freigegeben hat —
 * Mikrofon und Lautsprecher haengen am selben Codec und teilen sich die
 * Leitungen. Lieber stumm als eine zerissene Aufnahme.
 */
/*
 * Zu jedem Ton ein Vibrationsmuster -- nur auf der Uhr, der StickS3 hat
 * keinen Motor. Es laeuft auch dann, wenn der Ton ausfaellt (Aufnahmepfad
 * belegt): Gerade dann ist es die einzige Rueckmeldung.
 *
 *   Start      ein kurzer Stoss       "jetzt sprechen"
 *   Antwort    zwei kurze             "da ist was"
 *   Fehler     ein langer             "hat nicht geklappt"
 *   Verworfen  ein ganz kurzer        "nichts passiert"
 */
static void haptic_for_tone(audio_tone_t tone)
{
    switch (tone) {
    case AUDIO_TONE_START:
        haptic_pattern(1, 60, 0);
        break;
    case AUDIO_TONE_DONE:
        haptic_pattern(2, 50, 90);
        break;
    case AUDIO_TONE_ERROR:
        haptic_pattern(1, 300, 0);
        break;
    case AUDIO_TONE_CANCEL:
        haptic_pattern(1, 30, 0);
        break;
    default:
        break;
    }
}

static void play_tone(audio_tone_t tone)
{
    haptic_for_tone(tone);
    if (s_silent) {
        return;
    }
#if CONFIG_VOICE_BOARD_WATCH206
    if (s_night_mode) {
        return;
    }
#endif
    if (audio_pipeline_is_busy()) {
        ESP_LOGD(TAG, "Ton uebersprungen, Aufnahmepfad belegt");
        return;
    }
    audio_tone_play(tone);
}

static void start_recording_tick(void)
{
    if (!s_recording_tick_timer) {
        return;
    }
    (void)esp_timer_stop(s_recording_tick_timer);
    esp_err_t err = esp_timer_start_periodic(s_recording_tick_timer, RECORDING_TICK_US);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start recording tick failed: %s", esp_err_to_name(err));
    }
}

static void stop_recording_tick(void)
{
    if (s_recording_tick_timer) {
        (void)esp_timer_stop(s_recording_tick_timer);
    }
}

static bool app_ui_allows_recording_start(void)
{
    return s_app_ui_state != APP_UI_STATE_PENDING_CONFIRMATION;
}

static uint32_t start_recording(void)
{
    const bool ble_ready = voice_ble_is_ready();
    const bool ota_active = voice_ble_ota_is_active();
    const bool ui_allows_start = app_ui_allows_recording_start();
    if (s_recording || s_ota_updating || ota_active || !ble_ready || !ui_allows_start) {
        ESP_LOGW(TAG,
                 "start recording denied: recording=%d ota=%d ble_ota=%d ble_ready=%d ui_state=%d",
                 s_recording, s_ota_updating, ota_active, ble_ready, s_app_ui_state);
        return 0;
    }

    const uint32_t session_id = s_session_id++;
    /*
     * Der Ton kommt **vor** dem Start des Aufnahmepfads: beide teilen sich
     * die I2S-Leitungen, und der Piep ist ohnehin das Signal "jetzt
     * sprechen" — er darf die ersten Silben nicht ueberlappen.
     */
    /* Spricht die Uhr noch die letzte Antwort, hoert sie jetzt auf -- der Satz des Nutzers geht vor. */
    audio_tone_speech_stop();
    play_tone(AUDIO_TONE_START);

    /*
     * Sensor still, solange aufgenommen wird.
     *
     * Der BMI270 haengt am selben I2C-Bus wie der ES8311, und der wird beim
     * Start des Aufnahmepfads Register fuer Register konfiguriert. Eine
     * Abfrage mitten hinein ist die eine Stelle, an der sich Geste und
     * Mikrofon ins Gehege kommen koennen — und waehrend jemand die Taste
     * haelt, gibt es ohnehin nichts aufzuwecken.
     */
    wrist_wake_watch(false);
    /* Opus-Rahmen alle 20 ms: dafuer das schnelle Intervall, bis die Antwort da ist. */
    (void)voice_ble_request_fast_interval();

    esp_err_t err = acquire_recording_pm_locks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire recording pm locks failed: %s", esp_err_to_name(err));
        ui_status_set_error("Gerät bleibt nicht wach");
        return 0;
    }

    err = audio_pipeline_start(session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err));
        release_recording_pm_locks();
        /*
         * Der Grund gehoert auf den Schirm, nicht nur ins Log: An diesem
         * Geraet haengt kein Kabel, wenn es nicht geht. "Aufnahme startet
         * nicht" allein sagt nur, dass etwas kaputt ist — der Fehlercode
         * sagt, was: ESP_ERR_NO_MEM ist ein Speicherproblem,
         * ESP_ERR_INVALID_STATE eine haengende Vorgaenger-Sitzung,
         * ESP_ERR_NOT_FOUND ein Codec, der nicht antwortet.
         */
        char meldung[64];
        snprintf(meldung, sizeof(meldung), "Aufnahme startet nicht: %s", esp_err_to_name(err));
        ui_status_set_error(meldung);
        return 0;
    }

    s_recording = true;
#if CONFIG_VOICE_BOARD_WATCH206
    s_recorded_since_sample = true;
#endif
    s_app_ui_state = APP_UI_STATE_RECORDING;
    s_recording_started_us = esp_timer_get_time();
    s_release_ticks = 0;
    s_primary_release_us = 0;
    restart_display_dim_timer();
    restart_deep_sleep_timer();
    ui_status_set_recording(session_id);
    ui_status_set_recording_meter(0, 100);
    start_recording_tick();
    return session_id;
}

static uint32_t stop_recording(void)
{
    if (!s_recording) {
        return 0;
    }

    const uint32_t session_id = audio_pipeline_session_id();
    s_recording = false;
    stop_recording_tick();
    audio_pipeline_stop();
    release_recording_pm_locks();
    restart_display_dim_timer();
    restart_deep_sleep_timer();
    /* Nicht sofort langsam: Transkript und Antwort kommen gleich; der Rueckfall zaehlt ab jetzt. */
    note_ble_transfer();
    return session_id;
}

/*
 * Bricht die laufende Aufnahme ab: nichts geht an Todoteck, das Geraet ist
 * sofort wieder bereit.
 *
 * Das Ereignis geht vor dem Ende-Rahmen raus, den der Abbau des
 * Aufnahmepfads ohnehin schickt — sonst waere der Abbruch fuer die Bruecke
 * nicht von einem normalen Ende zu unterscheiden und die Aufnahme ginge
 * trotzdem an Todoteck.
 */
static void abort_recording(void)
{
    const uint32_t cancelled = stop_recording();
    (void)voice_ble_send_recording_discarded(cancelled);
    s_primary_down_us = 0;
    s_primary_release_us = 0;
    s_primary_session_id = 0;
    s_app_ui_state = APP_UI_STATE_READY;
}

/*
 * Der Abbau des Aufnahmepfads laeuft in eigenen Tasks und gibt die
 * I2S-Leitungen erst nach dem Leeren der Warteschlange frei. Kurz darauf
 * warten, sonst faellt genau der Ton aus, der den Abbruch quittieren soll.
 */
static void play_cancel_tone(void)
{
    for (int waited = 0; waited < CANCEL_TONE_WAIT_MS && audio_pipeline_is_busy();
         waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    play_tone(AUDIO_TONE_CANCEL);
}

static void queue_app_event(app_event_type_t type)
{
    queue_app_event_with_ota(type, 0, 0);
}

static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .written = written,
            .size = size,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

#if !CONFIG_VOICE_BOARD_WATCH206 /* nur die PMIC-Leitung des StickS3 ruft aus einer ISR */
static void queue_app_event_from_isr(app_event_type_t type, BaseType_t *high_task_woken)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .written = 0,
            .size = 0,
        };
        (void)xQueueSendFromISR(s_app_event_queue, &event, high_task_woken);
    }
}
#endif

static void queue_ui_state_event(const char *state, const char *text, const char *question,
                                 const cJSON *choices, ui_outcome_t outcome, uint16_t ref,
                                 uint8_t stage)
{
    if (!s_app_event_queue) {
        ESP_LOGW(TAG, "drop ui_state state=%s text_len=%u: app queue unavailable",
                 state ? state : "nil",
                 (unsigned)(text ? strlen(text) : 0));
        return;
    }

    app_event_t event = {
        .type = APP_EVENT_UI_STATE,
        .outcome = (uint8_t)outcome,
        .ref = ref,
        .stage = stage,
    };
    if (state) {
        strlcpy(event.state, state, sizeof(event.state));
    }
    if (text) {
        copy_utf8_clipped(event.text, text, sizeof(event.text));
    }
    if (question) {
        copy_utf8_clipped(event.question, question, sizeof(event.question));
    }
    if (cJSON_IsArray(choices)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, choices) {
            if (event.choice_count >= UI_STATUS_CHOICES_MAX) {
                break;
            }
            if (cJSON_IsString(item) && item->valuestring[0]) {
                copy_utf8_clipped(event.choices[event.choice_count], item->valuestring,
                                  UI_STATUS_CHOICE_LEN);
                event.choice_count++;
            }
        }
    }
    ESP_LOGI(TAG, "queue ui_state state=%s text_len=%u current=%s recording=%d",
             event.state[0] ? event.state : "nil",
             (unsigned)strlen(event.text),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop ui_state state=%s: app queue full",
                 event.state[0] ? event.state : "nil");
    }
}

/*
 * Der Name kommt aus den Todoteck-Einstellungen und wird ueber den
 * BLE-Host-Task gemeldet. Ueber die Warteschlange statt direkt, damit die
 * Anzeige nur aus einem Strang bedient wird.
 */
static void queue_device_name_event(const char *name)
{
    if (!s_app_event_queue || !name) {
        return;
    }
    app_event_t event = {
        .type = APP_EVENT_DEVICE_NAME,
    };
    copy_utf8_clipped(event.text, name, sizeof(event.text));
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop device_name: app queue full");
    }
}

/*
 * Transkript zuerst: was die Bruecke verstanden hat, bevor sie es ausfuehrt.
 * Ueber die Warteschlange, weil es Zustand des App-Strangs beruehrt (Aufnahme
 * laeuft? Kontrollzentrum offen?). Der StickS3 nimmt es an und tut nichts.
 */
static void queue_transcript_event(const char *text, uint32_t cancel_ms)
{
    if (!s_app_event_queue || !text) {
        return;
    }
    app_event_t event = {
        .type = APP_EVENT_TRANSCRIPT,
        .size = cancel_ms,
    };
    copy_utf8_clipped(event.text, text, sizeof(event.text));
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop transcript: app queue full");
    }
}

#if CONFIG_VOICE_BOARD_WATCH206
/*
 * Aus dem esp_timer-Task (ui_status, 30 s nach einer Verlaufs-Aenderung):
 * nur einreihen. Geschrieben wird im Ereignis-Task, wie das Akku-Protokoll
 * (battery_log_save) -- nicht auf dem 3,5-KB-Stack des Zeitgebers.
 */
static void history_save_cb(void)
{
    queue_app_event(APP_EVENT_HISTORY_SAVE);
}
#endif

#if WRIST_WAKE_ENABLED
/*
 * Das Handgelenk wurde angehoben. Laeuft im Abfrage-Task des Sensors, geht
 * deshalb ueber dieselbe Warteschlange wie die Tasten — die Anzeige wird nur
 * aus einem Strang bedient.
 */
static void wrist_raise_cb(void)
{
    queue_app_event(APP_EVENT_WRIST_RAISE);
}
#endif

/*
 * Die Uhrzeit kommt von der Bruecke — der StickS3 hat weder RTC-Baustein noch
 * WLAN, und eine geratene Uhrzeit auf dem Handgelenk waere schlimmer als
 * keine. (Die Waveshare-Uhr hat einen; die Bruecke stellt ihn mit.) Ueber die Warteschlange wie alles andere, damit die Anzeige aus
 * einem Strang bedient wird.
 */
static void queue_time_event(int64_t epoch, int32_t tz_offset_min)
{
    if (!s_app_event_queue) {
        return;
    }
    app_event_t event = {
        .type = APP_EVENT_TIME,
        .epoch = epoch,
        .tz_offset_min = tz_offset_min,
    };
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop time: app queue full");
    }
}

static void talk_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_TALK_DOWN);
}

static void talk_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_TALK_UP);
}

static void browse_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_BROWSE_DOWN);
}

#if CONFIG_VOICE_BOARD_WATCH206
static void browse_button_long_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_ACTION);
}
#endif

static void browse_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_BROWSE_UP);
}

/*
 * Beruehrung des Schirms (nur die Uhr): ueber die Warteschlange an
 * handle_touch(), wo die Gestensprache steht.
 */
#if CONFIG_VOICE_BOARD_WATCH206
static void screen_touch_cb(const voice_board_touch_event_t *touch)
{
    if (!s_app_event_queue) {
        return;
    }
    app_event_t event = {
        .type = APP_EVENT_TOUCH,
        .gesture = (uint8_t)touch->gesture,
        .touch_x = touch->x,
        .touch_y = touch->y,
    };
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop touch: app queue full");
    }
}
#endif

static void ble_connection_cb(bool connected)
{
    queue_app_event(connected ? APP_EVENT_BLE_CONNECTED : APP_EVENT_BLE_DISCONNECTED);
}

/*
 * `face`: Zeilen fuer das Ziffernblatt und der naechste Termin.
 *   {"event":"face","lines":["…","…"],"event_title":"Zahnarzt","event_start":1793520000}
 * Direkt aus dem Bluetooth-Strang: ui_status nimmt sich die Anzeige-Sperre
 * selbst, und die Daten sind zu gross fuer die Ereignis-Warteschlange.
 */
static void apply_face_command(const cJSON *root)
{
    char lines[UI_STATUS_FACE_LINES][UI_STATUS_FACE_LEN] = {{0}};
    uint8_t count = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "lines");
    const cJSON *item = NULL;
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(item, arr) {
            if (count >= UI_STATUS_FACE_LINES) {
                break;
            }
            if (cJSON_IsString(item) && item->valuestring[0]) {
                copy_utf8_clipped(lines[count++], item->valuestring, UI_STATUS_FACE_LEN);
            }
        }
    }
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "event_title");
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(root, "event_start");
    const int64_t start_s = cJSON_IsNumber(start) ? (int64_t)start->valuedouble : 0;
    if (start_s != s_next_event_start) {
        s_next_event_start = start_s;
        s_event_reminded = false;
    }
    ui_status_set_face((const char (*)[UI_STATUS_FACE_LEN])lines, count,
                       cJSON_IsString(title) ? title->valuestring : "", start_s);
}

/*
 * `list`: eine Karte (Heute, Einkauf, Zuhause), in Teilen, weil eine
 * Schreiboperation nur gut 240 Byte traegt.
 *   {"event":"list","kind":"today","rev":7,"part":0,"items":[["Pool messen","18:00","#2ecc71"]]}
 * Teil 0 leert die Karte; `rev` kommt mit jedem Tippen zurueck, damit die
 * Bruecke einen Index auf einer veralteten Liste nicht falsch zuordnet. Das
 * dritte Element ist die Projektfarbe ("#RRGGBB" oder ""), das vierte die
 * Gruppenueberschrift (nur `today`), das fuenfte die Flags ("c" = Schalter
 * mit Bestaetigung); was fehlt, gilt als "". Bruecken vor dem UI/UX-Paket
 * schicken nur zwei.
 */
static void apply_list_command(const cJSON *root)
{
    const cJSON *kind_item = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const int kind = ui_cards_kind_from_name(cJSON_IsString(kind_item) ? kind_item->valuestring : "");
    if (kind < 0) {
        return;
    }
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(root, "rev");
    const cJSON *part = cJSON_GetObjectItemCaseSensitive(root, "part");
    ui_cards_begin_part((ui_card_kind_t)kind, cJSON_IsNumber(rev) ? (uint16_t)rev->valuedouble : 0,
                        cJSON_IsNumber(part) ? (uint8_t)part->valuedouble : 0);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *entry = NULL;
    if (cJSON_IsArray(items)) {
        cJSON_ArrayForEach(entry, items) {
            const cJSON *text = cJSON_GetArrayItem(entry, 0);
            const cJSON *aux = cJSON_GetArrayItem(entry, 1);
            const cJSON *colour = cJSON_GetArrayItem(entry, 2);
            const cJSON *group = cJSON_GetArrayItem(entry, 3);
            const cJSON *flags = cJSON_GetArrayItem(entry, 4);
            if (cJSON_IsString(text)) {
                ui_cards_add_item((ui_card_kind_t)kind, text->valuestring,
                                  cJSON_IsString(aux) ? aux->valuestring : "",
                                  cJSON_IsString(colour) ? colour->valuestring : "",
                                  cJSON_IsString(group) ? group->valuestring : "",
                                  cJSON_IsString(flags) ? flags->valuestring : "");
            }
        }
    }
    ui_cards_commit((ui_card_kind_t)kind);
}

/*
 * `weather`: die Wetterkarte, in bis zu vier Teilen (docs/protocol.md). `part`
 * ist eine Abschnittskennung, keine laufende Nummer: 0 leert den Stand und
 * traegt die Zusammenfassung (leere Felder = kein Wetter), 1 die Stunden,
 * 2 die Tage, 3 Pollen und Warnungen; leere Teile 1-3 schickt die Bruecke
 * nicht. Jeder Teil zeichnet nach, falls die Karte gerade zu sehen ist.
 *   {"event":"weather","rev":3,"part":0,"now":"10° · Bewölkt","today":"14° / 6° · Regen 60 %",
 *    "rain":"Regen ab 15 Uhr","sun":"07:14 - 16:58","wind":"18 km/h","stale":""}
 *   {"event":"weather","rev":3,"part":1,"hours":[["12h","11°",""],["15h","11°","r"]]}
 *   {"event":"weather","rev":3,"part":2,"days":[["Mo","14° / 6°","Regen 60 %"]]}
 *   {"event":"weather","rev":3,"part":3,"pollen":["Hasel mäßig"],"warnings":["Sturmböen bis 18 Uhr"]}
 */
static const char *json_string(const cJSON *item)
{
    return cJSON_IsString(item) ? item->valuestring : "";
}

static void apply_weather_command(const cJSON *root)
{
    const cJSON *part = cJSON_GetObjectItemCaseSensitive(root, "part");
    const int which = cJSON_IsNumber(part) ? (int)part->valuedouble : 0;
    const cJSON *entry = NULL;
    switch (which) {
    case 0: {
        const cJSON *rev = cJSON_GetObjectItemCaseSensitive(root, "rev");
        ui_weather_begin(cJSON_IsNumber(rev) ? (uint16_t)rev->valuedouble : 0,
                         json_string(cJSON_GetObjectItemCaseSensitive(root, "now")),
                         json_string(cJSON_GetObjectItemCaseSensitive(root, "today")),
                         json_string(cJSON_GetObjectItemCaseSensitive(root, "rain")),
                         json_string(cJSON_GetObjectItemCaseSensitive(root, "sun")),
                         json_string(cJSON_GetObjectItemCaseSensitive(root, "wind")),
                         json_string(cJSON_GetObjectItemCaseSensitive(root, "stale")));
        break;
    }
    case 1: {
        const cJSON *hours = cJSON_GetObjectItemCaseSensitive(root, "hours");
        if (cJSON_IsArray(hours)) {
            cJSON_ArrayForEach(entry, hours) {
                const char *flag = json_string(cJSON_GetArrayItem(entry, 2));
                ui_weather_add_hour(json_string(cJSON_GetArrayItem(entry, 0)),
                                    json_string(cJSON_GetArrayItem(entry, 1)),
                                    strchr(flag, 'r') != NULL);
            }
        }
        break;
    }
    case 2: {
        const cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
        if (cJSON_IsArray(days)) {
            cJSON_ArrayForEach(entry, days) {
                ui_weather_add_day(json_string(cJSON_GetArrayItem(entry, 0)),
                                   json_string(cJSON_GetArrayItem(entry, 1)),
                                   json_string(cJSON_GetArrayItem(entry, 2)));
            }
        }
        break;
    }
    case 3: {
        const cJSON *pollen = cJSON_GetObjectItemCaseSensitive(root, "pollen");
        const cJSON *warnings = cJSON_GetObjectItemCaseSensitive(root, "warnings");
        if (cJSON_IsArray(pollen)) {
            cJSON_ArrayForEach(entry, pollen) {
                ui_weather_add_pollen(json_string(entry));
            }
        }
        if (cJSON_IsArray(warnings)) {
            cJSON_ArrayForEach(entry, warnings) {
                ui_weather_add_warning(json_string(entry));
            }
        }
        break;
    }
    default:
        return;
    }
    ui_weather_commit();
}

#if CONFIG_VOICE_BOARD_WATCH206
/* Einstellungen aus dem RTC-Speicher an die Anzeige -- beim Start und nach jedem `watch_settings`. */
static void apply_watch_settings_ui(void)
{
    ui_status_set_font_level(s_font_level);
    ui_status_set_face_layout((ui_face_layout_t)s_face_layout);
    ui_cards_refresh();
    if (s_tile_count > 0) {
        ui_control_set_tiles(s_tiles, s_tile_count);
    }
}
#endif

#if CONFIG_VOICE_BOARD_WATCH206
/* "HH:MM" -> Minute des Tages; -1, wenn es keine gueltige Uhrzeit ist. */
static int parse_hhmm(const char *text)
{
    if (strlen(text) != 5 || text[2] != ':') {
        return -1;
    }
    for (int i = 0; i < 5; i++) {
        if (i != 2 && (text[i] < '0' || text[i] > '9')) {
            return -1;
        }
    }
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    return hour < 24 && minute < 60 ? hour * 60 + minute : -1;
}

/*
 * `night`: "" = aus, "HH:MM-HH:MM" = Fenster (Ortszeit, darf ueber
 * Mitternacht gehen). `true`, wenn der Wert gueltig war und uebernommen ist.
 */
static bool parse_night_window(const char *text)
{
    if (text[0] == '\0') {
        s_night_from_min = 0;
        s_night_to_min = 0;
        return true;
    }
    if (strlen(text) != 11 || text[5] != '-') {
        return false;
    }
    char part[6];
    memcpy(part, text, 5);
    part[5] = '\0';
    const int from = parse_hhmm(part);
    memcpy(part, text + 6, 5);
    const int to = parse_hhmm(part);
    if (from < 0 || to < 0) {
        return false;
    }
    s_night_from_min = (uint16_t)from;
    s_night_to_min = (uint16_t)to;
    return true;
}
#endif

/*
 * `watch_settings`: Einstellungen der Uhr aus der App.
 *   {"event":"watch_settings","font":"m","home_after_s":30,"tiles":["silent","brightness"],
 *    "face":"info","night":"21:00-06:30"}
 * Die Bruecke schickt sie bei jeder Verbindung und nach jedem Abgleich; sie
 * sind klein und idempotent. Was fehlt oder nicht passt, bleibt, wie es war;
 * unbekannte Kachelnamen fallen still weg. Den Rueckkehr-Zeitgeber startet
 * das nicht neu -- sonst schoebe jeder Abgleich die Rueckkehr hinaus. Der
 * neue Wert gilt ab der naechsten Bedienung; "nie" (0) gilt sofort.
 */
static void apply_watch_settings_command(const cJSON *root)
{
#if CONFIG_VOICE_BOARD_WATCH206
    const cJSON *font = cJSON_GetObjectItemCaseSensitive(root, "font");
    if (cJSON_IsString(font)) {
        if (strcmp(font->valuestring, "s") == 0) {
            s_font_level = 0;
        } else if (strcmp(font->valuestring, "m") == 0) {
            s_font_level = 1;
        } else if (strcmp(font->valuestring, "l") == 0) {
            s_font_level = 2;
        }
    }
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(root, "home_after_s");
    if (cJSON_IsNumber(home)) {
        const double value = home->valuedouble;
        s_home_after_s = value <= 0 ? 0 :
                         value < HOME_AFTER_MIN_S ? HOME_AFTER_MIN_S :
                         value > HOME_AFTER_MAX_S ? HOME_AFTER_MAX_S : (uint16_t)value;
        if (s_home_after_s == 0 && s_home_timer) {
            (void)esp_timer_stop(s_home_timer);
        }
    }
    const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(root, "tiles");
    if (cJSON_IsArray(tiles)) {
        uint8_t order[UI_CONTROL_COUNT];
        uint8_t count = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, tiles) {
            if (count >= UI_CONTROL_COUNT) {
                break;
            }
            const int tile = cJSON_IsString(item) ? ui_control_tile_from_name(item->valuestring) : -1;
            bool duplicate = false;
            for (uint8_t i = 0; i < count; i++) {
                duplicate = duplicate || order[i] == tile;
            }
            if (tile >= 0 && !duplicate) {
                order[count++] = (uint8_t)tile;
            }
        }
        /* Mindestens eine Kachel; eine leere Liste aendert nichts. */
        if (count > 0) {
            memcpy(s_tiles, order, count);
            s_tile_count = count;
        }
    }
    /*
     * Seit 0.4.4: Layout des Zifferblatts und Nachtfenster. Der Server ist
     * die Wahrheit -- ein `face` von hier gewinnt auch gegen eine Auswahl an
     * der Uhr, die er noch nicht kennt. Das neue Fenster greift im naechsten
     * Akku-Takt (zehn Sekunden), nicht aus diesem Bluetooth-Strang heraus.
     */
    const cJSON *face = cJSON_GetObjectItemCaseSensitive(root, "face");
    if (cJSON_IsString(face)) {
        const int layout = ui_face_layout_from_name(face->valuestring);
        if (layout >= 0) {
            s_face_layout = (uint8_t)layout;
        }
    }
    const cJSON *night = cJSON_GetObjectItemCaseSensitive(root, "night");
    if (cJSON_IsString(night) && !parse_night_window(night->valuestring)) {
        ESP_LOGW(TAG, "watch_settings: Nachtfenster \"%s\" ungueltig", night->valuestring);
    }
    ESP_LOGI(TAG, "watch_settings: Schrift %u, Rueckkehr %u s, %u Kacheln, Layout %s, "
             "Nacht %02u:%02u-%02u:%02u",
             (unsigned)s_font_level, (unsigned)s_home_after_s, (unsigned)s_tile_count,
             ui_face_layout_name((ui_face_layout_t)s_face_layout),
             (unsigned)(s_night_from_min / 60) % 24u, (unsigned)(s_night_from_min % 60),
             (unsigned)(s_night_to_min / 60) % 24u, (unsigned)(s_night_to_min % 60));
    apply_watch_settings_ui();
#else
    (void)root;
#endif
}

/*
 * Zehn Minuten vor dem naechsten Termin: dreimal vibrieren und den Schirm
 * wecken, auf dem der Countdown steht. Einmal je Termin -- geprueft im
 * Akku-Takt (alle zehn Sekunden), genauer muss es nicht sein.
 */
static void check_event_reminder(void)
{
    const int64_t start = s_next_event_start;
    if (start <= 0 || s_event_reminded) {
        return;
    }
    const int64_t now = (int64_t)time(NULL);
    if (now >= start - EVENT_REMINDER_LEAD_S && now < start) {
        s_event_reminded = true;
        ESP_LOGI(TAG, "Termin in %lld s -> Erinnerung", (long long)(start - now));
        haptic_pattern(3, 120, 150);
        note_activity();
        ui_status_show_home();
    }
}

/*
 * Rahmen an `speaker_rx`: die gesprochene Antwort fuer den Lautsprecher der
 * Uhr (audio_tone_speech_*). Waehrend einer Aufnahme wird nichts angenommen
 * -- Mikrofon und Lautsprecher teilen sich die I2S-Leitungen.
 */
static void ble_speaker_cb(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    switch (data[0]) {
    case 0x01: {
        if (s_recording || s_silent) {
            return;
        }
        const uint32_t rate = len >= 3 ? (uint32_t)(data[1] | (data[2] << 8)) : 8000;
        (void)audio_tone_speech_begin(rate);
        break;
    }
    case 0x02:
        audio_tone_speech_feed(data + 1, len - 1);
        break;
    case 0x03:
        audio_tone_speech_end();
        break;
    case 0x04:
        audio_tone_speech_stop();
        break;
    default:
        break;
    }
}

#if CONFIG_VOICE_BOARD_WATCH206
/*
 * `options` (0.4.5): die Optionen eines input_select, Antwort der Bruecke
 * auf `options_request`.
 *   {"event":"options","rev":4,"index":1,"current":0,"items":["Feierabend","Office","Gaming"]}
 * Direkt aus dem Bluetooth-Strang wie `list`: ui_options nimmt sich die
 * Sperre selbst. Eine leere Liste heisst "Karte inzwischen neu" -- die
 * Auswahl schliesst, der Fehlerton kommt ueber die Warteschlange (Ton und
 * Vibration gehoeren in den Ereignis-Task).
 */
static void apply_options_command(const cJSON *root)
{
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(root, "rev");
    const cJSON *index = cJSON_GetObjectItemCaseSensitive(root, "index");
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsNumber(rev) || !cJSON_IsNumber(index) || index->valuedouble < 0 ||
        index->valuedouble > 255) {
        return;
    }
    const char *texts[UI_OPTIONS_MAX];
    uint8_t count = 0;
    const cJSON *entry = NULL;
    if (cJSON_IsArray(items)) {
        cJSON_ArrayForEach(entry, items) {
            if (count >= UI_OPTIONS_MAX) {
                break;
            }
            /* Die Position zaehlt (`choice`), also auch ein kaputter Eintrag -- als leere Zeile. */
            texts[count++] = cJSON_IsString(entry) ? entry->valuestring : "";
        }
    }
    const ui_options_fill_t result =
        ui_options_fill((uint16_t)rev->valuedouble, (uint8_t)index->valuedouble,
                        cJSON_IsNumber(current) ? (int)current->valuedouble : -1, texts, count);
    if (result == UI_OPTIONS_FILL_EMPTY) {
        queue_app_event(APP_EVENT_OPTIONS_FAILED);
    }
}
#endif

static void ble_control_cb(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ignore invalid control json");
        return;
    }

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *question = cJSON_GetObjectItemCaseSensitive(root, "question");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *epoch = cJSON_GetObjectItemCaseSensitive(root, "epoch");
    const cJSON *tz_offset = cJSON_GetObjectItemCaseSensitive(root, "tz_offset_min");
    if (cJSON_IsString(event) && strcmp(event->valuestring, "device_name") == 0 &&
        cJSON_IsString(name)) {
        queue_device_name_event(name->valuestring);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "time") == 0 &&
               cJSON_IsNumber(epoch)) {
        /*
         * `valuedouble`, nicht `valueint`: cJSON legt Ganzzahlen zusaetzlich
         * als int ab, und der laeuft bei Sekunden seit 1970 auf 32-Bit-Zielen
         * ueber. Die Sekunde ist auf diesem Weg ohnehin die kleinste Einheit,
         * die Rundung des double liegt weit darunter.
         */
        queue_time_event((int64_t)epoch->valuedouble,
                         cJSON_IsNumber(tz_offset) ? (int32_t)tz_offset->valuedouble : 0);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "ui_state") == 0 &&
        cJSON_IsString(state)) {
        const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(root, "outcome");
        const cJSON *ref = cJSON_GetObjectItemCaseSensitive(root, "ref");
        const double ref_value = cJSON_IsNumber(ref) ? ref->valuedouble : 0;
        /* `stage` an `thinking` (0.4.4); Unbekanntes zaehlt wie keine Stufe. */
        const cJSON *stage = cJSON_GetObjectItemCaseSensitive(root, "stage");
        uint8_t stage_value = THINK_STAGE_NONE;
        if (cJSON_IsString(stage) && strcmp(stage->valuestring, "understand") == 0) {
            stage_value = THINK_STAGE_UNDERSTAND;
        } else if (cJSON_IsString(stage) && strcmp(stage->valuestring, "act") == 0) {
            stage_value = THINK_STAGE_ACT;
        }
        queue_ui_state_event(state->valuestring, cJSON_IsString(text) ? text->valuestring : "",
                             cJSON_IsString(question) ? question->valuestring : "",
                             cJSON_GetObjectItemCaseSensitive(root, "choices"),
                             ui_outcome_from_name(cJSON_IsString(outcome) ? outcome->valuestring
                                                                          : ""),
                             ref_value >= 1 && ref_value <= 65535 ? (uint16_t)ref_value : 0,
                             stage_value);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "face") == 0) {
        note_ble_transfer();
        apply_face_command(root);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "list") == 0) {
        note_ble_transfer();
        apply_list_command(root);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "weather") == 0) {
        note_ble_transfer();
        apply_weather_command(root);
#if CONFIG_VOICE_BOARD_WATCH206
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "options") == 0) {
        apply_options_command(root);
#endif
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "watch_settings") == 0) {
        apply_watch_settings_command(root);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "battery_log_request") == 0) {
        /*
         * Nicht von hier aus antworten: Die Teile gehen mit 30 ms Abstand
         * raus, und der Bluetooth-Strang soll dafuer nicht eine Sekunde
         * stehen. Ueber die Warteschlange wie alles andere.
         */
        const cJSON *since = cJSON_GetObjectItemCaseSensitive(root, "since");
        app_event_t ev = {
            .type = APP_EVENT_BATTERY_LOG_REQUEST,
            .epoch = cJSON_IsNumber(since) && since->valuedouble > 0 ? (int64_t)since->valuedouble
                                                                       : 0,
        };
        if (s_app_event_queue && xQueueSend(s_app_event_queue, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "battery_log_request: Warteschlange voll");
        }
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "transcript") == 0 &&
               cJSON_IsString(text)) {
        const cJSON *cancel = cJSON_GetObjectItemCaseSensitive(root, "cancel_ms");
        queue_transcript_event(text->valuestring,
                               cJSON_IsNumber(cancel) && cancel->valuedouble > 0
                                   ? (uint32_t)cancel->valuedouble : 0);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "interaction_mode") == 0 &&
               cJSON_IsString(mode)) {
        if (strcmp(mode->valuestring, "click_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_CLICK_TO_TALK);
        } else if (strcmp(mode->valuestring, "hold_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_HOLD_TO_TALK);
        } else {
            ESP_LOGW(TAG, "unknown interaction_mode %s", mode->valuestring);
        }
    }
    cJSON_Delete(root);
}

static uint32_t elapsed_button_ms(int64_t down_us)
{
    if (down_us <= 0) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - down_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    return (uint32_t)(elapsed_us / 1000);
}

/*
 * Wie lange die Sprechtaste wirklich unten war.
 *
 * Kam das Loslassen ueber den Abgleich im Aufnahme-Takt statt als Ereignis,
 * zaehlt der Zeitpunkt, an dem der Pin es zuerst gemeldet hat. Sonst zaehlte
 * die Verzoegerung des Abgleichs mit — und ein Antippen rutschte damit ueber
 * die Mindesthaltedauer, also genau an der Regel vorbei, die es abfangen
 * soll.
 */
static uint32_t primary_hold_ms(void)
{
    if (s_primary_release_us > 0 && s_primary_release_us > s_primary_down_us) {
        return (uint32_t)((s_primary_release_us - s_primary_down_us) / 1000);
    }
    return elapsed_button_ms(s_primary_down_us);
}

static void apply_app_ui_state(const char *state, const char *text, const char *question)
{
    apply_app_ui_state_choices(state, text, question, NULL, 0, UI_OUTCOME_NONE, 0,
                               THINK_STAGE_NONE);
}

static void apply_app_ui_state_choices(const char *state, const char *text, const char *question,
                                       const char (*choices)[UI_STATUS_CHOICE_LEN],
                                       uint8_t choice_count, ui_outcome_t outcome, uint16_t ref,
                                       uint8_t stage)
{
    ESP_LOGI(TAG, "apply ui_state state=%s text_len=%u current=%s recording=%d",
             state ? state : "nil",
             (unsigned)(text ? strlen(text) : 0),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    stop_host_response_timer();
    /* Der Turn denkt nicht mehr: Der naechste `thinking` zaehlt wieder ab null. */
    if (strcmp(state, "thinking") != 0) {
        s_thinking_since_us = 0;
    }
    if (strcmp(state, "ready") == 0) {
        if (s_recording) {
            ESP_LOGI(TAG, "ignore ready ui_state while recording");
            return;
        }
        s_app_ui_state = APP_UI_STATE_READY;
        /* Traegt die Antwort einen Text, gehoert er aufs Display — bisher
         * wurde er hier verworfen und war damit nur in der App zu sehen. */
        ui_status_set_idle_text(text, question, outcome, ref);
        if (choice_count > 0) {
            ui_status_set_choices(choices, choice_count);
        }
        /*
         * Nur bei echtem Inhalt einen Ton: die Bruecke setzt "ready" auch
         * beim Verbinden und nach einer verworfenen, zu kurzen Aufnahme.
         */
        if (text && text[0]) {
            play_tone(AUDIO_TONE_DONE);
#if CONFIG_VOICE_BOARD_WATCH206
            s_answer_unread = true;
            s_answer_at_us = esp_timer_get_time();
            /* Tecki freut sich drei Sekunden -- nur auf der Uhr, der StickS3 bleibt wie er ist. */
            if (choice_count == 0) {
                ui_status_set_mood(UI_STATUS_MOOD_HAPPY, 3000);
            }
#endif
        }
        note_activity();
        voice_ble_request_slow_interval();
    } else if (strcmp(state, "recording") == 0) {
        s_app_ui_state = APP_UI_STATE_RECORDING;
        if (!s_recording) {
            ui_status_set_recording(0);
        }
    } else if (strcmp(state, "thinking") == 0) {
        if (s_app_ui_state != APP_UI_STATE_THINKING || s_thinking_since_us == 0) {
            s_thinking_since_us = esp_timer_get_time();
        }
        s_app_ui_state = APP_UI_STATE_THINKING;
#if CONFIG_VOICE_BOARD_WATCH206
        /*
         * Eine Stufe waehrend der Frist des Transkripts verdeckt "Abbrechen"
         * nicht: Die Szene bleibt, bis die Frist um ist oder die naechste
         * kommt. Ohne Stufe (aeltere Bruecke) wie bisher -- die schickt
         * `thinking` erst nach der Frist.
         */
        if (stage != THINK_STAGE_NONE && ui_status_transcript_cancellable()) {
            ESP_LOGI(TAG, "Stufe %u waehrend der Transkript-Frist: Szene bleibt", (unsigned)stage);
            return;
        }
#endif
        ui_status_set_thinking(stage == THINK_STAGE_UNDERSTAND ? "Verstehe …" :
                               stage == THINK_STAGE_ACT ? "Erledige …" : "Denkt nach",
                               text, s_thinking_since_us);
    } else if (strcmp(state, "pending_confirmation") == 0) {
        s_app_ui_state = APP_UI_STATE_PENDING_CONFIRMATION;
        ui_status_set_partial_text("Bestätigen oder abbrechen");
    } else if (strcmp(state, "error") == 0) {
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error(text && text[0] ? text : "Unbekannter Fehler");
        play_tone(AUDIO_TONE_ERROR);
        voice_ble_request_slow_interval();
    } else if (strcmp(state, "no_speech") == 0) {
        s_app_ui_state = APP_UI_STATE_NO_SPEECH;
        ui_status_set_no_speech(text);
        /*
         * Derselbe kurze tiefe Ton wie bei einer verworfenen Aufnahme, nicht
         * die zwei absteigenden des Fehlers: Am Ohr soll "es kam nichts an"
         * klingen wie "verworfen" und nicht wie "kaputt".
         */
        play_tone(AUDIO_TONE_CANCEL);
        note_activity();
        voice_ble_request_slow_interval();
    } else {
        ESP_LOGW(TAG, "unknown ui_state %s", state);
    }
    ESP_LOGI(TAG, "applied ui_state current=%s recording=%d",
             app_ui_state_name(s_app_ui_state),
             s_recording);
}

static void apply_interaction_mode(interaction_mode_t mode)
{
    s_interaction_mode = mode;
    /*
     * Die Bedienart steht nicht mehr auf dem Schirm: Der Bereitzustand zeigt
     * die Uhrzeit, und "Halten zum Sprechen" hat dort niemand mehr gebraucht
     * (ui_status.c, ui_status_set_idle). Sie steuert weiterhin, was ein Druck
     * bewirkt — nur eben ohne Beschriftung.
     */
    if (s_app_ui_state == APP_UI_STATE_READY && !s_recording) {
        ui_status_set_idle();
    }
    ESP_LOGI(TAG, "interaction mode %s",
             mode == INTERACTION_MODE_CLICK_TO_TALK ? "click_to_talk" : "hold_to_talk");
}

static void ble_ota_cb(voice_ble_ota_event_t event, uint32_t written, uint32_t size)
{
    switch (event) {
    case VOICE_BLE_OTA_EVENT_BEGIN:
        queue_app_event_with_ota(APP_EVENT_OTA_BEGIN, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_PROGRESS:
        queue_app_event_with_ota(APP_EVENT_OTA_PROGRESS, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_DONE:
        queue_app_event_with_ota(APP_EVENT_OTA_DONE, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_ERROR:
    case VOICE_BLE_OTA_EVENT_ABORT:
        queue_app_event(APP_EVENT_OTA_END);
        break;
    }
}

#if CONFIG_VOICE_BOARD_WATCH206
static ui_control_state_t control_state(void)
{
    return (ui_control_state_t){
        .brightness = BRIGHTNESS_NAMES[s_brightness_level % 3],
        .silent = s_silent,
        .cinema = s_cinema,
        .saver = s_saver,
        .battery_percent = s_battery_percent,
        .charging = s_battery_charging,
        .remaining = s_battery_remaining,
    };
}

/* Ortsstunde aus Systemzeit und gemeldetem Abstand; -1 ohne gestellte Uhr. */
static int local_hour_now(void)
{
    if (!s_clock_valid) {
        return -1;
    }
    const time_t now = time(NULL) + (time_t)s_clock_offset_min * 60;
    struct tm parts;
    gmtime_r(&now, &parts);
    return parts.tm_hour;
}

/* Nachtlade-Modus an oder aus: Anzeige und Helligkeit folgen. */
static void set_night_mode(bool on)
{
    if (s_night_mode == on) {
        return;
    }
    s_night_mode = on;
    ESP_LOGI(TAG, "Nachtlade-Modus %s", on ? "an" : "aus");
    ui_status_set_night_mode(on);
    if (on) {
        if (s_display_dim_timer) {
            (void)esp_timer_stop(s_display_dim_timer);
        }
        if (s_display_off_timer) {
            (void)esp_timer_stop(s_display_off_timer);
        }
        set_display_off(false);
        s_display_dimmed = false;
        ui_status_set_idle_dimmed(false);
        /* Am Kabel: die 80 MHz kosten nichts, was zaehlt, und die Uhrzeit bleibt fluessig. */
        set_display_apb_lock(true);
        (void)ui_status_set_brightness(NIGHT_BRIGHTNESS);
    } else {
        (void)ui_status_set_brightness(active_brightness());
        restart_display_dim_timer();
    }
}

/*
 * Gilt der Nachtlade-Modus gerade? Am USB-Kabel, zwischen 22 und 6 Uhr, und
 * nicht waehrend der zehn Sekunden nach einer Beruehrung. Gerufen im
 * Akku-Takt und nach dem Ende der zehn Sekunden.
 */
static void update_night_mode(void)
{
    const int hour = local_hour_now();
    const bool window = hour >= 0 && (hour >= NIGHT_FROM_HOUR || hour < NIGHT_TO_HOUR);
    set_night_mode(s_usb_powered && window && !s_night_peek && !s_recording && !s_ota_updating);
}

/* Ortsminute des Tages (0..1439) aus Systemzeit und gemeldetem Abstand; -1 ohne gestellte Uhr. */
static int local_minute_now(void)
{
    if (!s_clock_valid) {
        return -1;
    }
    const time_t now = time(NULL) + (time_t)s_clock_offset_min * 60;
    struct tm parts;
    gmtime_r(&now, &parts);
    return parts.tm_hour * 60 + parts.tm_min;
}

/*
 * Liegt die Ortszeit im Nachtfenster? Dann rot und gedeckelt (s. o.). Gerufen
 * im Akku-Takt und nach dem Stellen der Uhr -- ein neues Fenster aus
 * `watch_settings` greift damit spaetestens zehn Sekunden spaeter.
 */
static void update_night_window(void)
{
    const int now = local_minute_now();
    const int from = s_night_from_min;
    const int to = s_night_to_min;
    bool in = false;
    if (now >= 0 && from != to) {
        in = from < to ? (now >= from && now < to) : (now >= from || now < to);
    }
    if (in == s_night_window) {
        return;
    }
    s_night_window = in;
    s_night_bright_override = false;
    ESP_LOGI(TAG, "Nachtmodus %s", in ? "an" : "aus");
    ui_status_set_night_red(in);
    /* Der Nachtlade-Modus haelt seine eigene Helligkeit; sonst gilt die neue Obergrenze sofort. */
    if (!s_night_mode && !ui_torch_on()) {
        apply_brightness_now();
    }
}

static void night_peek_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_NIGHT_PEEK_END);
}

/* Eine Beruehrung im Nachtlade-Modus: zehn Sekunden das normale Zifferblatt. */
static void start_night_peek(void)
{
    if (!s_night_peek_timer) {
        const esp_timer_create_args_t args = {
            .callback = night_peek_timer_cb,
            .name = "night_peek",
        };
        if (esp_timer_create(&args, &s_night_peek_timer) != ESP_OK) {
            return;
        }
    }
    s_night_peek = true;
    set_night_mode(false);
    (void)esp_timer_stop(s_night_peek_timer);
    (void)esp_timer_start_once(s_night_peek_timer, NIGHT_PEEK_US);
}

/*
 * Akku-Restzeit nachfuehren (alle zehn Sekunden, aus update_battery_status):
 * Stichprobe alle zehn Minuten ohne Kabel; am Kabel leert sich der Ring.
 * Die Kachel bekommt "noch 9 h", das Zifferblatt ab 20 % "noch etwa 2 h".
 */
/*
 * Akku-Protokoll: die letzten sechs Proben in den NVS -- stuendlich, und nur
 * wenn seit dem letzten Schreiben etwas Neues da ist. Der Blob ist klein
 * (36 Byte), der Takt niedrig; die Flash-Zellen halten das Jahre.
 */
static void battery_log_save(void)
{
    battery_log_sample_t tail[BATTERY_LOG_NVS_SAMPLES];
    const size_t n = battery_log_export_tail(&s_battery_log, tail, BATTERY_LOG_NVS_SAMPLES);
    if (n == 0) {
        return;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BATTERY_LOG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Akku-Protokoll: NVS %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(nvs, BATTERY_LOG_NVS_KEY, tail, n * sizeof(tail[0]));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Akku-Protokoll: NVS schreiben %s", esp_err_to_name(err));
        return;
    }
    s_battery_log_saved_t = tail[n - 1].t;
}

/* Beim Kaltstart: was der NVS hat, in den leeren Ring. Nach dem Tiefschlaf ist der Ring voller. */
static void battery_log_load(void)
{
    if (s_battery_log.count != 0) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(BATTERY_LOG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    battery_log_sample_t tail[BATTERY_LOG_NVS_SAMPLES];
    size_t size = sizeof(tail);
    esp_err_t err = nvs_get_blob(nvs, BATTERY_LOG_NVS_KEY, tail, &size);
    nvs_close(nvs);
    if (err != ESP_OK || size % sizeof(tail[0]) != 0) {
        return;
    }
    battery_log_import(&s_battery_log, tail, size / sizeof(tail[0]));
    const battery_log_sample_t *newest = battery_log_newest(&s_battery_log);
    s_battery_log_saved_t = newest ? newest->t : 0;
    ESP_LOGI(TAG, "Akku-Protokoll: %u Proben aus dem NVS", (unsigned)s_battery_log.count);
}

/*
 * Eine Stichprobe, wenn sie faellig ist (alle zehn Minuten) und die Uhr
 * gestellt ist -- gerufen im Akku-Takt aus update_battery_status().
 */
static void battery_log_sample(int level, bool charging, bool usb)
{
    const int64_t now64 = (int64_t)time(NULL);
    if (now64 < (int64_t)BATTERY_LOG_MIN_EPOCH || now64 > (int64_t)UINT32_MAX) {
        return;
    }
    const uint32_t now = (uint32_t)now64;
    if (!battery_log_due(&s_battery_log, now)) {
        return;
    }
    uint8_t flags = 0;
    if (charging) {
        flags |= BATTERY_LOG_FLAG_CHARGING;
    }
    if (usb) {
        flags |= BATTERY_LOG_FLAG_USB;
    }
    if (!s_display_off) {
        flags |= BATTERY_LOG_FLAG_DISPLAY_ON;
    }
    if (voice_ble_is_connected()) {
        flags |= BATTERY_LOG_FLAG_BLE;
    }
    if (s_light_sleep_on) {
        flags |= BATTERY_LOG_FLAG_LIGHT_SLEEP;
    }
    if (s_recorded_since_sample) {
        flags |= BATTERY_LOG_FLAG_RECORDED;
    }
    s_recorded_since_sample = false;
    battery_log_add(&s_battery_log, now, level, flags);
    if (now - s_battery_log_saved_t >= BATTERY_LOG_NVS_INTERVAL_S || now < s_battery_log_saved_t) {
        battery_log_save();
    }
}

/*
 * Antwort auf `battery_log_request`: alle Proben mit t > since, in Teilen von
 * je hoechstens 240 Byte, 30 ms Abstand. Laeuft im Ereignis-Strang -- bei 48
 * Stunden sind das 36 Teile und gut eine Sekunde, in der die Anzeige steht;
 * der Normalfall nach dem Verbinden ist ein Teil.
 */
static void send_battery_log(int64_t since)
{
    const uint32_t since32 = since > 0 && since < (int64_t)UINT32_MAX ? (uint32_t)since : 0;
    const unsigned total = battery_log_part_count(battery_log_count_since(&s_battery_log, since32));
    char json[BATTERY_LOG_PART_MAX_BYTES + 1];
    for (unsigned part = 0; part < total; part++) {
        (void)battery_log_part_json(&s_battery_log, since32, part, json, sizeof(json));
        if (voice_ble_send_state_json(json) != ESP_OK) {
            ESP_LOGW(TAG, "Akku-Protokoll: Teil %u/%u nicht gesendet", part, total);
            return;
        }
        if (part + 1 < total) {
            vTaskDelay(pdMS_TO_TICKS(BATTERY_LOG_PART_GAP_MS));
        }
    }
    ESP_LOGI(TAG, "Akku-Protokoll: %u Teil(e) ab %lu gesendet", total, (unsigned long)since32);
}

static void update_battery_estimate(int level, bool external)
{
    const int64_t now = (int64_t)time(NULL);
    if (external) {
        battery_ring_clear(&s_battery_ring);
    } else if (battery_ring_due(&s_battery_ring, now)) {
        battery_ring_add(&s_battery_ring, now, level);
    }
    const int32_t minutes = external ? -1 : battery_ring_remaining_min(&s_battery_ring, level);
    battery_format_remaining(s_battery_remaining, sizeof(s_battery_remaining), minutes, false);
    char face[24];
    battery_format_remaining(face, sizeof(face), level <= BATTERY_FACE_HINT_PERCENT ? minutes : -1,
                             true);
    ui_status_set_battery_remaining(face);
    if (ui_control_visible()) {
        const ui_control_state_t st = control_state();
        ui_control_update(&st);
    }
}

static void apply_brightness_now(void)
{
    if (!s_display_dimmed) {
        (void)ui_status_set_brightness(active_brightness());
    }
}

/* Eine Kachel im Kontrollzentrum angetippt. */
static void handle_control_tile(int tile)
{
    switch ((ui_control_tile_t)tile) {
    case UI_CONTROL_BRIGHTNESS:
        s_brightness_level = (uint8_t)((s_brightness_level + 1) % 3);
        /* Nachts darf der Nutzer trotzdem hochstellen -- bis der Schirm ausgeht. */
        s_night_bright_override = s_night_window;
        apply_brightness_now();
        break;
    case UI_CONTROL_SILENT:
        s_silent = !s_silent;
        break;
    case UI_CONTROL_CINEMA:
        s_cinema = !s_cinema;
        break;
    case UI_CONTROL_SAVER:
        s_saver = !s_saver;
        apply_brightness_now();
        break;
    case UI_CONTROL_TORCH:
        ui_control_hide();
        (void)ui_status_set_brightness(255);
        ui_torch_set(true);
        return;
    default:
        return;
    }
    haptic_pattern(1, 25, 0);
    const ui_control_state_t st = control_state();
    ui_control_update(&st);
}

/*
 * "Abbrechen" in der Transkript-Szene (Flaeche, Sprechtaste oder Hand aufs
 * Display): Die Bruecke fuehrt den Satz dann nicht aus und meldet sich mit
 * einem `ready` ("Abgebrochen.").
 */
static void cancel_transcript(void)
{
    ESP_LOGI(TAG, "Transkript abgebrochen");
    if (voice_ble_send_cancel_transcript() == ESP_OK) {
        ui_status_set_transcript_cancelled();
        haptic_pattern(1, 40, 0);
    } else {
        ui_status_set_not_connected();
        play_tone(AUDIO_TONE_CANCEL);
    }
}

/*
 * Zurueck aufs Ziffernblatt nach `home_after_s` ohne Bedienung -- Karten,
 * Kontrollzentrum und Verlauf zu. Nicht, solange etwas laeuft, das man
 * sehen muss (Aufnahme, Denken, Rueckfrage, Transkript, Update) oder das
 * man absichtlich angeschaltet hat (Taschenlampe); dann neuer Versuch nach
 * derselben Zeit. Eine ungelesene Antwort haelt sich mindestens zwei
 * Minuten (UNREAD_ANSWER_HOLD_US).
 */
static void handle_home_timeout(void)
{
    if (s_home_after_s == 0) {
        return;
    }
    if (s_recording || s_ota_updating || ui_torch_on() ||
        s_app_ui_state == APP_UI_STATE_RECORDING || s_app_ui_state == APP_UI_STATE_THINKING ||
        s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION ||
        ui_status_choices_open() || ui_status_transcript_shown()) {
        restart_home_timer();
        return;
    }
    if (s_answer_unread) {
        const int64_t shown_us = esp_timer_get_time() - s_answer_at_us;
        if (shown_us >= 0 && shown_us < UNREAD_ANSWER_HOLD_US) {
            (void)esp_timer_stop(s_home_timer);
            (void)esp_timer_start_once(s_home_timer, (uint64_t)(UNREAD_ANSWER_HOLD_US - shown_us));
            return;
        }
        s_answer_unread = false;
    }
    ESP_LOGI(TAG, "%u s ohne Bedienung -> Ziffernblatt", (unsigned)s_home_after_s);
    ui_face_picker_hide();
    ui_options_hide();
    ui_control_hide();
    ui_cards_hide();
    ui_status_show_home();
}

/*
 * Hand aufs Display: alles still. Laufende Aufnahme verwerfen, Ton und
 * Vibration aus, Schirm dunkel -- wie ein Abdecken bei Apple und Samsung.
 * Laeuft gerade die Frist eines Transkripts, ist das zugleich "Abbrechen".
 */
static void handle_palm(void)
{
    if (ui_status_transcript_cancellable()) {
        (void)voice_ble_send_cancel_transcript();
    }
    audio_tone_speech_stop();
    if (s_recording) {
        abort_recording();
    }
    ui_torch_set(false);
    ui_face_picker_hide();
    ui_options_hide();
    ui_control_hide();
    ui_cards_hide();
    ui_status_show_home();
    (void)ui_status_set_brightness(0);
    s_display_dimmed = true;
    ui_status_set_idle_dimmed(true);
    set_display_apb_lock(false);
    if (s_display_dim_timer) {
        (void)esp_timer_stop(s_display_dim_timer);
    }
    if (s_display_off_timer) {
        (void)esp_timer_stop(s_display_off_timer);
    }
    set_display_off(true);
}

/*
 * Die Gestensprache der Uhr -- ein Regelwerk fuer alle Szenen (10/2026,
 * Reihe seit 11/2026). Dieselbe Tabelle steht im README (Abschnitt der Uhr).
 *
 * Waagerecht liegt alles in einer Reihe: Verlauf | Zifferblatt | Heute |
 * Einkauf | Zuhause | Wetter. Nach links wischen holt, was rechts liegt.
 *
 *   Geste             Zifferblatt      Antwort / Verlauf      Karten              Kontrollzentrum
 *   ----------------  ---------------  ---------------------  ------------------  ---------------
 *   vom oberen Rand   Kontrollzentrum  Kontrollzentrum        Kontrollzentrum     schliesst
 *   nach unten
 *   vom linken Rand   Verlauf          Zifferblatt            Zifferblatt         Zifferblatt
 *   nach rechts
 *   nach unten        nichts           zurueck: Zifferblatt   Liste zurueck; am   schliesst
 *                                                             Anfang: Zifferblatt
 *   nach oben         Karten           Karten                 Liste weiter        schliesst
 *   links             Heute            neuer; von der         naechste Karte;     --
 *                                      juengsten: Zifferbl.   am Ende nichts
 *   rechts            Verlauf (zuerst  aelter                 vorige Karte; von   --
 *                     die Bilanz)                             Heute: Zifferblatt
 *   Tippen            Verlauf oeffnen  Ja/Nein; Nochmal /     Zeile abhaken /     Kachel
 *                                      Rueckgaengig; Kurz-    schalten; Heute und
 *                                      form <-> ganz; sonst   Schalter mit `c`:
 *                                      aelter                 markieren, dann
 *                                                             "OK"; Flag `s`:
 *                                                             Auswahl (0.4.5);
 *                                                             Rueckg.
 *   Lange druecken    Layout-Auswahl   wie Tippen             wie Tippen          wie Tippen
 *   (0,6-2 s, 0.4.4)  (Info, Minimal,
 *                     Gross)
 *   Hand aufs Display alles still: Aufnahme verworfen, Ebenen zu, Schirm dunkel
 *
 * In der Layout-Auswahl: links/rechts blaettert, Tippen uebernimmt, nach
 * unten oder vom linken Rand bricht ab (auch nach 10 s ohne Eingabe). In der
 * Auswahl eines input_select (0.4.5, Kartenzeile mit Flag `s`): Tippen auf
 * eine Option waehlt sie (auf die aktive: nur zu), oben/unten scrollt, nach
 * unten am Anfang oder vom linken Rand bricht ab (auch nach 10 s). Bis
 * 0.4.3 war langer Druck auf dem Schirm frei -- eine Beruehrung bis 0,8 s
 * ohne Wischen war ein Tippen, darueber zaehlte sie als Aermel und tat
 * nichts. Der lange Druck auf die Blaettertaste (Aktion) ist davon
 * unberuehrt.
 *
 * Sonderfaelle: Ein gedimmter Schirm wird von der ersten Beruehrung nur
 * geweckt; waehrend der Aufnahme tut der Schirm nichts (das Handgelenk
 * liegt oft am Aermel); in der Transkript-Szene zaehlt nur "Abbrechen"
 * (und die Hand, die ebenfalls abbricht); bei der Taschenlampe schaltet jede
 * Beruehrung sie aus.
 *
 * Abweichungen von vorher, mit Grund:
 *   - Karten, nach unten: schliesst sofort. Vorher blaetterte es erst eine
 *     Seite zurueck und schloss nur auf der ersten -- dieselbe Geste tat je
 *     nach Seite zweierlei. Die Seiten laufen mit "nach oben" im Kreis.
 *   - Karten zu fuehrt aufs Zifferblatt (11/2026: die Karten liegen jetzt
 *     rechts daneben in der Reihe, nicht mehr eine Ebene darueber -- Wunsch
 *     des Nutzers). Nach oben oeffnet sie weiterhin, als zweiter Weg.
 *   - Verlauf: Rechts ist jetzt aelter, links neuer (vorher umgekehrt),
 *     damit der Verlauf links und die Karten rechts vom Zifferblatt liegen.
 *   - Nach oben oeffnet die Karten nur, wenn gerade nichts laeuft (keine
 *     Aufnahme, kein Denken). Vorher ging das auch mitten im Nachdenken, und
 *     die Antwort kam hinter der Karte an.
 *   - Tippen auf eine Antwort in Kurzform zeigt die ganze (und zurueck).
 *     Ohne Kurzform bleibt Tippen, was es war: die Blaettertaste.
 *   - Karte Heute, Tippen (11/2026): markiert die Zeile nur; abgehakt wird
 *     erst mit "OK - erledigt" in der Zeile. Ein Fehltipp am Handgelenk
 *     hakte vorher sofort ab. Einkauf und Zuhause bleiben beim direkten
 *     Tippen (Wunsch des Nutzers).
 *   - Kontrollzentrum und Karten oeffnen nicht in der Transkript-Szene:
 *     Sie verdeckten genau die zwei Sekunden, in denen "Abbrechen" gilt.
 */
/*
 * Chips im Verlauf (V3, V4, V5): "Nochmal" schickt die Frage als `repeat`,
 * die Bruecke fuehrt sie wie den Aktionssatz aus und antwortet normal.
 * "Rueckgaengig" schickt `undo_turn` mit der `ref` des Turns oder `undo`
 * mit den Koordinaten der Karten-Aktion; die Antwort kommt als `ui_state`
 * ("Rueckgaengig" als Frage, Text, outcome done oder error). `true`, wenn
 * ein Chip getroffen war -- dann tut das Tippen sonst nichts.
 */
static bool handle_history_chip(int16_t x, int16_t y)
{
    ui_history_chip_hit_t hit;
    if (!ui_status_history_chip_at(x, y, &hit)) {
        return false;
    }
    esp_err_t err = ESP_FAIL;
    switch (hit.chip) {
    case UI_HISTORY_CHIP_REPEAT:
        ESP_LOGI(TAG, "Nochmal: %s", hit.question);
        err = voice_ble_send_repeat(hit.question);
        if (err == ESP_OK) {
            apply_app_ui_state("thinking", "", "");
        }
        break;
    case UI_HISTORY_CHIP_UNDO_TURN:
        ESP_LOGI(TAG, "Rueckgaengig Turn ref=%u", (unsigned)hit.ref);
        err = voice_ble_send_undo_turn(hit.ref);
        if (err == ESP_OK) {
            ui_status_history_mark_undone();
            apply_app_ui_state("thinking", "", "");
        }
        break;
    case UI_HISTORY_CHIP_UNDO_CARD:
        ESP_LOGI(TAG, "Rueckgaengig Karte %s rev=%u #%u", ui_cards_kind_name(hit.card_kind),
                 (unsigned)hit.card_rev, (unsigned)hit.card_index);
        err = voice_ble_send_undo(ui_cards_kind_name(hit.card_kind), hit.card_rev, hit.card_index);
        if (err == ESP_OK) {
            ui_cards_restore((ui_card_kind_t)hit.card_kind, hit.card_rev, hit.card_index);
            ui_status_history_mark_undone();
        }
        break;
    default:
        return false;
    }
    if (err == ESP_OK) {
        haptic_pattern(1, 30, 0);
    } else {
        ui_status_set_not_connected();
        play_tone(AUDIO_TONE_CANCEL);
    }
    return true;
}

static bool cards_allowed(void)
{
    return s_app_ui_state != APP_UI_STATE_RECORDING && s_app_ui_state != APP_UI_STATE_THINKING &&
           s_app_ui_state != APP_UI_STATE_PENDING_CONFIRMATION;
}

/*
 * Layout an der Uhr gewaehlt (Paket I): sofort zeigen, im RTC-Speicher
 * merken und der Bruecke melden, die es beim Server speichert und danach
 * `watch_settings` neu schickt. Ohne Verbindung gilt es trotzdem -- bis die
 * naechsten `watch_settings` des Servers kommen, der die Wahrheit ist.
 */
static void choose_face_layout(ui_face_layout_t layout)
{
    s_face_layout = (uint8_t)layout;
    ui_status_set_face_layout(layout);
    ESP_LOGI(TAG, "Zifferblatt-Layout %s", ui_face_layout_name(layout));
    if (voice_ble_send_face_layout(ui_face_layout_name(layout)) != ESP_OK) {
        ESP_LOGW(TAG, "face_layout nicht gesendet (keine Bruecke)");
    }
    haptic_pattern(1, 30, 0);
}

/*
 * Option eines input_select gewaehlt (0.4.5): `option` an die Bruecke, die
 * daraus `select_option` beim Server macht. Die Zeile zeigt "..." bis zur
 * neuen Karte, "Rueckgaengig" steht fuenf Sekunden, und der Verlauf haelt
 * "Buero Modus: Gaming" mit den Karten-Koordinaten -- das `undo` von dort
 * findet beim Server die vorige Option.
 */
static void choose_option(const ui_options_choice_t *choice)
{
    ESP_LOGI(TAG, "Option %s rev=%u #%u -> %u", ui_cards_kind_name(choice->kind),
             (unsigned)choice->rev, (unsigned)choice->index, (unsigned)choice->choice);
    if (voice_ble_send_option(ui_cards_kind_name(choice->kind), choice->rev, choice->index,
                              choice->choice) != ESP_OK) {
        play_tone(AUDIO_TONE_CANCEL);
        return;
    }
    haptic_pattern(1, 40, 0);
    ui_cards_mark_sent(choice->kind, choice->rev, choice->index);
    char line[UI_CARD_TEXT_LEN + UI_OPTIONS_TEXT_LEN + 4];
    snprintf(line, sizeof(line), "%s: %s", choice->title, choice->option);
    ui_status_history_add_card(line, (uint8_t)choice->kind, choice->rev, choice->index);
}

static void handle_touch(const app_event_t *event)
{
    /*
     * Langer Druck (0.4.4) ist nur auf dem Zifferblatt eine eigene Geste --
     * dort oeffnet er die Auswahl der Layouts. Ueberall sonst bleibt er, was
     * er bis 0.4.3 war: ein Tippen (bis 0,8 s zaehlte jede Beruehrung ohne
     * Wischen als Tippen). Bei gedimmtem Schirm weckt er nur, wie jedes Tippen.
     */
    uint8_t gesture = event->gesture;
    if (gesture == VOICE_BOARD_LONG_PRESS &&
        (s_night_mode || s_display_dimmed || ui_face_picker_visible() || ui_options_visible() ||
         ui_control_visible() || ui_cards_visible() || !ui_status_face_shown())) {
        gesture = VOICE_BOARD_TAP;
    }
    if (gesture == VOICE_BOARD_PALM) {
        handle_palm();
        return;
    }
    if (ui_torch_on()) {
        /* Taschenlampe: jede Beruehrung schaltet sie aus. */
        ui_torch_set(false);
        apply_brightness_now();
        note_user_input();
        return;
    }
    if (s_night_mode) {
        /* Nachts am Kabel: die Beruehrung zeigt zehn Sekunden das normale Zifferblatt, sonst nichts. */
        start_night_peek();
        note_user_input();
        return;
    }
    const bool was_dimmed = s_display_dimmed;
    note_user_input();
    if (was_dimmed || s_recording) {
        return;
    }
    if (ui_status_transcript_shown()) {
        if (gesture == VOICE_BOARD_TAP &&
            ui_status_transcript_cancel_at(event->touch_x, event->touch_y)) {
            cancel_transcript();
        }
        return;
    }
    /* Auswahl des Zifferblatt-Layouts: alle Gesten gehen an sie. */
    if (ui_face_picker_visible()) {
        ui_face_layout_t chosen = UI_FACE_INFO;
        const ui_face_picker_action_t action =
            ui_face_picker_touch(gesture, event->touch_x, event->touch_y, &chosen);
        if (action == UI_FACE_PICKER_CHOSEN) {
            choose_face_layout(chosen);
        } else if (action == UI_FACE_PICKER_CANCELLED) {
            haptic_pattern(1, 15, 0);
        }
        return;
    }
    /* Auswahl eines input_select (0.4.5): liegt ueber der Karte, alle Gesten gehen an sie. */
    if (ui_options_visible()) {
        ui_options_choice_t choice;
        const ui_options_action_t action =
            ui_options_touch(gesture, event->touch_x, event->touch_y, &choice);
        if (action == UI_OPTIONS_CHOSEN) {
            choose_option(&choice);
        } else if (action == UI_OPTIONS_CLOSED) {
            haptic_pattern(1, 15, 0);
        }
        return;
    }
    if (gesture == VOICE_BOARD_LONG_PRESS) {
        /* Nur noch auf dem Zifferblatt (s. o.): die Auswahl der Layouts. */
        ui_face_picker_show((ui_face_layout_t)s_face_layout);
        haptic_pattern(1, 40, 0);
        return;
    }
    /*
     * Zurueck-Geste vom linken Rand (Paket G): Ein Wischen nach rechts, das
     * am linken Rand beginnt, geht ueberall eine Ebene zurueck -- Karten,
     * Wetter und Kontrollzentrum aufs Zifferblatt, der Verlauf ebenso. Auf
     * dem Zifferblatt selbst gibt es nichts, wohin; dort bleibt es das
     * normale Wischen nach rechts (in den Verlauf).
     */
    if (gesture == VOICE_BOARD_SWIPE_RIGHT && event->touch_x < BACK_EDGE_PX &&
        (ui_control_visible() || ui_cards_visible() || ui_status_in_history())) {
        ui_control_hide();
        ui_cards_hide();
        ui_status_show_home();
        return;
    }
    if (ui_control_visible()) {
        if (gesture == VOICE_BOARD_TAP) {
            const int tile = ui_control_tile_at(event->touch_x, event->touch_y);
            if (tile >= 0) {
                handle_control_tile(tile);
            }
        } else if (gesture == VOICE_BOARD_SWIPE_UP ||
                   gesture == VOICE_BOARD_SWIPE_DOWN) {
            ui_control_hide();
        }
        return;
    }
    /*
     * Vom oberen Rand nach unten: das Kontrollzentrum -- egal, was gerade
     * zu sehen ist. Weiter unten angesetzt ist "nach unten" eine Ebene
     * zurueck. touch_y ist der Punkt, an dem der Finger aufsetzte.
     */
    if (gesture == VOICE_BOARD_SWIPE_DOWN && event->touch_y < TOP_EDGE_PX) {
        ui_cards_hide();
        const ui_control_state_t st = control_state();
        ui_control_show(&st);
        return;
    }
    if (ui_cards_visible()) {
        ui_cards_hit_t hit;
        const ui_cards_action_t action = ui_cards_touch(gesture, event->touch_x,
                                                        event->touch_y, &hit);
        if (action == UI_CARDS_ITEM) {
            if (voice_ble_send_item(ui_cards_kind_name(hit.kind), hit.rev, hit.index) == ESP_OK) {
                haptic_pattern(1, 40, 0);
                /*
                 * Karten-Aktion als Verlaufseintrag (V5): "Erledigt: Muell
                 * rausstellen" bzw. "Licht Kueche: An", mit den Koordinaten
                 * fuer ein spaeteres `undo` aus dem Verlauf (15 Minuten).
                 */
                char line[UI_CARD_TEXT_LEN + 16];
                if (hit.kind == UI_CARD_SWITCHES) {
                    /* Ohne An/Aus (Option, Schloss) kennt die Uhr den Zielstand nicht. */
                    snprintf(line, sizeof(line), "%s: %s", hit.text,
                             !hit.binary ? "weiter" : hit.on ? "An" : "Aus");
                } else {
                    snprintf(line, sizeof(line), "Erledigt: %s", hit.text);
                }
                ui_status_history_add_card(line, (uint8_t)hit.kind, hit.rev, hit.index);
            } else {
                play_tone(AUDIO_TONE_CANCEL);
            }
        } else if (action == UI_CARDS_UNDO) {
            ESP_LOGI(TAG, "Rueckgaengig: %s #%u", ui_cards_kind_name(hit.kind),
                     (unsigned)hit.index);
            if (voice_ble_send_undo(ui_cards_kind_name(hit.kind), hit.rev, hit.index) == ESP_OK) {
                haptic_pattern(1, 30, 0);
            } else {
                play_tone(AUDIO_TONE_CANCEL);
            }
        } else if (action == UI_CARDS_CLOSE) {
            /* Von Heute nach rechts oder nach unten: zurueck aufs Zifferblatt. */
            ui_status_show_home();
        } else if (action == UI_CARDS_OPTIONS) {
            /*
             * input_select (Flag `s`, 0.4.5): die Bruecke nach den Optionen
             * fragen und die Auswahl gleich oeffnen ("Laedt ..."). Ohne
             * Bruecke kommt nichts -- dann bleibt die Karte, mit Fehlerton.
             */
            if (voice_ble_send_options_request(ui_cards_kind_name(hit.kind), hit.rev,
                                               hit.index) == ESP_OK) {
                note_ble_transfer();
                ui_options_open(hit.kind, hit.rev, hit.index, hit.text);
                haptic_pattern(1, 20, 0);
            } else {
                play_tone(AUDIO_TONE_CANCEL);
            }
        } else if (action == UI_CARDS_SELECT) {
            /* Heute: Zeile markiert, OK-Chip steht -- ein kurzer Stoss sagt das. */
            haptic_pattern(1, 20, 0);
        } else if (action == UI_CARDS_END) {
            /* Rechts von Wetter kommt nichts mehr: ein kurzer Stoss sagt das. */
            haptic_pattern(1, 15, 0);
        }
        return;
    }
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_TAP: {
        char choice[UI_STATUS_CHOICE_LEN];
        const int index = ui_status_choice_at(event->touch_x, event->touch_y,
                                              choice, sizeof(choice));
        if (index < 0) {
            if (handle_history_chip(event->touch_x, event->touch_y)) {
                break;
            }
            /* Kurzform <-> ganze Antwort; sonst wie die Blaettertaste. */
            if (!ui_status_toggle_more()) {
                queue_app_event(APP_EVENT_BROWSE_UP);
            }
            break;
        }
        ESP_LOGI(TAG, "Auswahl %d: %s", index, choice);
        if (voice_ble_send_choice((uint8_t)index, choice) != ESP_OK) {
            ui_status_set_not_connected();
            play_tone(AUDIO_TONE_CANCEL);
            break;
        }
        haptic_pattern(1, 30, 0);
        apply_app_ui_state("thinking", "", "");
        break;
    }
    /*
     * Eine Reihe, raeumlich gedacht: Verlauf | Zifferblatt | Heute | Einkauf
     * | Zuhause. Nach links wischen holt, was rechts liegt; nach rechts, was
     * links liegt. Im Verlauf heisst rechts also "aelter", links "neuer",
     * und links von der juengsten Antwort geht es aufs Zifferblatt zurueck.
     */
    case VOICE_BOARD_SWIPE_LEFT:
        if (ui_status_in_history()) {
            ui_status_browse_newer();
            ui_status_set_mood(UI_STATUS_MOOD_LOOK_RIGHT, 1200);
            break;
        }
        if (!cards_allowed()) {
            break;
        }
        ui_cards_show(UI_CARD_TODAY);
        note_ble_transfer();
        (void)voice_ble_send_glance_request();
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        ui_status_browse_history();
        ui_status_set_mood(UI_STATUS_MOOD_LOOK_LEFT, 1200);
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        ui_status_show_home();
        break;
    case VOICE_BOARD_SWIPE_UP:
        if (!cards_allowed()) {
            break;
        }
        /*
         * Hoch zu den Karten (Heute, Einkauf, Zuhause). Was dort steht, kam
         * mit dem letzten Abgleich; die Bitte um einen frischen kostet
         * nichts und zieht die Karte nach, sobald er da ist.
         */
        ui_cards_show(UI_CARD_TODAY);
        note_ble_transfer();
        (void)voice_ble_send_glance_request();
        break;
    default:
        break;
    }
}
#else
static void handle_touch(const app_event_t *event)
{
    (void)event;
}
#endif

static void app_event_task(void *arg)
{
    (void)arg;
    app_event_t event;
    while (true) {
        if (xQueueReceive(s_app_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case APP_EVENT_TALK_DOWN:
            ESP_LOGI(TAG, "button talk down");
            note_user_input();
#if CONFIG_VOICE_BOARD_WATCH206
            if (!s_recording && ui_status_transcript_cancellable()) {
                /*
                 * Sprechtaste kurz waehrend der Frist: Abbrechen, keine neue
                 * Aufnahme. Das Loslassen danach findet keine Aufnahme und
                 * keinen Druckzeitpunkt vor und tut nichts.
                 */
                cancel_transcript();
                break;
            }
#endif
            if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK && s_recording) {
                /*
                 * Zweiter Druck auf dieselbe Taste, waehrend die Aufnahme
                 * laeuft: Abbruch.
                 *
                 * Im Halten-Modus kann das gar nicht der Anfang eines neuen
                 * Turns sein — dafuer muesste die Taste zwischendurch los
                 * gewesen sein, und dann liefe keine Aufnahme mehr. Es ist
                 * also entweder ein verlorengegangenes Loslassen oder der
                 * ausdrueckliche Wunsch aufzuhoeren. Beides endet hier gleich,
                 * und der Ausweg liegt auf der Taste, die man ohnehin schon
                 * unter dem Finger hat. Seit die Blaettertaste waehrend der
                 * Aufnahme nichts mehr tut, ist es sogar der einzige.
                 */
                ESP_LOGI(TAG, "zweiter Druck bei laufender Aufnahme -> Abbruch");
                abort_recording();
                ui_status_set_cancelled();
                play_cancel_tone();
                break;
            }
            if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && s_recording) {
                const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
                s_primary_session_id = stop_recording();
                esp_err_t primary_up_err = voice_ble_send_button_click("primary", primary_duration_ms,
                                                                       s_primary_session_id);
                if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
                    apply_app_ui_state("ready", "", "");
                }
                s_primary_down_us = 0;
                s_primary_session_id = 0;
            } else {
                s_primary_down_us = esp_timer_get_time();
                if (s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION) {
                    ESP_LOGI(TAG, "button talk down as pending confirmation control");
                    s_primary_session_id = 0;
                    (void)voice_ble_send_button_click("primary", 0, 0);
                    break;
                }
                s_primary_session_id = start_recording();
                if (s_primary_session_id == 0) {
                    s_primary_down_us = 0;
                    if (!voice_ble_is_ready() && !s_ota_updating) {
                        /*
                         * Keine Bruecke: Bisher geschah dann sichtbar nichts
                         * -- start_recording() schrieb nur ins Log. Auf der
                         * Waveshare-Uhr sah das aus wie eine tote Taste.
                         */
                        ui_status_set_not_connected();
                        play_tone(AUDIO_TONE_CANCEL);
                    }
                    break;
                }
                esp_err_t primary_down_err = s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK
                    ? voice_ble_send_button_click("primary", 0, s_primary_session_id)
                    : voice_ble_send_button_down("primary", s_primary_session_id);
                if (s_primary_session_id != 0 && primary_down_err != ESP_OK) {
                    (void)stop_recording();
                    s_primary_session_id = 0;
                    apply_app_ui_state("ready", "", "");
                }
            }
            break;
        case APP_EVENT_TALK_UP:
            ESP_LOGI(TAG, "button talk up");
            note_activity();
            if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK) {
                break;
            }
            if (!s_recording && s_primary_session_id == 0 && s_primary_down_us == 0) {
                break;
            }
            const uint32_t primary_duration_ms = primary_hold_ms();
            if (s_recording && primary_duration_ms < PRIMARY_MIN_HOLD_MS) {
                ESP_LOGI(TAG, "Druck zu kurz (%u ms) -> verworfen",
                         (unsigned)primary_duration_ms);
                abort_recording();
                ui_status_set_press_too_short();
                play_cancel_tone();
                break;
            }
            if (s_recording) {
                s_primary_session_id = stop_recording();
            }
            esp_err_t primary_up_err = voice_ble_send_button_up("primary", primary_duration_ms,
                                                                s_primary_session_id);
            if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
                apply_app_ui_state("ready", "", "");
            }
            s_primary_down_us = 0;
            s_primary_release_us = 0;
            s_primary_session_id = 0;
            break;
        case APP_EVENT_BROWSE_DOWN:
            ESP_LOGI(TAG, "button browse down");
            note_user_input();
            s_secondary_down_us = esp_timer_get_time();
            break;
        case APP_EVENT_BROWSE_UP: {
            ESP_LOGI(TAG, "button browse up");
            note_activity();
            if (s_action_fired) {
                /* Das Loslassen nach einem langen Druck gehoert zur Aktion. */
                s_action_fired = false;
                s_secondary_down_us = 0;
                break;
            }
            const uint32_t secondary_duration_ms = elapsed_button_ms(s_secondary_down_us);
            s_secondary_down_us = 0;

            if (s_recording) {
                /*
                 * Waehrend der Aufnahme tut diese Taste nichts -- und das ist
                 * die Aenderung, um die es geht.
                 *
                 * Frueher brach sie ab. Damals sass sie an der Kante und
                 * wurde nur mit Absicht getroffen; heute ist sie die grosse
                 * Flaeche auf der Front, die am Handgelenk von selbst
                 * ausloest. Eine Taste, die versehentlich gedrueckt wird,
                 * darf keinen laufenden Satz vernichten. Der Ausweg liegt
                 * weiterhin auf der Sprechtaste selbst: ein zweiter Druck
                 * darauf bricht ab.
                 */
                ESP_LOGI(TAG, "Blaettertaste waehrend der Aufnahme -> ignoriert");
                voice_ble_send_button_click("secondary", secondary_duration_ms, 0);
                break;
            }

            /*
             * Sonst: einen Schritt zurueck durch die letzten Fragen und
             * Antworten. Der erste Druck holt die juengste zurueck -- genau
             * das, was diese Taste vorher auch tat, nur endete es dort.
             */
            ui_status_browse_history();
            voice_ble_send_button_click("secondary", secondary_duration_ms, 0);
            break;
        }
        case APP_EVENT_UI_STATE:
            apply_app_ui_state_choices(event.state, event.text, event.question,
                                       (const char (*)[UI_STATUS_CHOICE_LEN])event.choices,
                                       event.choice_count, (ui_outcome_t)event.outcome,
                                       event.ref, event.stage);
            break;
        case APP_EVENT_TOUCH:
            handle_touch(&event);
            break;
        case APP_EVENT_TRANSCRIPT:
#if CONFIG_VOICE_BOARD_WATCH206
            if (s_recording) {
                break;
            }
            /* Nichts darf die Frist verdecken: Ebenen und Taschenlampe weg. */
            ui_face_picker_hide();
            ui_options_hide();
            ui_control_hide();
            ui_cards_hide();
            if (ui_torch_on()) {
                ui_torch_set(false);
                apply_brightness_now();
            }
            note_activity();
            ui_status_set_transcript(event.text, event.size);
#endif
            break;
        case APP_EVENT_HOME_TIMEOUT:
#if CONFIG_VOICE_BOARD_WATCH206
            handle_home_timeout();
#endif
            break;
        case APP_EVENT_NIGHT_PEEK_END:
#if CONFIG_VOICE_BOARD_WATCH206
            s_night_peek = false;
            update_night_mode();
#endif
            break;
        case APP_EVENT_HISTORY_SAVE:
#if CONFIG_VOICE_BOARD_WATCH206
            ui_status_history_save();
#endif
            break;
        case APP_EVENT_BATTERY_LOG_REQUEST:
#if CONFIG_VOICE_BOARD_WATCH206
            send_battery_log(event.epoch);
#endif
            break;
        case APP_EVENT_OPTIONS_FAILED:
#if CONFIG_VOICE_BOARD_WATCH206
            /* Die Karte war inzwischen neu: die Bruecke schickt sie gleich nach. */
            play_tone(AUDIO_TONE_CANCEL);
#endif
            break;
        case APP_EVENT_BLE_SETTLE:
            if (!s_recording && !s_ota_updating && !voice_ble_ota_is_active()) {
                (void)voice_ble_request_slow_interval();
            }
            break;
        case APP_EVENT_DISPLAY_OFF:
#if CONFIG_VOICE_BOARD_WATCH206
            if (s_display_dimmed && !s_recording && !s_ota_updating && !s_night_mode) {
                set_display_off(true);
            }
#endif
            break;
        case APP_EVENT_ACTION:
            /*
             * Die Aktionstaste: was sie tut, legt die Bruecke fest (ein Satz,
             * der als Text-Turn geht). Hier nur Rueckmeldung und Weitergabe.
             */
            note_user_input();
            s_action_fired = true;
            if (s_recording) {
                break;
            }
            if (voice_ble_send_action() == ESP_OK) {
                haptic_pattern(2, 40, 60);
                apply_app_ui_state("thinking", "", "");
            } else {
                ui_status_set_not_connected();
                play_tone(AUDIO_TONE_CANCEL);
            }
            break;
        case APP_EVENT_BLE_CONNECTED:
            s_app_ui_state = APP_UI_STATE_READY;
            ui_status_set_link(true);
            ui_status_set_idle();
            note_activity();
            /* Gleich kommen device_info-Antwort, Zeit, Zifferblatt und Karten. */
            note_ble_transfer();
            break;
        case APP_EVENT_BLE_DISCONNECTED:
            s_recording = false;
            s_ota_updating = false;
            s_app_ui_state = APP_UI_STATE_READY;
            stop_host_response_timer();
            audio_pipeline_stop();
            release_recording_pm_locks();
            release_ota_pm_locks();
            stop_recording_tick();
            ui_status_set_link(false);
#if CONFIG_VOICE_BOARD_WATCH206
            ui_status_set_updating(false);
#endif
            ui_status_set_pairing(s_display_name[0] ? s_display_name
                                                    : voice_ble_device_name());
            break;
        case APP_EVENT_POWER_IRQ:
            gpio_intr_enable(VOICE_BOARD_PIN_PMIC_IRQ);
            /* fall through */
        case APP_EVENT_BATTERY_REFRESH:
            update_battery_status();
            check_event_reminder();
            break;
        case APP_EVENT_ENTER_DEEP_SLEEP:
            enter_deep_sleep();
            break;
        case APP_EVENT_OTA_BEGIN:
            s_ota_updating = true;
            if (s_recording) {
                const uint32_t session_id = stop_recording();
                voice_ble_send_button_up("primary", elapsed_button_ms(s_primary_down_us),
                                         session_id);
            }
            esp_err_t ota_pm_err = acquire_ota_pm_locks();
            if (ota_pm_err != ESP_OK) {
                ESP_LOGW(TAG, "acquire OTA pm lock failed: %s", esp_err_to_name(ota_pm_err));
            }
            note_activity();
#if CONFIG_VOICE_BOARD_WATCH206
            ui_status_set_updating(true);
#endif
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_PROGRESS:
            s_ota_updating = true;
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_DONE:
            ui_status_set_ota_rebooting();
            break;
        case APP_EVENT_OTA_END:
            s_ota_updating = false;
#if CONFIG_VOICE_BOARD_WATCH206
            ui_status_set_updating(false);
#endif
            release_ota_pm_locks();
            s_app_ui_state = APP_UI_STATE_READY;
            stop_host_response_timer();
            ui_status_set_idle();
            note_activity();
            break;
        case APP_EVENT_WRIST_RAISE:
            /*
             * Dasselbe wie ein Tastendruck, nur ohne Taste: heller Schirm,
             * Zeitgeber fuer Abdunkeln und Tiefschlaf von vorn. Kein
             * Szenenwechsel — wer aufs Handgelenk schaut, will sehen, was
             * ohnehin dasteht.
             *
             * Stillgelegt: Das Ereignis wird nur noch eingereiht, wenn
             * WRIST_WAKE_ENABLED gesetzt ist. Der Zweig bleibt stehen, damit
             * der switch alle Ereignisse abdeckt.
             */
#if WRIST_WAKE_ENABLED
            note_activity();
            voice_ble_advertise_fast();
#endif
            break;
        case APP_EVENT_TIME:
#if CONFIG_VOICE_BOARD_WATCH206
            {
                /* Springt die Uhr um mehr als fuenf Minuten, taugen die Akku-Stichproben nichts mehr. */
                const int64_t jump = event.epoch - (int64_t)time(NULL);
                if (jump > 300 || jump < -300) {
                    battery_ring_clear(&s_battery_ring);
                }
            }
#endif
            s_clock_offset_min = event.tz_offset_min;
            s_clock_valid = true;
            ui_status_set_clock(event.epoch, event.tz_offset_min);
#if CONFIG_VOICE_BOARD_WATCH206
            update_night_window();
#endif
            ESP_LOGI(TAG, "Uhr gestellt: %lld UTC, Ortszeit %+d min",
                     (long long)event.epoch, (int)event.tz_offset_min);
            /*
             * Hat das Geraet einen Uhrbaustein, bekommt er die Zeit mit --
             * dann steht sie nach einem Neustart, bevor die Bruecke sich
             * meldet. Ohne Baustein (StickS3) ist das ESP_ERR_NOT_SUPPORTED.
             */
            {
                esp_err_t rtc_err = voice_board_rtc_write(event.epoch, event.tz_offset_min);
                if (rtc_err != ESP_OK && rtc_err != ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGW(TAG, "Uhrbaustein nicht gestellt: %s", esp_err_to_name(rtc_err));
                }
            }
            break;
        case APP_EVENT_DEVICE_NAME:
            if (event.text[0]) {
                copy_utf8_clipped(s_display_name, event.text, sizeof(s_display_name));
                ESP_LOGI(TAG, "Anzeigename aus Todoteck: %s", s_display_name);
                ui_status_set_device_name(s_display_name);
            }
            break;
        case APP_EVENT_RECORDING_TICK: {
            if (!s_recording) {
                stop_recording_tick();
                break;
            }
            if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK &&
                s_primary_session_id != 0) {
                /*
                 * Zwei Takte statt einem: ein einzelner Ausreisser beim Lesen
                 * des Pins wuerde sonst mitten im Satz abschneiden. 400 ms
                 * merkt beim Loslassen niemand, ein Fehlschnitt sehr wohl.
                 */
                if (voice_board_talk_button_pressed()) {
                    s_release_ticks = 0;
                    s_primary_release_us = 0;
                } else {
                    if (s_release_ticks == 0) {
                        s_primary_release_us = esp_timer_get_time();
                    }
                    s_release_ticks++;
                }
                if (s_release_ticks >= 2) {
                    /*
                     * Halten heisst halten, zweiter Teil: Die Taste ist los,
                     * die Aufnahme laeuft aber noch — dann ist das
                     * Loslassen-Ereignis unterwegs verlorengegangen (es kommt
                     * aus einem ISR-Callback ueber eine Warteschlange mit
                     * zwoelf Plaetzen). Ohne diesen Abgleich liefe die
                     * Aufnahme bis zur 30-Sekunden-Grenze weiter, obwohl
                     * niemand mehr spricht — und genau so hat es sich am
                     * Handgelenk angefuehlt.
                     *
                     * Der Abgleich stellt kein eigenes Verhalten dar: er
                     * stellt dasselbe Ereignis nach, das gefehlt hat.
                     */
                    ESP_LOGW(TAG, "Taste ist los, Aufnahme laeuft noch -> beenden");
                    s_release_ticks = 0;
                    queue_app_event(APP_EVENT_TALK_UP);
                    break;
                }
            }

            const uint32_t elapsed_ms = elapsed_button_ms(s_recording_started_us);
            const uint8_t remaining = elapsed_ms >= RECORDING_MAX_MS
                ? 0
                : (uint8_t)(100 - (elapsed_ms * 100) / RECORDING_MAX_MS);
            ui_status_set_recording_meter(audio_pipeline_level(), remaining);
            if (elapsed_ms >= RECORDING_MAX_MS) {
                /*
                 * An der Grenze wird abgeschickt, nicht verworfen: der Server
                 * nimmt ohnehin nur 30 Sekunden an, und ein Satz, der bis
                 * dahin lief, ist mehr wert als gar nichts.
                 */
                ESP_LOGI(TAG, "Aufnahmegrenze erreicht, sende automatisch");
                const uint32_t session_id = stop_recording();
                esp_err_t limit_err = s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK
                    ? voice_ble_send_button_click("primary", elapsed_ms, session_id)
                    : voice_ble_send_button_up("primary", elapsed_ms, session_id);
                if (session_id != 0 && limit_err != ESP_OK) {
                    apply_app_ui_state("ready", "", "");
                }
                s_primary_down_us = 0;
                s_primary_session_id = 0;
            }
            break;
        }
        case APP_EVENT_HOST_RESPONSE_TIMEOUT:
            if (!s_recording && (s_app_ui_state == APP_UI_STATE_RECORDING ||
                                 s_app_ui_state == APP_UI_STATE_THINKING)) {
                ESP_LOGW(TAG, "host response timeout, returning to ready");
                apply_app_ui_state("ready", "", "");
            }
            break;
        }
    }
}

static esp_err_t init_gpio_button(gpio_num_t gpio_num, uint8_t active_level,
                                  button_handle_t *button)
{
    const button_config_t button_config = {0};
    const button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = active_level,
        .enable_power_save = true
    };

    return iot_button_new_gpio_device(&button_config, &gpio_config, button);
}

static esp_err_t init_buttons(void)
{
    s_app_event_queue = xQueueCreate(12, sizeof(app_event_t));
    if (!s_app_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_gpio_button(VOICE_BOARD_PIN_BUTTON_TALK,
                                     VOICE_BOARD_BUTTON_TALK_ACTIVE_LEVEL, &s_talk_button);
    if (err != ESP_OK) {
        return err;
    }
    err = init_gpio_button(VOICE_BOARD_PIN_BUTTON_BROWSE,
                           VOICE_BOARD_BUTTON_BROWSE_ACTIVE_LEVEL, &s_browse_button);
    if (err != ESP_OK) {
        return err;
    }

    err = iot_button_register_cb(s_talk_button, BUTTON_PRESS_DOWN, NULL,
                                 talk_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_talk_button, BUTTON_PRESS_UP, NULL,
                                 talk_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_browse_button, BUTTON_PRESS_DOWN, NULL,
                                 browse_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_browse_button, BUTTON_PRESS_UP, NULL,
                                 browse_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_VOICE_BOARD_WATCH206
    /*
     * Aktionstaste: PWR lang (Voreinstellung der Komponente, 1,5 s). Kuerzer
     * als die sechs Sekunden, nach denen der AXP2101 hart ausschaltet.
     */
    err = iot_button_register_cb(s_browse_button, BUTTON_LONG_PRESS_START, NULL,
                                 browse_button_long_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
#endif

    BaseType_t ok = xTaskCreate(app_event_task, "app_event_task", 4096,
                                NULL, 6, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void display_dim_timer_cb(void *arg)
{
    (void)arg;

#if CONFIG_VOICE_BOARD_WATCH206
    /* Nachts am Kabel steht die Helligkeit fest bei NIGHT_BRIGHTNESS; "gedimmt" waere heller. */
    if (s_night_mode) {
        return;
    }
#endif
    if (!s_display_dimmed && !s_recording && !s_ota_updating) {
        esp_err_t err = ui_status_set_brightness(DISPLAY_DIM_BRIGHTNESS);
        if (err == ESP_OK) {
            s_display_dimmed = true;
            ui_status_set_idle_dimmed(true);
            set_display_apb_lock(false);
#if WRIST_WAKE_ENABLED
            /* Ab jetzt lohnt die Geste: Es gibt etwas aufzuwecken. */
            wrist_wake_watch(wrist_wake_allowed());
#endif
#if CONFIG_VOICE_BOARD_WATCH206
            if (s_display_off_timer) {
                (void)esp_timer_stop(s_display_off_timer);
                (void)esp_timer_start_once(s_display_off_timer, DISPLAY_OFF_TIMEOUT_US);
            }
#endif
            ESP_LOGI(TAG, "display dimmed after inactivity");
        } else {
            ESP_LOGW(TAG, "dim display failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t init_display_dim_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = display_dim_timer_cb,
        .name = "display_dim",
    };
    return esp_timer_create(&timer_args, &s_display_dim_timer);
}

#if CONFIG_VOICE_BOARD_WATCH206
static void display_off_timer_cb(void *arg)
{
    (void)arg;
    /* Ueber die Warteschlange: Sleep-In nimmt die LVGL-Sperre, nichts fuer den Zeitgeber-Strang. */
    queue_app_event(APP_EVENT_DISPLAY_OFF);
}

static esp_err_t init_display_off_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = display_off_timer_cb,
        .name = "display_off",
    };
    return esp_timer_create(&timer_args, &s_display_off_timer);
}
#endif

static void deep_sleep_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_ENTER_DEEP_SLEEP);
}

static void host_response_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_HOST_RESPONSE_TIMEOUT);
}

static esp_err_t init_deep_sleep_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = deep_sleep_timer_cb,
        .name = "deep_sleep",
    };
    return esp_timer_create(&timer_args, &s_deep_sleep_timer);
}

static void recording_tick_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_RECORDING_TICK);
}

static esp_err_t init_recording_tick_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = recording_tick_timer_cb,
        .name = "recording_tick",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_recording_tick_timer);
}

#if CONFIG_VOICE_BOARD_WATCH206
static void home_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_HOME_TIMEOUT);
}

static esp_err_t init_home_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = home_timer_cb,
        .name = "home",
    };
    return esp_timer_create(&timer_args, &s_home_timer);
}
#endif

static void ble_settle_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_BLE_SETTLE);
}

static esp_err_t init_ble_settle_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = ble_settle_timer_cb,
        .name = "ble_settle",
    };
    return esp_timer_create(&timer_args, &s_ble_settle_timer);
}

static esp_err_t init_host_response_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = host_response_timer_cb,
        .name = "host_response",
    };
    return esp_timer_create(&timer_args, &s_host_response_timer);
}

static void battery_refresh_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_BATTERY_REFRESH);
}

static esp_err_t init_battery_refresh_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = battery_refresh_timer_cb,
        .name = "battery_refresh",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_battery_refresh_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_battery_refresh_timer, BATTERY_REFRESH_FALLBACK_US);
}

#if !CONFIG_VOICE_BOARD_WATCH206
static void IRAM_ATTR pmic_irq_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(VOICE_BOARD_PIN_PMIC_IRQ);

    BaseType_t high_task_woken = pdFALSE;
    queue_app_event_from_isr(APP_EVENT_POWER_IRQ, &high_task_woken);
    if (high_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t init_pmic_irq(void)
{
    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << VOICE_BOARD_PIN_PMIC_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&irq_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_wakeup_enable(VOICE_BOARD_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(VOICE_BOARD_PIN_PMIC_IRQ, pmic_irq_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_intr_type(VOICE_BOARD_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_intr_enable(VOICE_BOARD_PIN_PMIC_IRQ);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}
#else
/*
 * Die Uhr hat keine Leitung vom AXP2101 zum Chip (VOICE_BOARD_PIN_PMIC_IRQ
 * ist NC). Netzteil ein/aus merkt sie deshalb am Akku-Zeitgeber, spaetestens
 * nach BATTERY_REFRESH_FALLBACK_MS -- fuer "nicht schlafen, solange er laedt"
 * reicht das.
 */
static esp_err_t init_pmic_irq(void)
{
    return ESP_OK;
}
#endif

static void update_battery_status(void)
{
    uint8_t sys_status = 0;
    esp_err_t irq_err = voice_board_clear_power_irqs(&sys_status);
    if (irq_err == ESP_OK && sys_status) {
        ESP_LOGI(TAG, "PMIC sys irq=0x%02x", sys_status);
    }

    int level = 0;
    bool charging = false;
    bool usb_powered = false;
    esp_err_t err = voice_board_battery_level(&level);
    if (err == ESP_OK) {
        err = voice_board_battery_charging(&charging);
    }
    if (err == ESP_OK) {
        err = voice_board_usb_powered(&usb_powered);
    }
    if (err == ESP_OK) {
        const bool external_power_changed = (charging != s_battery_charging) ||
                                            (usb_powered != s_usb_powered);
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        s_battery_percent = level;
        ui_status_set_battery(level, charging, usb_powered);
#if CONFIG_VOICE_BOARD_WATCH206
        update_battery_estimate(level, charging || usb_powered);
        battery_log_sample(level, charging, usb_powered);
        update_night_mode();
        update_night_window();
        if (charging || usb_powered) {
            s_saver_auto_done = false;
        } else if (!s_saver && !s_saver_auto_done && level <= SAVER_AUTO_PERCENT) {
            s_saver = true;
            s_saver_auto_done = true;
            ESP_LOGI(TAG, "Akku %d %% -> Energiesparen an", level);
            apply_brightness_now();
        }
#endif
        if (external_power_changed) {
            ESP_LOGI(TAG, "power source changed charging=%d usb=%d",
                     charging, usb_powered);
            restart_deep_sleep_timer();
            if (s_pm_ready) {
                apply_light_sleep();
            }
        }
    } else {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
    }
}

/*
 * Klartext zum Neustart-Grund fuers Boot-Log. Nach einem OTA-Rueckfall ist
 * das die einzige Spur der abgestuerzten Fassung: Die alte Fassung startet
 * und nennt hier, woran die neue gescheitert ist (Panic, Brownout, Watchdog).
 */
static const char *reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:    return "Einschalten";
    case ESP_RST_EXT:        return "externer Reset";
    case ESP_RST_SW:         return "Software-Neustart";
    case ESP_RST_PANIC:      return "Panic (Absturz)";
    case ESP_RST_INT_WDT:    return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT:   return "Task-Watchdog";
    case ESP_RST_WDT:        return "Watchdog";
    case ESP_RST_DEEPSLEEP:  return "Tiefschlaf";
    case ESP_RST_BROWNOUT:   return "Brownout (Spannung)";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "eFuse-Fehler";
    case ESP_RST_PWR_GLITCH: return "Spannungseinbruch";
    case ESP_RST_CPU_LOCKUP: return "CPU blockiert";
    default:                 return "unbekannt";
    }
}

void app_main(void)
{
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "boot reset_reason=%d (%s) wakeup_cause=%d ext1_status=0x%llx",
             reset_reason, reset_reason_text(reset_reason), esp_sleep_get_wakeup_cause(),
             (unsigned long long)esp_sleep_get_ext1_wakeup_status());

    ESP_ERROR_CHECK(init_power_management());
    ESP_ERROR_CHECK(voice_board_init());
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_display_dim_timer());
    ESP_ERROR_CHECK(init_deep_sleep_timer());
    ESP_ERROR_CHECK(init_host_response_timer());
    ESP_ERROR_CHECK(init_ble_settle_timer());
    ESP_ERROR_CHECK(init_recording_tick_timer());
#if CONFIG_VOICE_BOARD_WATCH206
    ESP_ERROR_CHECK(init_home_timer());
    ESP_ERROR_CHECK(init_display_off_timer());
    /* Aus dem Tiefschlaf zurueck: die Einstellungen aus der App stehen im RTC-Speicher. */
    apply_watch_settings_ui();
    /* ui_status_init() hat den NVS schon aufgesetzt (Verlauf). */
    battery_log_load();
    ui_status_set_history_save_cb(history_save_cb);
#endif
    ESP_ERROR_CHECK(audio_tone_init());
#if WRIST_WAKE_ENABLED
    /* Ohne Sensor faellt nur die Geste weg — der Rueckgabewert ist deshalb kein Abbruchgrund. */
    (void)wrist_wake_init(wrist_raise_cb);
#endif
    if (s_clock_valid) {
        /* Aus dem Tiefschlaf zurueck: Zeit lief weiter, der Abstand kommt aus dem RTC-Speicher. */
        ui_status_restore_clock(s_clock_offset_min);
    } else {
        /*
         * Kaltstart. Mit Uhrbaustein (Waveshare-Uhr) steht die Zeit trotzdem
         * -- er laeuft am Akku weiter. ui_status_set_clock() stellt dabei
         * auch die Systemzeit, genau wie beim `time`-Ereignis der Bruecke.
         */
        int64_t rtc_epoch = 0;
        int32_t rtc_offset_min = 0;
        if (voice_board_rtc_read(&rtc_epoch, &rtc_offset_min) == ESP_OK) {
            s_clock_offset_min = rtc_offset_min;
            s_clock_valid = true;
            ui_status_set_clock(rtc_epoch, rtc_offset_min);
            ESP_LOGI(TAG, "Uhr aus dem Uhrbaustein: %lld UTC, Ortszeit %+d min",
                     (long long)rtc_epoch, (int)rtc_offset_min);
        }
    }
    /* Der Schirm ist nach dem Start hell: 80 MHz halten, bis er dimmt. */
    set_display_apb_lock(true);
    note_activity();
    voice_ble_set_connection_callback(ble_connection_cb);
    voice_ble_set_control_callback(ble_control_cb);
    voice_ble_set_speaker_callback(ble_speaker_cb);
    voice_ble_set_ota_callback(ble_ota_cb);
    ESP_ERROR_CHECK(init_buttons());
#if CONFIG_VOICE_BOARD_WATCH206
    {
        /* Ohne Touch bleiben beide Tasten -- kein Grund, nicht zu starten. */
        esp_err_t touch_err = voice_board_touch_init(screen_touch_cb);
        if (touch_err != ESP_OK) {
            ESP_LOGW(TAG, "Touch nicht bereit: %s", esp_err_to_name(touch_err));
        }
    }
#endif

    /*
     * Diagnose vor dem teuersten Schritt: NimBLE braucht beim Start grosse
     * zusammenhaengende Bloecke im internen RAM. Stuerzt eine Fassung hier
     * ab, zeigt die Zeile davor, wie knapp es war.
     */
    ESP_LOGI(TAG, "internes RAM vor Bluetooth: frei %u, groesster Block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_err_t err = voice_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        ui_status_set_error("Bluetooth nicht bereit");
    } else {
        ui_status_set_device_name(voice_ble_device_name());
        /*
         * Bis hierher gekommen: Anzeige, Board und Bluetooth laufen -- die
         * Fassung gilt als gut. Ohne diesen Aufruf faellt ein Bootloader mit
         * Rueckfall (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) beim naechsten
         * Neustart auf die vorige Fassung zurueck. Ein Abbild, das vorher
         * abstuerzt, erreicht diese Zeile nie -- genau dann soll er das.
         */
        const esp_err_t valid_err = esp_ota_mark_app_valid_cancel_rollback();
        if (valid_err != ESP_OK && valid_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "OTA-Fassung nicht bestaetigt: %s", esp_err_to_name(valid_err));
        }
    }

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        ui_status_set_error("Mikrofon nicht bereit");
    }

    if (err == ESP_OK) {
        ui_status_set_pairing(voice_ble_device_name());
    }
    ESP_LOGI(TAG, "Voice Stick booted");

    update_battery_status();
    ESP_ERROR_CHECK(init_battery_refresh_timer());
    ESP_ERROR_CHECK(init_pmic_irq());

    ESP_LOGI(TAG, "configuring PMIC");
    s_pm_ready = true;
    apply_light_sleep();
}
