# Voice Stick Protocol

This document describes the protocol implemented by the current firmware and macOS desktop app.

## Goals

- Low-latency push-to-talk audio from StickS3 to macOS.
- Opus over BLE to keep wireless bandwidth low.
- Ogg Opus forwarding from macOS to either Volcengine ASR or the VoiceStick Cloud relay.
- Final ASR text insertion into the focused macOS input field after release and confirmation.

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> Mac | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> Mac | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | Mac -> StickS3 | write without response |
| `ota_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104` | Mac -> StickS3 | write, write without response |
| `ota_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105` | StickS3 -> Mac | notify |

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

```json
{"event":"device_info","hardware":"stick_s3","firmware_version":"0.2.2","buttons":["primary","secondary"],"ui_states":["ready","recording","thinking","pending_confirmation","error"]}
{"event":"button_down","button":"primary","session_id":1234}
{"event":"button_up","button":"primary","duration_ms":620,"session_id":1234}
{"event":"button_down","button":"secondary"}
{"event":"button_up","button":"secondary","duration_ms":90}
```

Buttons are named by role instead of physical placement. On StickS3, the front
button maps to `primary` and the side button maps to `secondary`. `session_id` is
included when a `primary` press starts or stops a local audio recording.

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
```

The desktop helper always includes a `text` field, even for states without text
content. Firmware may immediately render local physical feedback, such as
showing the recording cat when the primary button starts audio, but the app's
`ui_state` is the authoritative display state. Current StickS3 firmware does not
render recognition text on-device because the LVGL font set does not include
Chinese glyphs; `text` is used only to choose fixed English hints.

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

During recognition and confirmation, the firmware keeps showing the thinking cat
until the app sends `ui_state:ready`. During pending confirmation, `primary`
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
