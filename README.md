# Voice Stick — Todoteck Fork

Fork of [78/voicestick](https://github.com/78/voicestick) with the `upstream`
remote kept, so upstream work on audio, display and power management can still
be merged in.

Upstream turns an M5Stack StickS3 into a Bluetooth push-to-talk input device for
a desktop app that pastes recognized text into the focused input field. This
fork keeps that firmware and protocol, and changes what happens **on the
device**: the screen speaks German, the speaker acknowledges what the stick is
doing, and the answer sent back by the host is rendered instead of discarded.

The host side for this fork is **not** the desktop app in this repository. It is
the Todoteck bridge (`scripts/voice-bridge/` for the PC, plus the Android
foreground service) in the [Todoteck repository](https://github.com/Fauteck/todo),
which uploads the audio to `POST /api/voice/turn` and writes the answer back into
the `ui_state` control event. The macOS, Windows and Linux apps in `desktop/` are
upstream code and untouched by this fork — see
[Upstream Desktop Apps](#upstream-desktop-apps).

> **Status** — what is merged, built and flashed onto which device — is not kept
> in this README. It lives in git and in Todoteck (project *Home Lab*).

## What This Fork Changes

| Change | Why |
| --- | --- |
| German interface with Latin-1 fonts (`todoteck_font_10` … `todoteck_font_16`, generated from Montserrat with `lv_font_conv`) | LVGL's built-in Montserrat carries ASCII only, so umlauts and ß would render as gaps |
| Answer text on the display | The `ui_state` control event already carried a `text` field; the stock firmware used it only to pick fixed English hints |
| Acknowledgement tones (`audio_tone` component): one on record start, two rising for an answer, two falling for an error, one short low one for a discard | On the wrist you cannot see the display while speaking |
| Level meter during recording | Peak per frame, square-root compressed (raw speech sits near ten percent of full scale and would be invisible as a bar), with decay against flicker |
| Remaining-time bar up to the 30 s limit | The limit exists anyway; at the limit the take is **sent**, not discarded — a sentence that ran that far beats nothing |
| The buttons swapped roles: the **side** button (bottom edge in landscape) talks, the big blue **front** button browses | The front button is where the wrist meets a desk edge, a door frame or a sleeve, and it started recordings nobody wanted. The smaller edge button is only ever hit with a finger. Nothing changes on the wire — the protocol has always spoken in roles (`primary` talks, `secondary` browses) and only the pin behind each role moved |
| Abort has one way out, not two: a second press on the **talk** button. The browse button does nothing while recording | Tearing the capture path down always emits a normal END audio frame, so the abort needs its own state event, `recording_discarded`. What went is the abort on the *other* button: it made sense while that button sat on the edge and was only ever pressed deliberately. On the front face it is the button that fires by accident, and a button that fires by accident must not be able to destroy a sentence in progress |
| Hold really means hold: a press shorter than 500 ms is discarded on the device, screen shows `Zu kurz` | A tap started a recording exactly like a hold, so whoever started speaking afterwards spoke into a recording that had already ended. The bridge dropped those takes silently (`MIN_TURN_MILLIS`); the device now says so instead |
| A turn that nobody spoke into gets its own state (`no_speech`, screen `Nichts gehört`, the short low discard tone) instead of an error | Whisper cannot report silence — it must emit tokens, and on German silence it falls back to the subtitle credit from its training videos. A button that fires in a pocket therefore came back as an ordinary answer: the screen showed a failed wiki search for „Untertitelung des ZDF, 2020", a sentence nobody had said. The Todoteck server now detects the case and names it; the device says so, and the tone says „discarded" rather than „broken" |
| The recording never outlives the press: every 200 ms tick compares the GPIO against the recording state | The release arrives from an ISR callback through a twelve-slot queue. If it is lost, the take used to run to the 30 s limit with nobody speaking. The tick replays the missing event; two consecutive ticks are required so a single misread cannot cut a sentence |
| Long text pushes Tecki off the screen instead of running through him | An answer like "Aufgabe angelegt: AliExpress Bestellung · Fällig: Di., 25.08.2026 · Projekt: Home Lab" used to grow upwards from the bottom edge across the status line and straight through the figure. The decision is made on the measured text, not on the scene: if it fits into the column beside Tecki nothing changes, if it does not, Tecki steps aside and the text gets the whole width in the largest font that fits |
| Landscape instead of portrait: the panel is rotated in the controller (`swap_xy`), Tecki moves into a 64 px column on the left, status line and text into a 150 px column beside him. `LVGL_DRAW_BUF_LINES` drops from 24 to 10 with it | The same answer needs seven lines on 119 px of width and three on 224 — it therefore lands one or two font steps larger. On a wrist, text along the arm also reads more naturally. The rotation is free in CPU terms — the ST7789 does it in hardware — but not in RAM: a draw buffer holds `width * lines` pixels, so the wider screen doubled the two buffers from 12,960 to 23,040 bytes of DMA-capable internal memory, and that is exactly the pool the recording path takes its 32 KB task stack and I2S buffers from. The device answered every button press with `Aufnahme startet nicht: ESP_ERR_NO_MEM` until the line count came down |
| A clock on the pairing, ready and resting screens (`time` control event) | The stick is worn on a wrist. Between two sentences it showed a mascot and a hint — a watch face costs nothing there and answers the question a wrist device is asked most often. `pairing` keeps its status word — that no bridge is there is visible nowhere else — and takes Montserrat 28 beside it. The board has no RTC and no Wi-Fi, so the time comes from a bridge; until one has sent it, the face reads `--:--`. Showing dashes rather than nothing is deliberate: without them a device whose bridge never sends the time looks exactly like one running older firmware, which is the confusion the first run on hardware produced |
| `ready` and `resting` are a watch face and nothing else: no status word, no hint, Tecki shrunk to 28 px in the bottom left corner, the time across the full width in `todoteck_clock_64` | This is the screen the device shows almost all of the time, so everything on it that is not the time is in the way. `Halten zum Sprechen` dates from a firmware whose ready screen had nothing else to show; on a wrist you read it twice and never again, and which button talks is answered by the first press. The clock font carries twelve characters — digits, colon, hyphen — which is why its line box is 47 px rather than the 57 a full Latin set needs at that size: the digits themselves grow from 35 px to 47. The widest possible time (`04:44`) measures 189 of the 224 available pixels. The built-in Montserrat 48 it replaces is gone from `sdkconfig.defaults` with it |
| Header carries only the link dot and the battery percentage | The battery drawing said the same as the number beside it, only less precisely, and the device name repeated what the pairing screen already shows as its hint. On a watch face every pixel that is not needed is in the way. The percentage now carries the state as colour as well: red below 20 %, blue on power |
| Raising the wrist wakes the dimmed screen (BMI270, polled at 10 Hz while dimmed) — **built, then switched off** (`WRIST_WAKE_ENABLED 0` in `firmware/main/main.c`) | The idea: the screen dims after 15 s, and getting the time back meant pressing the side button — with the other hand, which is the movement a watch exists to avoid. The gesture is movement, then stillness, then a tilt of at least 20° against the pose before it; all three, because otherwise every step while walking wakes the screen. It is off all the same: only a button press is meant to bring the dimmed screen back, so the sensor is never set up and never polled. The component stays in the tree — the flag is the whole switch. Deep sleep always needed the button anyway: M5 documents the IMU interrupt as `G4 (PYG4_IMU_INT via M5PM1)`, and whether that reaches a wake-capable pin is a measurement nobody has taken |
| The answer is set in the largest of six sizes (<!-- doku-vertrag:schriftstufen -->10, 11, 12, 13, 14, 16 px<!-- /doku-vertrag -->) that still fits the free area, each with its own line spacing | With only 10 px and 16 px to choose from, anything longer than a line landed in the smallest one — a two-sentence answer filled two thirds of the screen and left the rest white while being barely readable. The steps in between turn that white space into type size |
| Characters the fonts lack are substituted or dropped, not drawn as boxes | Bridge answers carry emoji (check mark, calendar, folder) and typographic punctuation. The emoji showed as empty rectangles mid-sentence and are removed together with the gap around them; dashes, curly quotes and ellipses are replaced with their ASCII equivalents, because dropping them would turn "Aufgabe – heute" into "Aufgabe heute" |
| The browse button steps back through the last five questions and answers, one per press, and lands back on the watch face after the oldest | The screen dims after 15 s and sleeps after 5 minutes; whoever looks later never read the answer. Recalling it costs nothing, asking again costs transcription, intent recognition and possibly a second task. Five, because the ring sits in internal RAM at 256 bytes an entry — anyone who needs to go further back looks in the app, where the history is complete anyway. The ring has an exit rather than a wrap: after the oldest entry the watch face returns, so the way back to the time is always one more press, and `2/5` in the header says how many that is (the watch shows a row of dots instead) |
| The question is shown above the answer, as soon as the answer arrives — not only in the history (`question` field on the `ui_state` control event) | A misheard question is worth seeing while you can still say it again. It travels in the same write as the answer, so the two cannot drift apart, and a bridge that does not send one leaves the screen as it was: the status word `Bereit` above the answer |
| It gets its own form: `todoteck_font_11`, two lines, muted, in quotes — not the semibold 16 the status word uses | Measured, not guessed: sixteen characters of the 16 px face fit the 150 px column, so `Leg eine Aufgabe an, Pool rückspülen` came out as `Leg eine Aufgabe...` — worthless as the headline of a history entry. Two lines of the 11 px face hold around sixty, which is what the bridge's 64-byte budget sends. The quotes are what marks it as a quote: `Pool rückspülen am Samstag` over `Aufgabe angelegt: …` would otherwise leave open which of the two sentences came from the device |
| Connection dot carries the real link state (filled green connected, empty ring disconnected) | It used to be derived from the scene, so a bridge dropping mid-screen was only noticed at the next attempt |
| `device_name` control event replaces the advertised `VS-XXXX` on the pairing screen | With two sticks, the Todoteck name is the only way to tell which one is in your hand. It sits where it is needed — on `pairing`, as its hint — and no longer in the header of every screen |
| Tecki instead of the upstream cats: the Todoteck mark itself, given eyes, drawn from LVGL primitives | The cats said nothing about Todoteck. Drawing the mascot from shapes instead of six 112×112 ARGB8888 blobs frees **294 KB of flash** and makes the figure editable without re-rendering and committing images |
| Firmware builds in GitHub Actions (`.github/workflows/firmware.yml`) | Nobody needs a local ESP-IDF toolchain to get a flashable image |

Both protocol additions are optional in both directions: a bridge that does not
know `recording_discarded` simply uploads the aborted take, and firmware that
does not know `device_name` ignores it.

### The Mascot

Tecki is not a second drawing — he is the Todoteck mark plus a face layer
computed from it. The geometry is normative and lives in the
[Todoteck repository](https://github.com/Fauteck/todo/blob/main/docs/maskottchen.md);
`ui_status_icons.c` derives everything from a single constant, `MARK_WIDTH`.

Two things about him are decided here rather than there:

- **Two greens.** `ui_status.c` paints a cream ground (`0xfff7ed`) in every
  scene but `Ruht`, which goes dark (`0x1b2430`). The brand green `0x2ecc71`
  reaches only 1.98:1 on cream — below the 3:1 WCAG asks for graphical
  objects — so the light scenes use `0x1e9e56` (3.25:1) and only the resting
  scene keeps `0x2ecc71` (7.45:1).
- **101.5, not 112.** Three scenes tilt the figure around its bend, and
  rotating about a point that is not the centre moves the bounding box. 101.5
  is the widest the mark can be while all six poses stay inside the 112 px
  square; wider and the tip in `Fehler` leaves the frame on the right.

What he deliberately does not do: no blinking (the battery is small), no sound waves while
recording (the level meter and the time bar are already there), no warning
sign on error (the body turns red and the status line says so). A third signal
for the same thing is noise.

There is no speaking pose, because this firmware has no speaking state — the
answer is shown as `Bereit` with text. That pose exists in the Todoteck
repository and is waiting for text-to-speech.

The three helper scripts in `scripts/` that built the cat sprites
(`slice_cat_sprites.py`, `tune_cat_sprites.py`, `png_to_lvgl_argb_bin.py`)
have no consumer any more. They are left in place on purpose: they are
upstream tooling, and deleting them would only add conflicts the next time
`upstream` is merged.

## Project Layout

- `firmware/`: ESP-IDF firmware for M5Stack StickS3 and the Waveshare ESP32-S3-Touch-AMOLED-2.06 (one source tree, two build targets).
- `firmware/components/voice_board/`: board layer — one header for both devices, the watch driver (AXP2101, PCF85063, FT3168, motor). The StickS3 calls pass through to the untouched `stick_s3_board`.
- `firmware/components/ui_status/`: display, fonts, level and time bars, link dot.
- `firmware/components/audio_tone/`: acknowledgement tones over the ES8311.
- `desktop/macos/`: Swift Package for the native macOS menu bar app (upstream).
- `desktop/windows/`, `desktop/linux/`: upstream desktop workspaces.
- `docs/protocol.md`: BLE protocol between StickS3 and host, including the fork additions.
- `docs/volcengine-asr.md`: trimmed Volcengine ASR notes used by the upstream desktop client.
- `docs/release.md`: upstream release process (not used by this fork).
- `scripts/`: upstream packaging and release helpers, the three cat-sprite tools that lost their consumer with Tecki, and `check-docs.py`, which checks this documentation against the code.

## Documentation Contracts

Parts of this README and of `docs/protocol.md` restate sets that live in the
code — UUIDs, the states the firmware announces, dependency names. Those parts
are marked:

```text
<!-- doku-vertrag:name --> ... <!-- /doku-vertrag -->
```

`scripts/check-docs.py` compares every marked region against the source and
fails if they have drifted apart; `.github/workflows/doku.yml` runs it on each
push and pull request touching the docs or the firmware. Run it yourself with
`python3 scripts/check-docs.py` — it needs nothing but Python, and the script
itself is the list of what is under contract.

Wording around a marked region can be rewritten freely: the contract holds the
claim, not the sentence. A missing marker is a failure too, so a contract
cannot be dropped by deleting a comment. Everything outside the markers is
prose, examples and reasoning, and is deliberately not checked — those cannot
go stale without someone reading them.

## Firmware Features

- StickS3 advertises as `VS-XXXX`, where `XXXX` is derived from the last two bytes of the eFuse MAC. A `device_name` control event replaces that name in the header.
- The front button maps to the protocol `primary` role; it starts a recording session on press and ends it on release once the host has put the device in `ready`. A press shorter than 500 ms is discarded (`Zu kurz`), and a press while a recording is already running aborts it — both emit `recording_discarded`.
- The firmware reads 16 kHz mono PCM from the ES8311 microphone, encodes it as Opus, and sends it over BLE notifications.
- Recording is capped at 30 seconds. A bar counts the remaining time down; at zero the firmware sends the take like a normal button release.
- The side button aborts a running recording, emits `recording_discarded`, shows `Abgebrochen` and plays the discard tone.
- The screen shows pairing, ready, listening (`Höre zu`), thinking (`Verstehe …` / `Erledige …` with the bridge's `stage` since 0.4.4, `Denkt nach` without; a seconds counter from 5 s), pending confirmation, error and battery states from host-sent `ui_state` updates. Text arriving with `ready` is rendered as the answer and kept for recall (up to 192 bytes).
- Text that does not fit below Tecki takes the whole screen and Tecki is hidden for as long as it is shown; the font is the largest size that fits, and anything beyond even the smallest ends in an ellipsis. Characters outside the fonts' Latin-1 range are replaced with ASCII where an equivalent exists and dropped otherwise.
- The screen dims after 15 seconds of inactivity. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. Either button wakes it from deep sleep — a button that does nothing while the device sleeps reads as a broken button.
- BLE OTA updates over the two 3 MB app slots of the OTA partition table, with rollback: a new image boots on probation and confirms itself once Bluetooth is up; if it crashes before that, the bootloader returns to the previous one (needs one USB flash of `merged*.bin` to install a bootloader with rollback enabled).

Tones share the I2S lines with the microphone: `audio_tone_play()` stays silent
while the capture path is up. That is why the start tone is played *before*
`audio_pipeline_start()` — which makes it the "speak now" cue at the same time.

## Hardware Target

- Board: M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- Side button (bottom edge in landscape): GPIO12, protocol `primary`, push-to-talk, abort on a second press
- Front button (the large blue one): GPIO11, protocol `secondary`, browses the history, ignored while recording
- Either button wakes the device from deep sleep
- IMU: BMI270 on the shared I2C bus (0x68), wired up for the wrist-raise gesture but not set up at runtime (`WRIST_WAKE_ENABLED 0`); its interrupt line (documented as G4) is unverified and unused
- PMIC IRQ: GPIO13
- Audio codec: ES8311 over I2S, 16 kHz / 16 bit / mono, speaker on the same lines
- Display: 135 x 240 ST7789P3 panel, driven in landscape (240 x 135) via the controller's axis swap
- LCD backlight: GPIO38 PWM

Main pin definitions live in `firmware/components/stick_s3_board/include/stick_s3_board.h`.

## Second Target: Waveshare ESP32-S3-Touch-AMOLED-2.06

The same firmware also builds for the Waveshare watch board
(`-DVOICE_BOARD=watch206`). Protocol, bridge, history, clock face and Tecki are
shared; pins, drivers and layout come from the board layer. Pins and addresses
are taken from Waveshare's schematic (V1.0) and board package
(`waveshare/esp32_s3_touch_amoled_2_06` 2.0.0), **not yet verified on the
device** — the first flash is the test.

| | StickS3 | Waveshare watch |
| --- | --- | --- |
| Display | ST7789 240 x 135 LCD, landscape | CO5300 410 x 502 AMOLED over QSPI, portrait; black ground in every scene (AMOLED) |
| Layout | Tecki beside the text | Tecki above the text, same "does it fit?" rule; own Latin-1 fonts 18–30 px, clock face 120 px |
| Talk (`primary`) | side button, GPIO12 | BOOT, GPIO0 (active low) |
| Browse (`secondary`) | front button, GPIO11 | PWR via AXP2101/transistor, GPIO10 (active high); a tap on the screen does the same |
| Microphone | ES8311 | ES7210 (two mics, mic 1 used), ES8311 drives the speaker only, amplifier on GPIO46 |
| Clock | from the bridge only | PCF85063 keeps time through reboots; the bridge's `time` event sets it |
| Haptics | — | 60 ms motor pulse on record start (GPIO18; only if a motor is fitted) |
| PMIC | M5PM1 with IRQ line | AXP2101, polled — its IRQ line does not reach the chip |
| Deep-sleep wake | both buttons, ext1 low | BOOT via ext1 low, PWR via ext0 high |

Why talk sits on BOOT: the AXP2101 powers the board off when PWRON is held for
several seconds; a recording may last thirty. Holding PWR therefore stays the
hard-off escape hatch.

Each image carries its device in the project name (`voice_stick` /
`voice_watch`). BLE OTA rejects an image built for the other device
(`wrong_device`) instead of booting it with the wrong pins, and `device_info`
reports `hardware` as `stick_s3` or `watch_206`, so a bridge can pick the right
file.

Watch-only on top of that:

| Feature | How |
| --- | --- |
| Raising the wrist wakes the dimmed screen | The gesture logic from `wrist_wake` (movement, stillness, tilt ≥ 20°), fed by the QMI8658 (0x6B) instead of the StickS3's BMI270. Since 0.4.1 the sensor runs in wake-on-motion (accelerometer only, 21 Hz low power, gyroscope off) and raises INT1 on GPIO21; only then does the firmware sample for up to 2 s at ~25 Hz and decide — no 100 ms polling task, and the same line wakes the chip from light sleep. On the StickS3 it stays off |
| Haptics with every tone | One short pulse to start, two for an answer, one long for an error, one very short for a discard — also when the tone is skipped because the capture path holds the I2S lines |
| Yes/no on the screen | `choices` on `ui_state` become touch areas; a tap sends a `choice` event (docs/protocol.md) |
| Gestures | One rule set for every scene — see the gesture table below |
| Watch-face layouts (0.4.4) | Three layouts (`watch_settings.face`): **Info** — the time in cells, up to three lines, event countdown, battery, Tecki; **Minimal** — only the large time, the date ("So 1.11.") and the battery as a dot (green, yellow from 20 %, red from 10 %, blue on power); **Groß** — hours and minutes on two lines in 180 px and exactly one line below (the event countdown, otherwise the first face line). A long press on the face (0.6–2 s) opens the picker: one large preview with dots and the name, swipe left/right to page, tap to take it, swipe down or back from the left edge to cancel (also after 10 s). The watch applies it at once, keeps it in RTC memory and sends `face_layout`; the bridge stores it on the server |
| Night mode (0.4.4) | Inside the window `watch_settings.night` (default 21:00–06:30, local time, may span midnight; `""` switches it off) the watch face — all three layouts — is drawn in dimmed red (time `#b0301c`, secondary text `#6a1d10`, Tecki tinted, the battery's red stays red) and the brightness is capped at "Dunkel" in every scene; other scenes keep their colours. Raising the brightness in the control centre at night holds until the screen goes off. Checked every 10 s; night charging keeps priority but turns red inside the window |
| Watch face lines and next event | Up to three lines from the bridge (`face`) under the time, plus the next calendar event with its day always in front, in local time: "in 25 min …" within the hour, otherwise "Heute 09:00 …", "Morgen 09:00 …", "Mo 09:00 …" (a date from a week out); all-day events (starting 00:00) without a time. Long titles end in "...", day and time are never cut; ten minutes before it the watch vibrates three times |
| Cards: Today, Shopping, Home, Weather | They sit in a row to the right of the watch face: swipe left from the face for Today, again for Shopping, Home and Weather; swipe right goes back, from Today to the face. Swipe up from the face opens Today too. Long lists scroll (LVGL scroll, snapped to rows): up moves the list on by one view, down moves it back and, at the top, returns to the face; a thin bar at the bottom shows where you are. Today rows carry the project colour as a stripe, group headings ("Termine", "Angepinnt", "Überfällig", "Heute", "Außerdem wichtig") sit between the groups, and an overdue date is red. Since 0.4.4 today's calendar events come first in the group "Termine" (flag `e`): read-only — a tap does nothing — with the time on the right and a stripe in calendar blue; the card is empty only without tasks and without events. A tap checks off a shopping item or toggles a Home Assistant switch (an `input_select` shows the current option as a word — since 0.4.5 a tap opens the picker described below instead of stepping to the next option; a lock shows "Offen"/"Zu" and always asks for the OK); on Today — and on switches the app marks as "Bestätigen" — a tap only selects the row (highlighted, short buzz) and the green "OK – erledigt" / "OK – schalten" chip at its right end carries it out — another row moves the selection, a tap elsewhere, a swipe, closing, a new list from the bridge or 5 s without OK drop it and send nothing. Either way the bridge carries it out (`list`/`item`, docs/protocol.md). Dots at the top show the card; empty cards show Tecki and a sentence |
| Weather card | Page 1: now, today, rain ("Regen ab 15 Uhr", blue), six hours (rainy ones blue), sun and wind; "Stand 08:30" when the phone's snapshot is old, the first warning in red under the title (a new one buzzes once). Swipe up for "Nächste Tage" (five days) and, if there is anything, "Pollen & Warnungen". Pure display, no touch areas; "Noch kein Wetter vom Handy." until the bridge's first `weather` |
| Undo | For 5 s after a row is checked off or toggled a "Rückgängig" chip sits at the bottom of the card; a tap sends `undo` with the row's coordinates and restores the row. The action also becomes a history entry ("Erledigt: Müll rausstellen", "Licht Küche: An") whose own "Rückgängig" chip works for 15 min |
| History with time, marker and chips | Swipe right from the face: first the day's balance ("Heute · 4 Befehle · 3 erledigt · 1 Frage · 0 Fehler", counted on the watch from midnight), then the last ten entries, newest first. Each shows when ("vor 12 min", "11:46", "gestern 18:40"), a coloured marker from the bridge's `outcome` (green angelegt/erledigt, grey Antwort, red Fehler), the question and the answer, and two chips: "Nochmal" repeats the question as a text turn (`repeat`), "Rückgängig" takes the turn back for 15 min (`undo_turn` with the bridge's `ref`). The ten entries live in NVS and survive a restart (written at most every 30 s) |
| Transcript first | The bridge shows what it understood (`transcript`) for 2 s before acting; "Abbrechen", a palm or a short press on the talk button stops it (`cancel_transcript`) |
| Short answers | An answer longer than two lines shows its first sentence (at most ~90 characters) and "Tippen für mehr"; a tap shows the whole answer. Open yes/no questions are always shown in full. The history behaves the same and shows its position as dots |
| Option picker (0.4.5) | A tap on an `input_select` row on Home (flag `s` in `list`, e.g. "Büro Modus") opens a list of its options instead of stepping on: the row text as title, the options as large rows (≥ 56 px, the card font size), the active one framed in green with a green dot; more than fit scroll like the cards. The watch asks the bridge (`options_request`) and shows "Lädt …" until `options` arrives — no answer within 3 s shows "Keine Antwort" for 1.5 s and closes. A tap on another option sends `option`, buzzes, closes, the row shows "..." until the new card arrives, "Rückgängig" stands 5 s and the history keeps "Büro Modus: Gaming" (undo from there for 15 min). A tap on the active option, swipe down, back from the left edge or 10 s without input close without sending; no OK step, even for confirmed switches. docs/protocol.md |
| Settings from the app | `watch_settings`: font size in three steps (answer, question line, card rows, transcript), how long until the watch returns to the face (an unread answer stays at least 2 min), which control-centre tiles show and in which order, the watch-face layout and the night window (0.4.4) |
| Clock in the header | Small `HH:MM` at the top of every scene except the watch face (which has the big one) and the torch — also over cards and the control centre |
| Spoken answers on the watch | The bridge sends the server's TTS as 8 kHz IMA-ADPCM over `speaker_rx`; the watch buffers in PSRAM and plays through the ES8311. Stops when a recording starts |
| Control centre | Swipe down from the top edge: silent (vibration only), brightness (three steps), cinema (no wrist wake, answers don't light the screen), power saving, torch (the whole screen white until the next touch), battery — order and visibility set in the app |
| Power saving | Dimmer, dims after 6 s instead of 15, no wrist gesture. Switches itself on once per charge below 15 % |
| Battery: red from 10 %, time left, night charging | The percentage in the header and the Akku tile turn red only at 10 % and below (grey before). Every ten minutes off the cable the watch samples the level (twelve samples in RTC memory); from three samples over at least 30 min it shows "24 % · noch 9 h" on the tile and, at 20 % and below, a red "noch etwa 2 h" under the face lines. On USB between 22:00 and 06:00 the face shows only the big time in dark grey at brightness 20 — no lines, no Tecki, no vibration, no tones; a touch shows the normal face for 10 s |
| Palm on the screen | Two or more touch points for a quarter second: a running recording is discarded, overlays close, the screen goes dark |
| Action button | Hold PWR for 1.5 s: the watch sends `{"event":"action"}`, and the bridge runs the sentence configured for it as a text turn (e.g. "Was steht heute an?") |
| Tecki's moods | Looks up happily for three seconds after an answer, glances in the direction of a swipe, and sleeps on the watch face between 22:00 and 06:00 |
| Tecki as status | On the watch face and the ready scene a fixed ranking decides his pose: update running (eyes shut, dots) > no bridge (searching look) > battery ≤ 15 % without charging (tired) > mood > scene |
| Energy (0.4.1, package H) | Measure first: every 10 min a sample of time, charge and state goes into a 48 h ring in RTC memory, the bridge collects it (`battery_log`, docs/protocol.md) and the server works out the discharge rate per firmware version. Then save: the screen is bright for 15 s, dimmed for 15 s, then off (AMOLED sleep-in, LVGL refresh paused); wrist raise, tap or button bring it back, the palm switches it off at once; night charging keeps the dark time and never switches off. The motion sensor runs in wake-on-motion (accelerometer only, 21 Hz) and raises an interrupt on GPIO21 instead of being polled every 100 ms; the same line and the touch line wake the chip from light sleep. Bluetooth advertises 30 s at 60–120 ms, then at 500–1000 ms (a touch or button restarts the fast 30 s); the connection idles at 400–500 ms with slave latency 4 and 8 s supervision and goes to 15–30 ms while recording, during OTA and for 3 s around every face/list/weather transfer. The CPU idles at 40 MHz (XTAL) with light sleep, holds 80 MHz while the screen is bright and 160 MHz only while recording or updating (0.4.1 set 240 for the watch and did not boot; 0.4.2 is back on 160). Deep sleep after 5 min of inactivity on battery, woken by the buttons only; light sleep stays off on USB (the USB-serial port would drop) |

Gestures on the watch — one rule set for every scene. Horizontally everything
sits in one row: history | watch face | Today | Shopping | Home | Weather, and
a swipe to the left brings in what lies to the right (the same table heads
`handle_touch` in `firmware/main/main.c`; touch areas are listed in
`firmware/components/ui_status/ui_layout_watch206.h`):

| Gesture | Watch face | Answer / history | Cards | Control centre |
| --- | --- | --- | --- | --- |
| Swipe down from the top edge | control centre | control centre | control centre | closes |
| Swipe right from the left edge | into the history | back to the watch face | back to the watch face | back to the watch face |
| Swipe down | nothing | back to the watch face | list back by one view; at the top back to the watch face (Weather: previous page) | closes |
| Swipe up | cards | cards | list on by one view (Weather: next page) | closes |
| Swipe left | Today | newer; from the newest back to the watch face | next card; nothing after Weather | — |
| Swipe right | into the history (the balance first) | older | previous card; from Today back to the watch face | — |
| Tap | opens the history | yes/no; "Nochmal" / "Rückgängig"; short ⇄ full answer; otherwise one older | check off / toggle; on Today and confirmed switches first select, then "OK"; `input_select` (`s`): option picker (0.4.5); "Rückgängig"; events (`e`) do nothing | tile |
| Long press (0.6–2 s, since 0.4.4) | layout picker (Info, Minimal, Groß) | like a tap | like a tap | like a tap |
| Palm on the screen | everything quiet: recording discarded, layers closed, screen dark | | | |

A dimmed screen is only woken by the first touch; during a recording the
screen does nothing; during the transcript only "Abbrechen" (and a palm)
count; with the torch on, any touch switches it off. In the layout picker a
swipe left/right pages, a tap takes the layout, swipe down or back from the
left edge cancels. In the option picker (0.4.5) a tap on an option chooses it
(on the active one it only closes), up/down scroll a long list, swipe down at
the top or back from the left edge cancels (also after 10 s). Until 0.4.3 a long press on the screen was unassigned — a
touch up to 0.8 s without a swipe was a tap, anything longer counted as a
sleeve and did nothing; the long press on the browse button (action) is a
different thing and unchanged. Cards do not open while
the watch is thinking or waiting for a confirmation, and nothing opens over
the transcript.

Not yet: Wi-Fi, the second microphone, wake from deep sleep by the IMU
(its interrupt on GPIO21 wakes from light sleep since 0.4.1, but a cold boot
cannot tell a raise from a step without 300 ms of sampling, so deep sleep
keeps the buttons as its only wake source), the SD card and the 32 MB flash
(the partition table stays at 8 MB, identical to the StickS3).

## Interaction Model

| State | Talk button (side) | Browse button (front) |
| --- | --- | --- |
| Unpaired / disconnected | No recording; screen shows `VS-XXXX` | No effective action |
| Connected idle | Hold to record — a press under 500 ms is discarded with `Zu kurz` | One step back through the history; the click still reaches the host, where the upstream app restores the last input confirmation |
| Recording | Release to finish recording; press again to abort it, nothing is sent | Nothing on the device; the click still reaches the host |
| Thinking / finalizing | New recording is ignored | Cancel the in-progress recognition |
| Pending confirmation countdown | Pause auto-paste and keep pending confirmation | Cancel pending text |
| Manual pending confirmation | Confirm paste | Cancel pending text |

The firmware reports raw button facts (`button_down` / `button_up` /
`button_click` with `primary` or `secondary`). The host owns the interaction
state machine and sends `ui_state` updates back to the firmware for the screen.
The pending-confirmation rows apply to the upstream desktop apps, which paste
into the focused input field; the Todoteck bridge does not paste and does not
use them.

## Audio Path

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> host -> Ogg Opus -> ASR -> answer -> ui_state text -> display
```

Neither host decodes Opus back to PCM. The frames are wrapped into Ogg Opus and
forwarded as they are.

## BLE Protocol Summary

One GATT service with five characteristics — audio and state as notifications
from the stick, control writes and the two OTA channels in the other direction.
UUIDs, frame layouts and the full event list live in
[docs/protocol.md](docs/protocol.md) and are not repeated here.

The fork adds one event in each direction, both optional: `recording_discarded`
(stick to host, a take was thrown away on the device) and `device_name` (host to
stick, the name shown in the header). A counterpart that does not know them
keeps working.

The negotiated MTU (measured: 247) caps a control event at 244 bytes, which
leaves roughly 195 bytes of answer text after the JSON frame — about 180
characters in UTF-8, fewer with umlauts. Longer answers need a continuation
frame, which does not exist yet.

## Firmware Build in CI

`.github/workflows/firmware.yml` (`Firmware bauen`) builds the firmware with
ESP-IDF v5.5.1 on every push touching `firmware/**`, and on manual dispatch. It
fails if the image does not fit the 3 MB OTA partition, because otherwise you end
up holding a finished file the device refuses. Two artifacts:

| File | Use |
| --- | --- |
| `voice_stick.bin` | Plain app image; this is what the Todoteck bridge pushes over BLE OTA |
| `merged.bin` | Bootloader, partition table and app in one — the USB rescue path when a build breaks the radio link |
| `voice_watch.bin` | The same for the Waveshare watch |
| `merged_watch.bin` | The watch's USB image — also the way to put this firmware on a factory-fresh watch |

Rescue flashing needs esptool only, no toolchain:

```sh
python -m esptool --chip esp32s3 write_flash 0x0 merged.bin
```

### Getting the image without a GitHub login

Runs on `main` additionally attach both images, plus a small `firmware.json`
manifest (commit, build time, sizes, SHA-256), to the rolling pre-release
[`firmware-latest`](https://github.com/Fauteck/todoteck-voicestick/releases/tag/firmware-latest).
Artifacts stay for per-commit debugging, but they are a poor pickup path: an
artifact is always a ZIP, expires after 90 days, and needs a token even on a
public repository. A release asset is a single file at a fixed URL:

```sh
curl -LO https://github.com/Fauteck/todoteck-voicestick/releases/download/firmware-latest/voice_stick.bin
```

That is what `scripts/fetch-firmware.ps1` in the
[Todoteck repository](https://github.com/Fauteck/todo) uses to drop both images
into the shared Drive folder the phone flashes from — no credentials involved.

The tag is deliberately not a `v*` one: `release.yml` triggers on `push: tags:
["v*"]` and would check the tag against `VERSION`, upload to Aliyun OSS and
kick off a website deploy in the **upstream** repository.

## Firmware Build Locally

Optional — CI covers the normal case. Prepare ESP-IDF; the commands below use
the local path `~/esp/v5.5.1/esp-idf`, replace it if your checkout lives
elsewhere.

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
```

The Waveshare watch builds into its own directory with its own `sdkconfig`, so
the two targets never overwrite each other's configuration:

```sh
idf.py -B build-watch206 -DVOICE_BOARD=watch206 build
idf.py -B build-watch206 -DVOICE_BOARD=watch206 -p /dev/cu.usbmodemXXXX flash monitor
```

Before the first flash of a factory-fresh watch, back up Waveshare's demo
firmware — it is the only way back to it:

```sh
python -m esptool --chip esp32s3 read_flash 0x0 0x2000000 waveshare-factory.bin
```

If `export.sh` reports that the ESP-IDF Python virtual environment is missing, run the matching installer once:

```sh
"$HOME/esp/v5.5.1/esp-idf/install.sh" esp32s3
```

Flash and monitor:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The firmware uses an OTA partition table with two 3 MB app slots plus a reserved 1984 KB `storage` partition. Devices flashed with the old single-app table need one USB flash to install the new partition table before BLE OTA updates can be used:

```sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

Firmware dependencies are declared through the ESP-IDF component manager:

<!-- doku-vertrag:firmware-abhaengigkeiten -->
- `espressif/bmi270`
- `espressif/button`
- `espressif/esp_codec_dev`
- `espressif/esp_lcd_sh8601`
- `78/esp-opus`
- `lvgl/lvgl`
<!-- /doku-vertrag -->

## Upstream Desktop Apps

The apps under `desktop/` are upstream code. They are kept so the fork stays
mergeable, and they still work against upstream ASR — they know nothing about
Todoteck.

The macOS app is a Swift Package targeting macOS 12 or newer:

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

It is a menu bar accessory app and requests Bluetooth permission. Text insertion
uses simulated `Command-V` plus optional Return. If macOS blocks the keyboard
events, grant Accessibility permission to the running terminal or app in System
Settings.

Its config lives at `~/Library/Application Support/VoiceStick/config.toml`;
create it from `desktop/macos/Config/config.example.toml`, which documents every
field (ASR provider and keys, `paired_device_ids`, `auto_enter`,
`interaction_mode`, per-device overlay and translation settings). Do not commit
API keys.

Pairing: flash and boot the StickS3, start the app, open `Pair Device...`,
select the matching `VS-XXXX` and click `Pair`. Repeat for further devices; with
multiple IDs saved, the app ignores nearby unpaired VoiceStick devices.

The upstream release path — Sparkle/WinSparkle appcast, Aliyun OSS firmware
manifests, GitHub Pages deploy — belongs to the upstream repository and is not
used by this fork. It is described in `docs/release.md`.

## Interaction with the Todoteck Bridge

- The bridge connects to the stick over BLE, wraps the Opus frames into Ogg Opus and posts one turn to `POST /api/voice/turn` with a device token.
- The answer comes back as plain text and is sent to the stick as `ui_state` with `state: "ready"` and the text in `text` — that is what triggers the answer screen and the done tone.
- Since the firmware renders umlauts, bridges send the full text from the JSON `text` field rather than the ASCII short form from the `X-Todoteck-Text` header. That header was a workaround for firmware that could not display umlauts and stays in place for other clients.
- Concept, phases and the server side: Wiki „Sprach-Eingabegerät (M5StickS3) als vierter Eingabekanal“ (Todoteck project `llm-wiki`).

## License

This project is licensed under the [MIT License](LICENSE).
