<!-- heimat: repo — ändert sich mit: firmware/components/voice_ble/voice_ble.c, firmware/main/main.c, firmware/components/stick_s3_board/include/stick_s3_board.h -->
# Voice Stick Protocol

This document describes the protocol implemented by the firmware. It has two
hosts: the upstream desktop apps under `desktop/`, and the Todoteck bridge in
the [Todoteck repository](https://github.com/Fauteck/todo). Where a section
speaks only for one of them, it says so; the goals below are upstream's.

## Goals

- Low-latency push-to-talk audio from StickS3 to macOS.
- Opus over BLE to keep wireless bandwidth low.
- Ogg Opus forwarding from macOS to either Volcengine ASR or the VoiceStick Cloud relay.
- Final ASR text insertion into the focused macOS input field after release and confirmation.

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

<!-- doku-vertrag:gatt-dienst -->
```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```
<!-- /doku-vertrag -->

Characteristics:

<!-- doku-vertrag:gatt-merkmale -->
| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> host | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> host | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | host -> StickS3 | write without response |
| `ota_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104` | host -> StickS3 | write, write without response |
| `ota_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105` | StickS3 -> host | notify |
| `speaker_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5106` | host -> device (fork) | write without response |
<!-- /doku-vertrag -->

The desktop app scans for this service and only connects to devices whose `VS-XXXX` ID is present in the local paired-device list. Multiple paired devices may be connected at the same time; audio, state, control, and OTA handling are scoped by CoreBluetooth peripheral identity.

## Audio Frame

All multibyte fields are little-endian.

```text
struct AudioBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x01 audio
  uint16_t header_len;    // 16
  uint32_t session_id;
  uint32_t seq;
  uint8_t  flags;         // bit0=start, bit1=end
  uint8_t  reserved;      // currently 0
  uint16_t payload_len;
  uint8_t  payload[payload_len];
}
```

The payload contains one raw Opus packet when `payload_len > 0`. The firmware currently encodes 60 ms of 16 kHz mono audio per packet. When recording stops, the firmware also sends an end frame with `flags & 0x02` and an empty payload.

The macOS app wraps incoming Opus packets into an Ogg Opus stream before sending them to ASR. It does not decode Opus to PCM.

## State Event

All multibyte fields are little-endian.

```text
struct StateBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x10 state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

State events report device facts from the firmware to the app. They do not carry
business actions such as "cancel" or "confirm"; the app owns that interpretation.

Currently emitted state events:

<!-- doku-vertrag:device-info -->
```json
{"event":"device_info","hardware":"stick_s3","firmware_version":"0.4.5","ui_states":["ready","recording","thinking","pending_confirmation","error","no_speech"]}
```
<!-- /doku-vertrag -->

`device_info` has to fit into a single notification: with the usual MTU of 247
that is 244 bytes including the 4-byte frame header. Until 0.3.3 it also
carried `buttons` and `interaction_modes` and came to 252 bytes, so NimBLE
dropped it silently and the bridge never learned which device was connected.
Both fields were removed in 0.3.4; no bridge read them.

```json
{"event":"button_down","button":"primary","session_id":1234}
{"event":"button_up","button":"primary","duration_ms":620,"session_id":1234}
{"event":"button_down","button":"secondary"}
{"event":"button_up","button":"secondary","duration_ms":90}
```

Buttons are named by role instead of physical placement, and the Todoteck fork
is the reason that matters: it swapped the two pins in 08/2026 without changing
a byte on the wire. `session_id` is included when a `primary` press starts or
stops a local audio recording.

Which pin carries which role on StickS3:

<!-- doku-vertrag:tasten-rollen -->
| Role | Button | GPIO |
| --- | --- | --- |
| `primary` | side (bottom edge in landscape) | 12 |
| `secondary` | front (the large blue one) | 11 |
<!-- /doku-vertrag -->

Upstream firmware has it the other way round. A host cannot tell and does not
need to: it sees roles, never pins.

The Waveshare ESP32-S3-Touch-AMOLED-2.06 (`hardware` = `watch_206` in
`device_info`) speaks the same protocol with its own pins. Its `PWR` button
reaches the chip through the AXP2101's PWRON line and an inverting transistor,
so it reads **high** when pressed:

<!-- doku-vertrag:tasten-rollen-uhr -->
| Role | Button | GPIO | Pressed |
| --- | --- | --- | --- |
| `primary` | BOOT | 0 | low |
| `secondary` | PWR | 10 | high |
<!-- /doku-vertrag -->

`primary` sits on BOOT because the AXP2101 powers the board off when PWRON is
held for several seconds, and a recording may last thirty. A tap on the touch
screen sends the same `secondary` click as the PWR button.

### Fork addition: `recording_discarded`

The Todoteck fork emits one further state event when a recording is thrown away
on the device:

```json
{"event":"recording_discarded","session_id":1234}
```

It is a device fact, not an app action: tearing the capture path down always
produces a normal END audio frame, so without this event a discarded take is
indistinguishable from a completed utterance and the bridge would upload it.
The event is sent before the END frame. A bridge that does not know it keeps
working — it simply uploads the discarded take, exactly as before.

Three situations produce it, and the app does not need to tell them apart:

| Situation | Screen |
| --- | --- |
| The `primary` button is pressed *again* while recording | `Abgebrochen` |
| The `primary` button was held for less than 500 ms | `Zu kurz` |

A `secondary` press while recording used to be a third situation. It is not any
more: since that role moved to the large front button, it is the one that fires
by accident on a wrist, and an accidental press must not be able to destroy a
sentence in progress. The abort now lives only on the button that is already
under the finger.

The 500 ms match `MIN_TURN_MILLIS` in the Todoteck bridge, which dropped such
takes anyway — silently, and only after they had been transferred.

This is deliberately *not* called `cancel`; see the deprecation table below.

Deprecated firmware-to-app events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `press_start` | `button_down` with `button:"primary"` | The old name assumed the front button and implied recording semantics. |
| `press_end` | `button_up` with `button:"primary"` | The old name implied recording semantics and did not include a button role. |
| `cancel` | `button_down` / `button_up` with `button:"secondary"` | The old event encoded app meaning; the same button can cancel, restore, or be ignored depending on app state. |

## Control Event

The Mac writes compact JSON to `control_rx`. Control events are authoritative UI
state from the app to the firmware display.

Current desktop events:

```json
{"event":"ui_state","state":"ready","text":""}
{"event":"ui_state","state":"recording","text":""}
{"event":"ui_state","state":"thinking","text":"partial text"}
{"event":"ui_state","state":"pending_confirmation","text":"final text"}
{"event":"ui_state","state":"error","text":"ASR timeout"}
{"event":"interaction_mode","mode":"hold_to_talk"}
{"event":"interaction_mode","mode":"click_to_talk"}
```

The desktop helper always includes a `text` field, even for states without text
content. Firmware may immediately render local physical feedback, such as
switching Tecki to the recording pose when the primary button starts audio, but
the app's `ui_state` is the authoritative display state.

### Fork addition: `choices` on `ui_state` and the `choice` state event

When the server asks back (for example "… Soll ich buchen?"), the Todoteck
bridge adds the possible answers to the `ready` state:

```json
{"event":"ui_state","state":"ready","text":"12,40 € Bäcker, heute. Soll ich buchen?","choices":["Ja","Nein"]}
```

At most three, each up to 15 bytes. The Waveshare watch draws them as touch
areas below the answer; a tap sends

```json
{"event":"choice","index":0,"text":"Ja"}
```

and the bridge answers the open question with that text — the same as saying
it. The StickS3 ignores `choices` and keeps the spoken answer. The next scene
clears the areas.

### Fork addition: spoken answers on the watch speaker (`speaker_rx`)

The bridge can play the server's spoken answer on the watch instead of the
phone. Every write to `speaker_rx` starts with a type byte:

| Type | Payload | Meaning |
| --- | --- | --- |
| `0x01` | sample rate, u16 little endian (8000) | begin — replaces an answer still playing |
| `0x02` | IMA-ADPCM, 4 bit per sample, low nibble first | audio |
| `0x03` | — | end — play what is buffered, then close the speaker |
| `0x04` | — | abort |

The watch buffers up to 32 s in PSRAM and starts after 0.4 s of audio. It
ignores `0x01` while recording or in silent mode, and stops playback when a
recording starts or a palm covers the screen. IMA-ADPCM at 8 kHz is about
4 KB/s — small enough for BLE next to everything else, and trivial to encode
on the phone.

### Fork addition: watch face and cards (Waveshare watch)

The bridge keeps the watch face and three list cards (`today`, `shopping`,
`switches`) current; since 0.4.0 a fourth card, the weather, comes with its
own `weather` event (see the 0.4.0 section). All of it is optional; the
StickS3 ignores it.

```json
{"event":"face","lines":["Pool rückspülen · 18:00","12° · Regen"],"event_title":"Zahnarzt","event_start":1793520000}
{"event":"list","kind":"shopping","rev":7,"part":0,"items":[["Milch",""],["Brot",""]]}
```

`face` carries up to three lines (56 bytes each since 0.4.0, 40 before) shown under the time, plus the
next calendar event (UTC seconds). The watch keeps the countdown itself and
vibrates ten minutes before it starts.

`list` arrives in parts because one write carries about 240 bytes: part `0`
clears the card, every part appends `[text, aux]` pairs (aux is a time for
`today`, `"1"`/`"0"` for `switches` — or, since 0.4.3, a word of up to 15
bytes such as the current option of an `input_select` or "Offen"/"Zu" of a
lock, which the watch shows as it is and replaces with "..." after a tap until
the next list arrives). At most 15 entries per card. The watch
never sees IDs; a tap on a row answers with kind, `rev` and position, and the
bridge maps it back to the item it sent — a `rev` that no longer matches is
ignored.

```json
{"event":"item","kind":"shopping","rev":7,"index":3}
{"event":"glance_request"}
```

`{"event":"action"}` is the watch's action button (PWR held for 1.5 s); the
bridge decides what it does.

`glance_request` asks the bridge to send face and cards now (the watch sends it
when the cards are opened by swiping up).

### Fork addition: UI/UX package (Waveshare watch)

Five more messages for the watch. All are optional: the StickS3 ignores them,
and watch firmware older than 0.3.3 ignores unknown events (`ble_control_cb`
does nothing for an `event` it does not know), so the bridge may always send
them to a `watch_206`.

Bridge → watch (`control_rx`):

```json
{"event":"watch_settings","font":"m","home_after_s":30,"tiles":["silent","brightness","cinema","saver","torch","battery"]}
{"event":"transcript","text":"Leg eine Aufgabe an, Pool rückspülen","cancel_ms":2000}
{"event":"list","kind":"today","rev":7,"part":0,"items":[["Pool messen","18:00","#2ecc71"],["Müll raus","",""]]}
```

`watch_settings` carries the watch settings from the app. The bridge sends it
on every connection and after every glance fetch; it is small and idempotent.
The watch keeps the values in RAM and in RTC memory, so deep sleep does not
reset them.

| Field | Values | Default | Effect |
| --- | --- | --- | --- |
| `font` | `"s"`, `"m"`, `"l"` | `"m"` | Answer text steps 26–18 / 30–18 / 30–23 px, card rows 20 / 23 / 26 px (6 / 5 / 5 per page, each row two text lines), question line 18 / 20 / 23 px, transcript like the answer |
| `home_after_s` | `0` (never) or 10–600 | `30` | Back to the watch face after this long without input (closes cards, control centre, history). A new answer nobody has touched yet stays at least 120 s. Not during recording, thinking, an open question, the transcript, OTA or the torch |
| `tiles` | ordered subset of <!-- doku-vertrag:uhr-kacheln -->`silent`, `brightness`, `cinema`, `saver`, `torch`, `battery`<!-- /doku-vertrag --> | all six in this order | Control-centre tiles in order; missing ones are hidden, the grid fills from the top (two columns). Unknown names are dropped, duplicates ignored, an empty list changes nothing |

A missing or invalid field leaves the current value. A new `home_after_s`
applies from the next input (a settings message alone does not restart the
countdown); `0` stops it at once.

`transcript` shows "Verstanden" and the recognised text before the bridge acts
on it, with an "Abbrechen" area at the bottom and a thin bar that runs down
over `cancel_ms` (default 2000, at most 10000). A tap on the area, a palm on
the screen, or a short press on the talk button within that window makes the
watch send `cancel_transcript`, show "Abgebrochen" and vibrate briefly. The
next `ui_state` (`thinking`, `ready`, …) ends the scene — since 0.4.4 except
a `thinking` with `stage` inside the window (see the 0.4.4 section); after the
window has run out the text stays but the area no longer cancels.

`list` items may carry a third element: the project colour `#RRGGBB` or `""`.
Only `today` fills it; `shopping` and `switches` send `""` or omit it. The
watch draws a 6 px rounded stripe on the left of the row; anything that is not
`#` plus six hex digits means no stripe. Older firmware reads only elements 0
and 1.

Watch → bridge (state characteristic, like `choice` and `item`):

```json
{"event":"cancel_transcript"}
{"event":"undo"}
```

`undo` reverts a card action (`item`). After every `item` the watch shows a
"Rückgängig" chip for 5 s (gone earlier on a card switch or when the cards
close); after that the action stays undoable for 15 min from the history
(see the 0.4.0 section below). Since 0.4.0 the event carries the coordinates
of the `item` it reverts (`kind`, `rev`, `index`); the bridge keeps the last
20 card actions for 15 min and matches on them. A bare `{"event":"undo"}` from
older firmware still reverts the last action. The watch restores the row's
previous look at once if the list it showed has not been replaced since (same
`rev`); otherwise the next glance redraws it.

`device_info` is unchanged — the bridge recognises the watch by
`"hardware":"watch_206"` as before.

### Fork addition: history, weather page, list groups (0.4.0)

Package G for the watch (11/2026). Everything here is optional in both
directions: the StickS3 ignores it, watch firmware before 0.4.0 ignores
unknown events and unknown JSON fields, and a bridge that sends none of it
leaves the watch as it was in 0.3.6.

**`outcome` and `ref` on `ui_state`** — bridge → watch, in the same write as
the answer:

```json
{"event":"ui_state","state":"ready","text":"Aufgabe angelegt: Pool rückspülen","question":"Leg eine Aufgabe an, Pool rückspülen","outcome":"created","ref":12}
```

`outcome` is one of `created`, `done`, `answer`, `error`, `none` and becomes
the marker of the history entry (green dot "angelegt" / "erledigt", grey
"Antwort", red "Fehler"). `ref` (1–65535, counting up) is present only while
the bridge can take the turn back; it is the handle for `undo_turn` below and
is valid for 15 min. A failed undo comes back as `ready` with
`"outcome":"error"` and the error text, not as `state:"error"`.

**History chips** — watch → bridge, from a history entry:

```json
{"event":"repeat","text":"Leg eine Aufgabe an, Pool rückspülen"}
{"event":"undo_turn","ref":12}
{"event":"undo","kind":"today","rev":7,"index":3}
```

"Nochmal" sends `repeat` with the entry's question; the bridge runs it as a
text turn, like the action sentence, and answers normally. "Rückgängig" sends
`undo_turn` with the entry's `ref`, or — for an entry the watch itself wrote
when a row was checked off or toggled — `undo` with the coordinates of that
`item`. Both chips are offered for 15 min after the entry; the bridge answers
`undo_turn` with a `ready` state whose `question` is "Rückgängig" and whose
`outcome` is `done` or `error`. The watch keeps its last ten entries (time,
question, answer, outcome, ref or card coordinates) in NVS, so they survive a
restart; the first page of the history is the day's balance ("Heute · 4
Befehle · 3 erledigt · 1 Frage · 0 Fehler"), counted on the watch from
midnight local time.

**`list` items with group and flags** — bridge → watch:

```json
{"event":"list","kind":"today","rev":7,"part":0,"items":[["Pool messen","18:00","#2ecc71","Heute",""],["Müll raus","31.10.","","Überfällig",""]]}
{"event":"list","kind":"switches","rev":2,"part":0,"items":[["Licht Küche","1","","","c"],["Brunnen","Pool füllen","","",""],["Garten Tür","Zu","","","c"]]}
```

Element 4 is the group heading (`today` only: "Angepinnt", "Überfällig",
"Heute", "Außerdem wichtig"); the watch draws a thin heading line wherever it
changes from the row before. Element 5 is a flag string: `c` means the row
needs the OK step (a switch with `confirm` set in the app) — tap selects, the
green "OK – schalten" chip toggles. Since 0.4.5 `s` marks an `input_select`: a tap
opens the option picker (see the 0.4.5 section). A missing element reads as `""`. An `aux`
that is a date (`31.10.`) marks an overdue task and is drawn red. Rows no
longer sit on pages: the list scrolls, and up/down move it by one view.

**`weather`** — bridge → watch, in up to four parts of at most 240 bytes;
`part` names the section (0–3), it is not a sequence number. Part 0 clears
the state and carries the summary; a bridge without weather sends part 0 with
empty fields. Empty parts 1–3 are not sent.

```json
{"event":"weather","rev":3,"part":0,"now":"10° · Bewölkt","today":"14° / 6° · Regen 60 %","rain":"Regen ab 15 Uhr","sun":"07:14 - 16:58","wind":"18 km/h","stale":""}
{"event":"weather","rev":3,"part":1,"hours":[["12h","11°",""],["13h","12°",""],["14h","12°",""],["15h","11°","r"],["16h","10°","r"],["17h","9°",""]]}
{"event":"weather","rev":3,"part":2,"days":[["Mo","14° / 6°","Regen 60 %"],["Di","12° / 4°","Bewölkt"],["Mi","11° / 3°","Sonnig"],["Do","13° / 5°","Schauer 30 %"],["Fr","10° / 4°","Regen 80 %"]]}
{"event":"weather","rev":3,"part":3,"pollen":["Hasel mäßig"],"warnings":["Sturmböen bis 18 Uhr"]}
```

All texts are at most 40 bytes and Latin-1 renderable (`°`, `·`, `-`; an
`…` becomes `...` on the watch). `stale` is `""` or the local time of the
snapshot ("08:30") when it is older than the server's limit; the watch then
shows "Stand 08:30". The watch shows the weather as the fourth card right of
Zuhause: page 1 now/today/rain/six hours/sun and wind, page 2 the next days,
page 3 pollen and warnings (only when there are any). The first warning also
sits in red under the title of page 1; a warning text the watch has not shown
before makes it vibrate once. Until the first `weather` arrives the card says
"Noch kein Wetter vom Handy.".

`face` lines may now be up to 56 bytes (was 40); `event_title` stays at 40.

### Fork addition: battery log (0.4.1)

Package H for the watch (11/2026): measure first, then save. Every ten
minutes the watch takes a sample of time, charge and state and keeps 288 of
them (48 h) in RTC memory, where they survive deep sleep but not a power
loss; the last six also go to NVS once an hour and come back after a cold
start. The bridge collects them and uploads them to the server, which works
out the discharge rate per firmware version — so every change shows what it
is worth.

**`battery_log_request`** — bridge → watch on `control_rx`, after
`device_info` and then hourly:

```json
{"event":"battery_log_request","since":1762000000}
```

`since` is the last sample time the bridge already holds (Unix seconds,
0 for everything). The watch answers on `state_tx` with one or more
**`battery_log`** events, each at most 240 bytes, written 30 ms apart so the
bridge keeps up:

```json
{"event":"battery_log","part":0,"total":3,"samples":[[1762000000,87,13],[1762000600,86,8]]}
```

`part` counts from 0, `total` is the number of parts, and only samples with
`t > since` are sent, oldest first. Nothing new gives one part with an empty
list: `{"event":"battery_log","part":0,"total":1,"samples":[]}`. A sample is
`[t, p, f]`: `t` Unix seconds (local time is the server's business), `p` per
cent, `f` a bit mask of the state at that moment:

| Bit | Meaning |
| --- | --- |
| 1 | charging |
| 2 | USB power present |
| 4 | display lit (bright or dimmed, not off) |
| 8 | Bluetooth connected |
| 16 | light sleep enabled |
| 32 | a recording happened since the previous sample |

Samples are only taken once the clock has been set (RTC chip or `time`);
the firmware version is not part of a sample — the bridge takes it from
`device_info` and attaches it to the upload. The StickS3 ignores the request.

### Fork addition: 0.4.4

Package I for the watch (11/2026): night mode, stages instead of "Denkt
nach", watch-face layouts, and calendar events on the Today card. Everything
is optional in both directions: older watch firmware ignores unknown fields
and events, a bridge that sends none of it leaves the watch as in 0.4.3, and
the StickS3 shows the stage line and ignores the rest. `device_info` is
unchanged (it stays within 244 bytes; the stages are not announced in
`ui_states`).

**`watch_settings` — two more fields** (bridge → watch):

```json
{"event":"watch_settings","font":"m","home_after_s":30,"tiles":["silent","brightness","cinema","saver","torch","battery"],"face":"info","night":"21:00-06:30"}
```

| Field | Values | Default | Effect |
| --- | --- | --- | --- |
| `face` | <!-- doku-vertrag:uhr-layouts -->`info`, `minimal`, `large`<!-- /doku-vertrag --> | `info` | Watch-face layout. `info`: the time in cells, up to three lines, event countdown, battery, Tecki (as in 0.4.3). `minimal`: only the large time, the date ("So 1.11.") and the battery as a dot — no lines, no Tecki, no event. `large`: hours and minutes on two lines in 180 px, below exactly one line — the event countdown, otherwise the first face line; battery small in the header |
| `night` | `""` (off) or `"HH:MM-HH:MM"` (local time, may span midnight) | `"21:00-06:30"` | Night window. Inside it the watch face — all three layouts — is drawn in dimmed red (time `#b0301c`, secondary text `#6a1d10`, Tecki tinted red, no green or blue accents; battery red stays red), and the brightness is capped at the lowest control-centre step ("Dunkel") in every scene; other scenes keep their colours. A brightness raised in the control centre at night holds until the screen next goes off. The window is checked every 10 s (battery tick) and when the clock is set. The night charging mode (USB, 22–6 h) keeps priority and stays as it is, but red inside the window |

An unknown `face` or a malformed `night` leaves the current value. The watch
keeps both in RTC memory like the other settings.

**Layout picker on the watch** — a long press (0.6–2 s, one finger, no swipe)
on the watch face opens it: one large preview with page dots and the name
underneath ("Info", "Minimal", "Groß"). Swipe left/right pages, a tap takes
the shown layout, swipe down or the left-edge back swipe cancels, and so do
10 s without input. The watch applies the layout at once, keeps it in RTC
memory and sends, on the state characteristic like `item` and `undo`:

```json
{"event":"face_layout","face":"minimal"}
```

The bridge stores it on the server and sends `watch_settings` again; a later
`watch_settings` with a different `face` wins — the server is the truth.
Elsewhere a long press stays what it was before 0.4.4: a tap.

**Stages on `ui_state:thinking`** (bridge → watch):

```json
{"event":"ui_state","state":"thinking","stage":"understand"}
{"event":"ui_state","state":"thinking","stage":"act"}
```

| `stage` | Line on the screen | Sent by the bridge |
| --- | --- | --- |
| `understand` | "Verstehe …" | as soon as the recording has ended and audio goes to the server (transcription) |
| `act` | "Erledige …" | as soon as the transcript is there and the bridge starts `/voice/turn` (or the execution) |
| missing (older bridge) | "Denkt nach" | — |

From 5 s after the first `thinking` of a turn (not per stage) the line gets a
seconds counter: "Erledige … 7 s", updated once a second while the screen is
on. A `text` on `thinking` is shown below the line. `recording` reads "Höre
zu". A `thinking` with `stage` that arrives while the `transcript` scene's
cancel window is still running does not end that scene (it would hide
"Abbrechen"); a `thinking` without `stage` ends it as before.

**Calendar events on the Today card** — `list` items for `today` may carry
the flag `e` (element 5) for a calendar event of today, in the group
"Termine" that comes before all task groups:

```json
{"event":"list","kind":"today","rev":8,"part":0,"items":[["Zahnarzt","14:30","","Termine","e"],["Pool messen","18:00","#2ecc71","Heute",""]]}
```

An `e` row is read-only: a tap neither selects nor sends `item`, there is no
OK chip and no strikethrough; the time sits on the right like a task's, and
instead of the project colour the row carries a stripe in calendar blue
`#3b82f6`. All-day events have an empty `aux`. The card counts as empty (Tecki
and "Heute ist alles erledigt.") only without tasks and without events. Up to
five group headings per list (was four).

### Fork addition: 0.4.5

Option picker for `input_select` on the Home card (11/2026). Until 0.4.4 a tap
on such a row stepped to the next option (`item`, the server cycles). Now the
watch shows the options and the user picks one. Optional in both directions:
older firmware ignores the flag `s` and keeps sending `item` — the bridge then
steps on as before; a bridge that never sets `s` leaves the watch as in 0.4.4.
`device_info` is unchanged.

**Flag `s` in `list`** (bridge → watch, element 5, `switches` only) marks a
row that is an `input_select`; it may stand next to `c`:

```json
{"event":"list","kind":"switches","rev":4,"part":0,"items":[["Licht Küche","1","","","c"],["Büro Modus","Feierabend","","","s"]]}
```

**`options_request`** — watch → bridge on the state characteristic, sent at
once when an `s` row is tapped (no OK step, even with `c`: the choice in the
list is the second step). Coordinates as with `item`:

```json
{"event":"options_request","kind":"switches","rev":4,"index":1}
```

**`options`** — bridge → watch on `control_rx`, answered from the last glance
the bridge holds (no server call):

```json
{"event":"options","rev":4,"index":1,"current":0,"items":["Feierabend","Office","Gaming"]}
```

`items` are the options in Home Assistant's order, at most 8, each clipped to
16 characters by the server; `current` is the index of the active option, `-1`
when unknown. If the message does not fit one write, the bridge drops options
from the end but never the current one. If `rev` is stale (the card has
changed since), the bridge answers with an empty list and sends the new card
afterwards:

```json
{"event":"options","rev":4,"index":1,"current":-1,"items":[]}
```

The watch opens the picker right after `options_request` with "Lädt …";
`options` fills it when `rev` and `index` match the open picker. An empty
`items` closes it with the error tone; no `options` within 3 s shows "Keine
Antwort" for 1.5 s and closes.

**`option`** — watch → bridge on the state characteristic, when an option
other than the active one is tapped; `choice` is the index into `items`:

```json
{"event":"option","kind":"switches","rev":4,"index":1,"choice":2}
```

The bridge maps `rev`/`index` to the entity (as with `item`) and `choice` to
the option text from its glance, and calls the server with that option
(`input_select.select_option`). Success: status line, undo record under the
same coordinates as `item`, then a fresh glance. On the watch the row shows
"..." until the new card arrives, "Rückgängig" stands for 5 s (`undo` with the
same `kind`/`rev`/`index`), and the history keeps "Büro Modus: Gaming" with
those coordinates — `undo` from there restores the previous option through the
server's record. Tapping the active option sends nothing and only closes;
swipe down (at the top of the list), back from the left edge and 10 s without
input close without sending.

### Fork addition: `question` on `ui_state`

The Todoteck bridge adds one optional field to the `ready` state:

```json
{"event":"ui_state","state":"ready","text":"Aufgabe angelegt: Pool rückspülen","question":"Leg eine Aufgabe an, Pool rückspülen"}
```

`question` is what the host understood — the transcript, not the answer. The
fork shows it as the headline above the answer and keeps both in a five-entry
ring that the `secondary` button steps through. It rides in the same write as
the answer so the two cannot drift apart, and it is budgeted at 64 bytes:
inside one 244-byte write the answer is the content and the question is its
label.

Optional in both directions. A host that does not send it leaves the screen as
it was (the status word `Bereit` above the answer); firmware that does not know
it ignores an unknown JSON field, as it always has.

The fork renders the `text` field: it arrives with `ready` and is shown as the
answer, in the largest of six font sizes that fits, with Tecki stepping aside
for text that needs the whole screen. Upstream firmware ignores it for anything
but picking a fixed English hint, because its LVGL font set is ASCII-only. Both
behaviours are legal for a host — a bridge cannot tell from the protocol which
one it is talking to, and the fork's fonts stop at Latin-1 either way.

### Fork addition: `ui_state:no_speech`

The Todoteck fork accepts one further UI state:

```json
{"event":"ui_state","state":"no_speech","text":"Ich habe nichts gehört."}
```

It means the turn ran but nobody spoke: the recording was silent, or the
recognizer invented a sentence out of that silence. Whisper has no way to say
"there was nothing" — it must emit tokens, and on German silence it falls back
to the subtitle credit it saw at the end of thousands of training videos
("Untertitelung des ZDF, 2020"). The Todoteck server detects that case and
answers the bridge with the reason `no_speech` instead of acting on the
invented sentence.

The device shows `Nichts gehört` and plays the short low discard tone, not the
two falling error tones: nothing is broken, the button simply fired in a
pocket. Without the state, a false trigger arrived as an ordinary answer — the
screen showed a failed wiki search for a phrase nobody had said.

A bridge learns whether the firmware knows the state from `ui_states` in
`device_info`; the Todoteck bridges fall back to `error` when it is missing,
because an unknown state is ignored and would leave the display sitting on
`thinking`.

### Fork addition: `device_name`

The Todoteck fork accepts one further control event, which replaces the
advertised `VS-XXXX` name in the header with the name the device carries in
Todoteck:

```json
{"event":"device_name","name":"Handgelenk"}
```

The name survives a disconnect, so the header keeps saying which of two sticks
is in your hand. Firmware that does not know the event ignores it.

### Fork addition: `time`

The Todoteck fork accepts a second control event, because the StickS3 has
neither an RTC chip nor Wi-Fi and therefore no clock of its own:

```json
{"event":"time","epoch":1756400000,"tz_offset_min":120}
```

`epoch` is UTC seconds since 1970, `tz_offset_min` the offset of local time in
minutes (120 for CEST). The firmware sets its system clock from it and renders
`HH:MM` on the pairing, ready and resting screens — the device is worn on a
wrist, and a watch that shows no time is a watch nobody looks at.

Sending an offset instead of a timezone name keeps a timezone database out of
the firmware. A host that never sends the event leaves the device without a
clock; the screens then look exactly as they did before, so this event is
optional like the others. Bridges send it right after connecting, which also
handles DST changes and the drift of the module's oscillator — the system clock
survives deep sleep but is not accurate over days, and the display therefore
shows no seconds.

`interaction_mode` controls the front-button behavior and idle screen hint.
`hold_to_talk` starts audio on primary down and stops on primary up.
`click_to_talk` starts audio on the first primary click and stops on the next
primary click.

Deprecated app-to-firmware events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `connected` | `ui_state:ready` | Connection is not a display state after pairing. |
| `partial` | `ui_state:thinking` with `text` | Partial text is display content for the thinking state. |
| `final` | `ui_state:pending_confirmation` with `text` | Final text is still cancellable until pasted. |
| `paste_done` | `ui_state:ready` | Once pasted, the device returns to ready. |
| `paste_cancelled` | `ui_state:ready` | Once cancelled, the device returns to ready. |
| `error` | `ui_state:error` with `text` | Errors are another UI state. |

## BLE OTA

The firmware uses a custom OTA channel over the same Voice Stick service. The macOS app writes OTA `begin` and `end` frames with BLE write-with-response, and streams OTA `data` frames with write-without-response using CoreBluetooth flow control.
The device sends progress notifications roughly every 32 KB of accepted firmware data.

The macOS app starts OTA for one connected device at a time. It discovers updates from the latest firmware manifest, downloads the manifest `ota_url`, verifies byte size and SHA-256, then sends the verified app-slot image over BLE. The browser flasher uses the manifest `merged_url` instead because USB flashing writes a merged image at offset `0x0`.

The 8 MB flash layout uses two 3 MB OTA app slots and keeps the remaining flash as a reserved SPIFFS data partition:

| Name | Offset | Size |
| --- | ---: | ---: |
| `ota_0` | `0x10000` | 3 MB |
| `ota_1` | `0x310000` | 3 MB |
| `storage` | `0x610000` | 1984 KB |

All multibyte fields are little-endian.

```text
struct OtaBeginFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x20 begin
  uint16_t header_len;    // 12
  uint32_t image_size;
  uint32_t transfer_id;
}

struct OtaDataFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x21 data
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t offset;
  uint8_t  payload[];
}

struct OtaEndFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x22 end
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t image_size;
}

struct OtaAbortFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x23 abort
  uint16_t header_len;    // 8
  uint32_t transfer_id;
}
```

`ota_tx` sends a state frame:

```text
struct OtaStateFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x30 OTA state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

OTA state events include:

```json
{"event":"ready","transfer_id":1,"size":1385760,"partition":"ota_1"}
{"event":"progress","transfer_id":1,"written":32768,"size":1385760}
{"event":"done","transfer_id":1,"reboot_ms":500}
{"event":"error","code":"bad_offset","esp_err":258}
{"event":"aborted"}
```

On the device display, OTA switches the normal idle/recording UI into an update state:

- `Updating` with percentage while the image is being written.
- `Rebooting` after the new boot partition is selected.

While OTA is active, the device ignores push-to-talk input and pauses display dimming/deep sleep timers. After a successful transfer, the firmware waits about 500 ms after sending the `done` event and then calls `esp_restart()`.
The desktop updater can cancel an in-progress transfer by sending `OtaAbortFrame`; the device aborts the OTA handle and keeps booting the current firmware.

Before switching the boot partition, the firmware compares the project name in
the new image's app description with its own (`voice_stick` on StickS3,
`voice_watch` on the Waveshare watch, set in `firmware/CMakeLists.txt`). An
image built for the other device is rejected with
`{"event":"error","code":"wrong_device",...}` and the running firmware stays.

## Runtime State Machine

StickS3:

```text
boot -> advertising -> connected -> idle -> recording -> idle
```

The firmware also dims the display after 30 seconds of idle time. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes the device from deep sleep.

macOS:

```text
needs_pairing -> scanning -> ready -> recording -> thinking -> pending_confirmation -> ready
```

During recognition and confirmation, the firmware keeps showing the thinking
pose until the app sends `ui_state:ready`. During pending confirmation, `primary`
confirms or pauses according to the app's internal countdown mode, and
`secondary` cancels. When idle, `secondary` restores the last recoverable input
confirmation. These meanings are app state-machine behavior, not firmware
protocol events.

Recordings shorter than 0.5 seconds are discarded locally and are not sent to ASR.

## ASR Transport

The desktop app can connect either directly to Volcengine or to VoiceStick Cloud. Both providers use the same WebSocket binary framing in the client, so request, audio, response, and error handling are shared.

Volcengine endpoint:

```text
wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
```

VoiceStick Cloud default endpoint:

```text
wss://api.xiaozhi.me/voicestick/asr/
```

The first request payload currently sent by the desktop app is:

```json
{
  "user": {"uid": "voice-stick-local"},
  "audio": {
    "format": "ogg",
    "codec": "opus",
    "rate": 16000,
    "bits": 16,
    "channel": 1
  },
  "request": {
    "model_name": "bigmodel",
    "enable_nonstream": true,
    "show_utterances": false,
    "enable_ddc": true
  }
}
```

The desktop app buffers Ogg chunks until the recording reaches 0.5 seconds, then starts ASR and flushes the buffered chunks. On button release, it sends the final Ogg chunk with the WebSocket last-packet flag and waits for the final response.

VoiceStick Cloud business errors should use the same error frame shape as Volcengine: message type `0x0f`, a four-byte big-endian error code, a four-byte big-endian message size, and a UTF-8 message. For quota or billing errors, the message should be JSON so the desktop app can surface an upgrade action:

```json
{
  "error": "quota_exceeded",
  "message": "Daily free quota has been used up.",
  "upgrade_url": "https://voicestick.app/account/billing"
}
```

See `docs/volcengine-asr.md` for the trimmed Volcengine API notes used by the desktop app.
