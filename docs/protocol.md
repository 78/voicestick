# Voice Stick Protocol (v2)

This document defines **Protocol v2** for communication between VoiceStick firmware and desktop clients (macOS, Windows, Linux), introduced in VoiceStick 0.5.0.

## Design Goals

- **App-driven initialization and policy**: Firmware exposes pure hardware primitives (inputs, streams, display, battery) and never persists mode state across connections. Every business workflow (hold-to-talk, click-to-talk, pending confirmation, shake cancellation, swing-to-join, games) lives in the desktop app.
- **Unified bidirectional streaming**: All real-time hardware I/O (`input`, `audio_in`, `imu`, and future `audio_out`) uses one WebRTC-inspired stream lifecycle (`stream.start` / `stream.renew` / `stream.stop`) multiplexed by `stream_id`.
- **Clean RPC control plane**: Standard `snake_case` JSON-RPC request/response correlation (`id`) with explicit error codes and transparent MTU chunking (`0x1F`).
- **Low latency with low idle power**: Short BLE connection intervals with high idle peripheral latency, instant uplink wake, and button-triggered local audio start with in-band DTX-compatible boundaries.
- **Permanent upgrade path**: The BLE service UUID, authorization characteristic, and OTA v1 channel remain unchanged so older 0.4.x firmware can always be upgraded to v2.

---

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

### Characteristics

| Group | Name | UUID | Direction | Properties | Purpose |
| --- | --- | --- | --- | --- | --- |
| **Stream** | `stream_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | Device -> App | `notify` | Uplink binary streams (`audio_in`, `imu`, `input`) |
| **Control** | `control_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | Device -> App | `notify` | JSON-RPC responses and system events |
| **Control** | `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | App -> Device | `write`, `write without response` (encrypted) | JSON-RPC requests and notifications |
| **OTA** | `ota_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104` | App -> Device | `write`, `write without response` (encrypted) | OTA v1 control and image chunks |
| **OTA** | `ota_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105` | Device -> App | `notify` | OTA v1 status notifications |
| **Auth** | `authorized` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5108` | Device -> App | `read` (encrypted) | Returns `0x01` after host authorization |
| **Stream** | `stream_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5109` | App -> Device | `write without response` (encrypted) | Reserved for downlink binary streams (`audio_out`, future bulk streams) |

> **GATT caching note**: Because `stream_rx` (`...5109`) is newly added in v2, firmware enables the standard GATT Service Changed (`0x2A05`) service so bonded hosts refresh cached attribute tables after OTA. Clients must not block basic v2 operation if `stream_rx` is absent on a device that does not advertise downlink stream capabilities.

### Advertising, Pairing, and Authorization

With no active BLE link, a press of the `primary` button advertises `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5106` for pairing. A VoiceStick without an authorized host opens this window by itself after boot or wake. The window closes after 60 seconds or a successful encrypted BLE bond. Each VoiceStick authorizes one host at a time but keeps the bonds of up to four hosts, so a previous host can be authorized again in the pairing window without forgetting the device in its system Bluetooth settings. When bond storage is full, the oldest bond other than the authorized host's is removed.

Outside the pairing window, a VoiceStick with an authorized host advertises `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5107` for reconnection; this advertisement is not discoverable, so system "add device" lists only show a VoiceStick in its pairing window. After a link comes up without encryption, the firmware waits 1.5 seconds before sending an SMP Security Request so a host can pair or restore encryption on its own first. Hosts should start pairing a few hundred milliseconds after the link is up rather than with the connection itself, because NimBLE answers a pairing request from a host it still has keys for with Pairing Failed while still reading remote features. A 3-second hold of `secondary` opens the pairing window. Earlier legacy firmware advertised the service UUID directly.

Desktop apps use the pairing advertisement in the device picker and check the encrypted `authorized` characteristic before saving the device. Background scanning uses the reconnect and legacy advertisements and matches the local `VS-XXXX` list. Firmware validates the saved host bond before accepting control, stream, or OTA writes or sending notifications. Desktop apps can connect to multiple VoiceSticks simultaneously.

---

## Compatibility, Versioning, and Legacy Upgrade Path

1. **Permanent upgrade path**: The service UUID, `authorized` characteristic, and OTA v1 framing (`ota_rx` / `ota_tx`) stay backward-compatible so a v2 desktop app can always discover and OTA-upgrade older 0.4.x firmware.
2. **Protocol v2 detection & 0.4.x fallback**:
   - After encryption and subscribing to `control_tx` and `stream_tx`, the desktop app sends a v2 framed `device.get_info` request (`0x02 0x10 ...`) to `control_rx`.
   - Legacy 0.4.x firmware expects raw unframed JSON (`0x7B '{'`) on `control_rx` and emits v1 `device_info` without `protocol_version: 2`. If the v2 `device.get_info` request times out (or an unsolicited v1 `device_info` arrives), the app may send legacy `{"event":"get_device_info"}` to read `hardware` and `firmware_version`, block input/streams, and prompt the user to run OTA v1 to upgrade to 0.5.0+.
   - Firmware v2 can also identify legacy unframed writes by checking byte 0 (`0x7B` vs `0x02`) if needed.
3. **Non-breaking future evolution**:
   - Readers must ignore unknown JSON fields and unknown optional stream/capability entries.
   - Preserve existing RPC method names, field names, and field types.
   - New hardware features (downlink `audio_out`, extra buttons, analog axes, wheels, haptics, `stream.update`) are advertised in `device.get_info` and added without incrementing `protocol_version`.
   - Increment `protocol_version` only if the framing envelope or JSON-RPC baseline contract changes incompatibly.

---

## Binary Framing Layer

All multibyte fields are **little-endian**. All v2 frames begin with `uint8_t version = 2` and `uint8_t type`.

### Frame Types Overview

| `type` | Characteristic | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `stream_tx` | Device -> App | `nop` link wake-up frame (4-byte empty frame; clients ignore) |
| `0x01` | `stream_tx` | Device -> App | `audio_in` encoded audio stream frame |
| `0x02` | `stream_rx` | App -> Device | `audio_out` encoded audio stream frame (reserved) |
| `0x03` | `stream_tx` | Device -> App | `imu` accelerometer / gyroscope sample frame |
| `0x04` | `stream_tx` | Device -> App | `input` HID-style full state snapshot frame |
| `0x10` | `control_rx` / `control_tx` | Bidirectional | Single-packet UTF-8 JSON control/event frame |
| `0x1F` | `control_rx` / `control_tx` | Bidirectional | Chunked UTF-8 JSON control/event frame |

*(OTA v1 frames `0x20`–`0x23` on `ota_rx` and `0x30` on `ota_tx` keep `version = 1`.)*

### Control Single JSON Frame (`type = 0x10`)

Used on `control_rx` and `control_tx` whenever the UTF-8 JSON fits in a single ATT packet (`4 + payload_len <= ATT_MTU - 3`, i.e., `payload_len <= 178` at baseline ATT MTU 185, or `<= 240` at preferred ATT MTU 247):

```text
struct ControlJsonFrame {
  uint8_t  version;      // 2
  uint8_t  type;         // 0x10
  uint16_t payload_len;  // byte length of utf8_json
  uint8_t  utf8_json[payload_len];
}
```

### Control Chunked JSON Frame (`type = 0x1F`)

Used transparently on `control_rx` or `control_tx` whenever a JSON message exceeds the single-packet MTU budget (for example, `device.get_info` with rich capabilities or long UTF-8 display text):

```text
struct ControlChunkFrame {
  uint8_t  version;      // 2
  uint8_t  type;         // 0x1F
  uint16_t payload_len;  // 3 + slice_len
  uint8_t  message_id;   // wrapping per-direction message ID
  uint8_t  chunk_index;  // 0 .. chunk_count - 1
  uint8_t  chunk_count;  // >= 2
  uint8_t  slice[payload_len - 3];
}
```

Chunking rules:
- Chunks of one message are sent strictly in order (`0` to `chunk_count - 1`) without interleaving another `0x1F` message in the same direction.
- Both sides support reassembling messages up to `max_message_bytes` (baseline minimum **1024 bytes**).
- Incomplete reassembly buffers are discarded on disconnect, on receiving a different `message_id`, or after a 2-second reassembly timeout.
- Messages that fit in a single packet must use `0x10`.

### Audio Stream Frame (`type = 0x01` / `0x02`)

Used on `stream_tx` (`0x01`, `audio_in`) and reserved on `stream_rx` (`0x02`, `audio_out`):

```text
struct AudioStreamFrame {
  uint8_t  version;       // 2
  uint8_t  type;          // 0x01 audio_in, 0x02 audio_out
  uint16_t header_len;    // >= 18 (18-byte fixed prefix + optional extension bytes)
  uint16_t stream_id;     // stream ID allocated by stream.start
  uint8_t  flags;         // bit0=start, bit1=end, bit2=flush (audio_out barge-in)
  uint8_t  reserved;      // 0
  uint32_t seq;           // wrapping packet sequence per stream
  uint32_t timestamp_ms;  // monotonic clock in ms (supports discontinuous / DTX gaps)
  uint16_t payload_len;   // encoded audio payload bytes (0 allowed for start/end markers)
  // uint8_t ext[header_len - 18];
  uint8_t  payload[payload_len];
}
```

- Readers must locate `payload` at byte offset `header_len` and ignore any unknown extension bytes.
- **In-band utterance / DTX boundaries**:
  - For discontinuous or button-triggered `audio_in` streams, the stream remains open across multiple utterances. While idle, zero audio frames are sent and the microphone is powered off.
  - At the instant capture begins (`t = 0 ms`), firmware immediately sends a zero-payload frame with `flags = 0x01 (start)` and `timestamp_ms = t0` (before the first Opus frame finishes encoding, so the app can render instant recording feedback).
  - Subsequent frames carry one raw encoded Opus packet (currently 60 ms of 16 kHz mono audio) with `flags = 0`.
  - When capture ends (button release, toggle stop, `stream.stop`, OTA, or low battery), firmware sends a zero-payload frame with `flags = 0x02 (end)` and `timestamp_ms = t1`. The app computes recording duration from `t1 - t0` (or received frame count) and finalizes ASR immediately.
  - If an `audio_in` stream ends cleanly on a single-packet boundary or future VAD/Opus DTX pauses between talkspurts, `timestamp_ms` preserves exact inter-packet timing across silence gaps.
- For future `audio_out` (`0x02`), setting `flags |= 0x04 (flush)` instructs firmware to discard queued playback audio immediately before playing the new payload.

### IMU Stream Frame (`type = 0x03`)

Used on `stream_tx` for 3-axis accelerometer or 6-axis accelerometer + gyroscope streams:

```text
struct ImuStreamFrame {
  uint8_t  version;       // 2
  uint8_t  type;          // 0x03 imu
  uint16_t payload_len;   // 8 + 2 * axis_count (14 for 3-axis, 20 for 6-axis)
  uint16_t stream_id;     // stream ID allocated by stream.start
  uint16_t seq;           // wrapping sample sequence per stream
  uint32_t timestamp_ms;  // wrapping device sampling clock in ms
  int16_t  samples[];     // board-axis order matching stream channels
}
```

- When `channels = ["accel"]`: 3 signed `int16` values (`ax, ay, az`), `payload_len = 14`, total frame **18 bytes**.
- When `channels = ["accel", "gyro"]`: 6 signed `int16` values (`ax, ay, az, gx, gy, gz`), `payload_len = 20`, total frame **24 bytes**.
- Scale factors (`accel_lsb_per_g`, `gyro_lsb_per_dps`) are declared in `device.get_info` (e.g., StickS3 BMI270: `8192` LSB/g and `16.384` LSB/dps; MagiClick 2P5 LIS2DH12: accel-only).

### Input Stream Frame (`type = 0x04`)

Used on `stream_tx` for HID-style full input state snapshots (buttons, plus future axes/wheels):

```text
struct InputStreamFrame {
  uint8_t  version;       // 2
  uint8_t  type;          // 0x04 input
  uint16_t payload_len;   // 12 for buttons-only (extensible per device.get_info)
  uint16_t stream_id;     // stream ID allocated by stream.start
  uint16_t seq;           // wrapping report sequence per stream
  uint32_t timestamp_ms;  // wrapping device monotonic clock in ms
  uint32_t buttons_mask;  // bit N = 1 if buttons[N] from device.get_info is pressed
  // Future extensions declared in device.get_info (e.g. int16 axes[], int16 wheel_deltas[])
}
```

- Total size for buttons-only devices: **16 bytes**.
- Firmware emits a report on every debounced button edge with the full current `buttons_mask` and exact edge `timestamp_ms`.
- Clients derive edges via `changed = mask ^ prev_mask` and measure press/double-click durations via `uint32` wrap-safe `(timestamp_ms - prev_timestamp_ms)`.
- **Self-healing against packet loss**: If `seq` advances by more than 1, the host resets in-progress gesture timers (such as double-click detection) and snaps its held button state to `buttons_mask` without ever leaving a stuck key.

### Link Wake-Up Frame (`type = 0x00`)

Used on `stream_tx` as a minimal 4-byte uplink frame (`[0x02, 0x00, 0x00, 0x00]`) to wake the BLE peripheral radio briefly under `slave_latency > 0` without negotiating a connection parameter change:

```text
struct NopStreamFrame {
  uint8_t  version;       // 2
  uint8_t  type;          // 0x00 nop
  uint16_t payload_len;   // 0
}
```

- Clients must silently ignore `0x00` frames on `stream_tx`.

---

## Control Plane: JSON-RPC and System Events

All JSON payloads on `control_rx` and `control_tx` use full English `snake_case` keys with strictly disjoint top-level envelopes so dispatch is unambiguous:

### Message Envelopes

| Kind | Direction | Top-Level Keys | Example |
| --- | --- | --- | --- |
| **Request** | App -> Device | `id`, `method`, optional `params` | `{"id":1,"method":"device.get_info"}` |
| **Notification** | App -> Device | `method`, optional `params` (no `id`) | `{"method":"stream.renew","params":{"stream_id":3}}` |
| **Success Response** | Device -> App | `id`, `result` | `{"id":1,"result":{...}}` |
| **Error Response** | Device -> App | `id`, `error` (`code`, `message`) | `{"id":3,"error":{"code":"busy","message":"imu in use"}}` |
| **System Event** | Device -> App | `event`, `seq`, `timestamp_ms`, `data` (**never** top-level `id`) | `{"event":"battery.changed","seq":1,"timestamp_ms":60120,"data":{...}}` |

- `id` is a positive integer generated monotonically by the app per connection (recommended RPC timeout: **3 seconds**, accommodating Apple-compliant idle wake periods up to 2 seconds).
- Standard `error.code` values:
  - `unsupported` — method, stream kind, codec, rate, or capability is not supported by this board/firmware.
  - `invalid_params` — missing, malformed, or out-of-range parameters.
  - `busy` — hardware resource cannot be acquired right now (e.g., OTA active, or board cannot run concurrent streams).
  - `not_found` — unknown or already-closed `stream_id`.
  - `internal` — unexpected hardware/driver failure.
- Event `seq` is a monotonic unsigned integer per connection. Event `timestamp_ms` is the device's monotonic millisecond clock (exact unsigned JSON integer `<= 2^53 - 1`).

---

### RPC Methods (6 Total)

#### 1. `device.get_info`

Read-only query returning device identity, protocol baseline, full hardware capabilities, and current battery reading. Has **no side effects** on running streams or display state. Automatically uses `0x1F` chunking if larger than the negotiated single-packet MTU.

Request:
```json
{"id": 1, "method": "device.get_info"}
```

Response (StickS3 example):
```json
{
  "id": 1,
  "result": {
    "protocol_version": 2,
    "hardware": "stick_s3",
    "firmware_version": "0.5.0",
    "device_uuid": "5a8f3b0e-2c71-4e6b-9d12-7f84b06d3ac1",
    "max_message_bytes": 1024,
    "buttons": ["primary", "secondary"],
    "streams": {
      "input": {},
      "audio_in": {
        "codecs": ["opus"],
        "sample_rates": [16000],
        "channels": [1],
        "frame_ms": [60],
        "triggers": ["button_hold", "button_toggle", "immediate"]
      },
      "imu": {
        "channels": ["accel", "gyro"],
        "rates_hz": [50, 100],
        "accel_lsb_per_g": 8192,
        "gyro_lsb_per_dps": 16.384,
        "max_lease_ms": 5000
      }
    },
    "display": {
      "scenes": ["idle", "recording", "busy", "error"]
    },
    "battery": {
      "percent": 73,
      "charging": false,
      "usb_powered": false
    }
  }
}
```

Board capability variations:
- **VoiceStick C3 v1**: `"hardware": "voicestick_c3"`, `"buttons": ["primary", "secondary", "prev", "next"]`, no `"imu"`, `"display": null`, `"battery": {...}`.
- **MagiClick 2P5**: `"hardware": "magiclick_2p5"`, `"imu": {"channels": ["accel"], "rates_hz": [50], "accel_lsb_per_g": ..., "max_lease_ms": 5000}`, `"display": {...}`, `"battery": null` (if unsupported).
- **Future speaker board**: includes `"streams": { ..., "audio_out": {"codecs": ["opus"], "sample_rates": [16000, 24000], "channels": [1], "frame_ms": [60], "max_buffer_frames": 12} }`.
- `device_uuid` is a persistent UUIDv4 stored in NVS across reboots and re-pairing.

#### 2. `device.reset`

Resets all runtime state to a clean neutral baseline: stops every active stream (emitting `END` on any capturing `audio_in` stream first), resets the display to default idle, and clears temporary link state. Executes the **exact same internal routine** as BLE disconnect.

The desktop app **must** call `device.reset` immediately after `device.get_info` when taking ownership of a connection—including when the app restarts and re-attaches to an OS-maintained BLE link that never physically disconnected—eliminating stale game/IMU/UI state.

Request:
```json
{"id": 2, "method": "device.reset"}
```

Response:
```json
{"id": 2, "result": {}}
```

#### 3. `stream.start`

Opens a data stream and returns a connection-unique `stream_id` (`uint16`, `1..65535`). Multiple streams of different kinds (e.g., `input` + `audio_in` + `imu`) may run concurrently unless the board returns `error.code = "busy"`.

- **`kind: "input"`** — Starts emitting `0x04` `InputStreamFrame` reports on `stream_tx`, and returns the current `buttons_mask` immediately:
  ```json
  // Request
  {"id": 3, "method": "stream.start", "params": {"kind": "input"}}
  // Response
  {"id": 3, "result": {"stream_id": 1, "buttons_mask": 0}}
  ```

- **`kind: "audio_in"`** — Opens an uplink audio stream (`0x01` frames on `stream_tx`):
  ```json
  // Request (Hold-to-Talk armed on primary button)
  {
    "id": 4,
    "method": "stream.start",
    "params": {
      "kind": "audio_in",
      "format": {
        "codec": "opus",
        "sample_rate": 16000,
        "channels": 1,
        "frame_ms": 60
      },
      "trigger": {
        "mode": "button_hold",
        "button": "primary"
      },
      "local_display_feedback": true
    }
  }
  // Response
  {"id": 4, "result": {"stream_id": 2}}
  ```
  - `trigger.mode`:
    - `"button_hold"`: Starts capturing on `button` down and stops on `button` up (0 ms local start, no round-trip delay).
    - `"button_toggle"`: Starts capturing on first `button` down and stops on the next `button` down.
    - `"immediate"`: Starts capturing immediately upon `stream.start` and continues until `stream.stop` (or optional `lease_ms` expiry).
  - `local_display_feedback` (optional bool, default `false`): When `true`, firmware immediately switches the display to `recording` while local capture is active and restores the last `display.set` state when capture ends.
  - `dtx` (optional bool, default `false`): Reserved for future native Opus/VAD discontinuous transmission.

- **`kind: "imu"`** — Opens an uplink IMU stream (`0x03` frames on `stream_tx`) with a mandatory lease:
  ```json
  // Request (e.g., 100 Hz 6-axis for Tennis, or 50 Hz accel-only for shake confirmation)
  {
    "id": 5,
    "method": "stream.start",
    "params": {
      "kind": "imu",
      "format": {
        "channels": ["accel", "gyro"],
        "rate_hz": 100
      },
      "lease_ms": 3000
    }
  }
  // Response
  {"id": 5, "result": {"stream_id": 3}}
  ```
  - Only `channels` and `rates_hz` advertised in `device.get_info` are accepted; unsupported requests return `error.code = "unsupported"` or `"invalid_params"`.
  - If the lease expires without `stream.renew`, firmware stops the sensor automatically and emits `stream.closed` (`reason: "lease_expired"`).

- **`kind: "audio_out"` *(reserved for future speaker hardware)*** — Opens a downlink audio stream (`0x02` frames on `stream_rx`) with credit-based flow control:
  ```json
  // Request
  {
    "id": 6,
    "method": "stream.start",
    "params": {
      "kind": "audio_out",
      "format": {"codec": "opus", "sample_rate": 24000, "channels": 1, "frame_ms": 60},
      "volume": 80
    }
  }
  // Response
  {"id": 6, "result": {"stream_id": 4, "initial_credits": 10}}
  ```

#### 4. `stream.renew`

Extends the lease of an active leased stream (such as `imu`) by its configured `lease_ms`. Typically sent as a **Notification** (without `id`) every 0.8 seconds; if sent with `id`, firmware replies `{"id": ..., "result": {}}` (or `not_found`).

```json
{"method": "stream.renew", "params": {"stream_id": 3}}
```

#### 5. `stream.stop`

Closes an active stream and releases its hardware resources. If an `audio_in` stream is currently capturing, firmware emits a final `AudioStreamFrame` with `flags = 0x02 (end)` before closing the stream.

```json
// Request
{"id": 7, "method": "stream.stop", "params": {"stream_id": 3}}
// Response
{"id": 7, "result": {}}
```

#### 6. `display.set`

Pure presentation command for boards advertising `"display"`. Firmware does not attach business semantics (such as blocking recording or arming shake detection) to display scenes. Can be sent as a Notification (best-effort) or Request (acknowledged).

```json
{"method": "display.set", "params": {"scene": "idle", "text": "Hold to Talk"}}
{"method": "display.set", "params": {"scene": "busy", "text": "Confirm or cancel"}}
{"method": "display.set", "params": {"scene": "idle", "text": "Racket"}}
{"method": "display.set", "params": {"scene": "error", "text": "ASR timeout"}}
```

- `scene`: Built-in visual state advertised in `device.get_info.display.scenes` (currently `"idle"`, `"recording"`, `"busy"`, `"error"`).
- `text`: Optional short UTF-8 display hint/label (empty string clears custom text). Current StickS3 LVGL font renders ASCII labels directly.

---

### System Events (2 Active + 1 Reserved)

Firmware emits system events on `control_tx` only while connected and authorized.

#### 1. `battery.changed`

Emitted when battery percentage (`0..100`), charging state, or USB power state changes (checked at startup, on USB power changes, after charging settles, and on a 60-second poll). Initial battery state is returned in `device.get_info`.

```json
{
  "event": "battery.changed",
  "seq": 1,
  "timestamp_ms": 60120,
  "data": {
    "percent": 72,
    "charging": false,
    "usb_powered": false
  }
}
```

#### 2. `stream.closed`

Emitted when firmware passively terminates a stream without an explicit `stream.stop` from the app (for example, lease expiration, OTA start, low-battery shutdown, or sensor error).

```json
{
  "event": "stream.closed",
  "seq": 2,
  "timestamp_ms": 95400,
  "data": {
    "stream_id": 3,
    "reason": "lease_expired"
  }
}
```

Standard `reason` values: `"lease_expired"`, `"ota"`, `"low_battery"`, `"error"`.

#### 3. `stream.credit` *(reserved for future `audio_out`)*

Replenishes downlink frame credits as the device drains its playback buffer:

```json
{
  "event": "stream.credit",
  "seq": 3,
  "timestamp_ms": 102000,
  "data": {
    "stream_id": 4,
    "credits": 4
  }
}
```

---

## Connection Lifecycle, Power Management, and Concurrency

### Standard Initialization Handshake

```text
1. BLE connect -> encrypt -> subscribe to control_tx (5102) & stream_tx (5101)
2. App -> FW:  {"id":1, "method":"device.get_info"}
   FW  -> App: {"id":1, "result":{"protocol_version":2, ...}}
   (If legacy 0.4.x detected -> stop here and offer OTA v1 upgrade)
3. App -> FW:  {"id":2, "method":"device.reset"}
   FW  -> App: {"id":2, "result":{}}
4. App -> FW:  {"id":3, "method":"stream.start", "params":{"kind":"input"}}
   FW  -> App: {"id":3, "result":{"stream_id":1, "buttons_mask":0}}
5. App -> FW:  {"id":4, "method":"stream.start", "params":{"kind":"audio_in", ...}}
   FW  -> App: {"id":4, "result":{"stream_id":2}}
6. App -> FW:  {"method":"display.set", "params":{"scene":"idle","text":"Hold to Talk"}}
```

### Runtime Reset & Deep Sleep

- **Non-persistent runtime state**: NVS stores only `device_uuid` and BLE bonds/owner. Active streams, display state, and connection parameters are strictly volatile.
- **Disconnect**: Firmware immediately stops all streams, powers off sensors/microphone, and resets display to default idle.
- **Deep sleep**:
  - StickS3 dims the display after 30 seconds of idle time and enters deep sleep on battery after 5 minutes (or 1 minute while disconnected). While USB powered or charging, it stays at the dimmed stage.
  - VoiceStick C3 turns off its status LED while waiting for the host and enters deep sleep after 60 minutes idle on battery (or 1 minute while disconnected).
  - Active leased streams (`imu`), active audio capture, or OTA prevent deep sleep.
  - Waking from deep sleep via the front button boots cleanly, reconnects BLE, and goes through the standard App initialization handshake before any voice or game streams run.

### BLE Link Parameters & Power Strategy

VoiceStick follows the same BLE power strategy as modern BLE mice: keep a short connection interval with a large `slave_latency` during normal operation, and only drop `slave_latency` to `0` for sustained downlink bursts (OTA):

- **Normal link (default after connect handshake)**: `interval = 15–30 ms`, `slave_latency = 30`, `supervision_timeout = 6 s` (voice-only boards without high-rate IMU, such as VoiceStick C3, may configure `interval = 45–60 ms`, `slave_latency = 30`, giving an Apple-compliant `~1.86 s` idle wake period).
  - Complies with Apple's Bluetooth Accessory Design Guidelines (`interval_min >= 15 ms`, `slave_latency <= 30`, `interval_max * (latency + 1) <= 2 s`, `timeout <= 6 s`).
  - **Zero extra uplink latency**: Per the BLE Link Layer spec, `slave_latency` only allows the peripheral to skip connection events when it has nothing to transmit. Whenever the user presses a button, captures Opus audio, or streams 50/100 Hz IMU data, the device wakes immediately on the next negotiated event and transmits continuously at full uplink throughput without needing any L2CAP parameter update.
  - **Immediate standby savings**: As soon as the user stops speaking or IMU stops, the radio immediately resumes skipping `slave_latency` events instead of staying awake at `latency = 0` for 30 seconds.
  - **Connect handshake grace window**: When a BLE link first connects, hosts start with `slave_latency = 0`. Firmware waits **3 seconds** after connection before requesting `slave_latency = 30`, allowing the serial initialization RPCs (`device.get_info` → `device.reset` → `stream.start`) to finish in milliseconds.
  - **Post-interaction fast-downlink window (display / status-LED boards)**: After a button release or stopping `audio_in` capture on boards advertising `display` (LCD screens or RGB status LEDs such as VoiceStick C3), firmware opens a brief **1.5-second** fast-downlink window: every **180 ms**, if no other uplink notification was sent in the past 180 ms, firmware emits a 4-byte `nop` (`0x00`) frame on `stream_tx` so the radio wakes at the next connection event and immediately receives queued host commands (`display.set`, `stream.stop`, etc.) within `~90–180 ms` instead of waiting for the full idle wake period. The window stops immediately upon receiving `display.set` with `scene = "idle"` or `"error"`, or when audio capture / IMU streaming starts. Boards without any display/LED output disable this NOP window completely.
- **OTA link (bulk downlink)**: `interval = 15–30 ms`, `slave_latency = 0`, `supervision_timeout = 4 s`.
  - Held continuously while **OTA is active** (from `OtaBeginFrame` until reboot, abort, or error), because OTA transfers megabytes of host-to-device downlink data that would otherwise be throttled by `slave_latency`.
  - On OTA abort or error, firmware immediately returns to the normal `slave_latency = 30` link.
- **Display dim decoupled**: Display dimming (30 seconds of inactivity on screen-equipped boards) is managed independently of BLE connection parameters.

### Concurrent Stream TX Priority

All notifications share the underlying BLE controller TX buffer pool. When multiple streams run concurrently (e.g., `input` + `audio_in` + `imu`) under radio congestion, firmware enforces strict priority:

1. `control_tx` (RPC responses & system events) — highest priority, never dropped for stream traffic.
2. `stream_tx` `input` (`0x04`) & `audio_in` (`0x01`) — high priority.
3. `stream_tx` `imu` (`0x03`) — best-effort; stale IMU samples may be dropped if TX buffers run low.

---

## Desktop App Workflows (Policy Ownership)

The desktop app owns all product workflows on top of Protocol v2 primitives:

- **Single-device mode arbitration**: Each connected device operates either as a system voice input device or in game / WebSocket passthrough mode.
- **Voice dictation**:
  - Arms `audio_in` with `button_hold` (Hold-to-Talk) or `button_toggle` (Click-to-Talk) on `primary`.
  - Discards utterances shorter than 0.5 seconds locally (measured from `AudioStreamFrame` `START`..`END` timestamps/frames) without calling ASR.
  - Recognizes `secondary` short-click (`< 3000 ms`) to cancel or restore, and on macOS recognizes a `secondary` double-click (second press within 180 ms of first release, second hold `<= 180 ms`) to toggle translation mode.
- **Pending confirmation & shake-to-cancel**:
  - When entering `pending_confirmation`, the app calls `stream.stop` on `audio_in` (so `primary` confirms/pauses instead of recording), calls `display.set({"scene":"busy","text":"Confirm or cancel"})`, and—if the device advertises `"imu"` with `"accel"`—calls `stream.start({"kind":"imu","format":{"channels":["accel"],"rate_hz":50},"lease_ms":3000})` to run shake detection locally in the app.
  - On confirm, cancel, or timeout, the app stops the `imu` stream, re-arms `audio_in`, and sets `display.set({"scene":"idle",...})`.
- **Swing-to-join & motion games**:
  - For swing-to-join, the app opens 50 Hz 6-axis `imu` streams on candidate devices (keeping `input` open) and runs quiet-grip + sustained-turn detection locally in the app.
  - Once a player's device is bound to a game role, the app stops `audio_in` (unless the game wants voice), upgrades the bound device's `imu` stream to 100 Hz, sets `display.set({"scene":"idle","text":"Racket"})` (or `"Left Hand"` / `"Right Hand"`), and stops all candidate probes.

---

## BLE OTA (v1 — Unchanged)

The firmware uses a custom OTA channel over the same Voice Stick service (`ota_rx` / `ota_tx`). Desktop apps write OTA `begin` and `end` frames with BLE write-with-response, and stream OTA `data` frames with write-without-response using BLE flow control. The device sends progress notifications roughly every 32 KB of accepted firmware data.

When OTA begins (`OtaBeginFrame`), firmware immediately closes any active v2 streams (emitting `AudioStreamFrame(END)` if `audio_in` was capturing, followed by `stream.closed` with `reason: "ota"`), rejects new `stream.start` requests with `error.code = "busy"`, and pauses display dimming and deep sleep timers.

The desktop app starts OTA for one connected device at a time. It discovers updates from the latest firmware manifest, downloads the manifest `ota_url`, verifies byte size and SHA-256, then sends the verified app-slot image over BLE. The browser flasher uses the manifest `merged_url` instead because USB flashing writes a merged image at offset `0x0`.

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

On the device display, OTA switches the UI to `Updating` with percentage while writing, then `Rebooting` after selecting the new boot partition. After a successful transfer, firmware waits about 500 ms after sending `done` and calls `esp_restart()`. Sending `OtaAbortFrame` aborts the transfer and keeps booting the current firmware.

---

## ASR Transport

The desktop app can connect either directly to Volcengine or to VoiceStick Cloud. Both providers use the same WebSocket binary framing in the client, so request, audio, response, and error handling are shared. For OpenAI Realtime, the app decodes incoming Opus packets to 24 kHz mono PCM16; for VoiceStick Cloud and Volcengine, it wraps them into Ogg Opus.

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

The desktop app buffers Ogg chunks until the recording reaches 0.5 seconds, then starts ASR and flushes the buffered chunks. On `AudioStreamFrame(END)`, it sends the final Ogg chunk with the WebSocket last-packet flag and waits for the final response.

VoiceStick Cloud business errors use the same error frame shape as Volcengine: message type `0x0f`, a four-byte big-endian error code, a four-byte big-endian message size, and a UTF-8 message. For quota or billing errors, the message is JSON so the desktop app can surface an upgrade action:

```json
{
  "error": "quota_exceeded",
  "message": "Daily free quota has been used up.",
  "upgrade_url": "https://voicestick.app/account/billing"
}
```

See `docs/volcengine-asr.md` for the trimmed Volcengine API notes used by the desktop app.

---

## Local Motion Game & Browser Bridge (macOS)

The macOS app owns all BLE connections and bridges 6-axis `imu` and `input` streams to local games and the browser studio:

- **Native game transport**: Launches the game executable with a private bidirectional Unix datagram socket on stdin (`--motion-stdin`). Each datagram is one message.
- **Discovery & role assignment**:
  - The game sends a JSON role request such as `{"roles":["racket"]}` or `{"roles":["leftHand","rightHand"]}` (with optional `"swingToJoin":true`).
  - The host returns an `InputRoster` with all authorized 6-axis IMU-capable devices (gated by `device.get_info` capabilities, not hardware model names), their local CoreBluetooth UUIDs, display names, role assignments, streaming IDs, and availability.
  - Explicit selection sends the same `roles` plus an `assignments` object mapping each role to a distinct device UUID (empty object releases all roles). Single candidate + single role auto-binds; multiple candidates or two-role games require player choice. Assignments last for the game process lifetime.
- **Local binary datagrams**:
  - IMU sample: 36-byte datagram carrying the 20-byte normalized 6-axis motion sample (`01 11`, `uint16 seq`, `uint32 timestamp_ms`, 6 × `int16` accel/gyro) followed by the source device's 16-byte RFC UUID.
  - Physical identification: 18-byte datagram (`01 13` followed by the 16-byte source UUID), derived by the host from a short `secondary` button press (`< 3000 ms`) on the `input` stream.
- **Swing-to-join (host-side detection)**:
  - When `"swingToJoin":true` is requested, the host opens 50 Hz 6-axis `imu` streams (with 3 s leases renewed every 0.8 s) on connected, non-busy candidates and runs quiet-grip + sustained-turn detection locally.
  - The host waits 350 ms from the first swing report to detect ambiguity, retries with a fresh attempt ID if ambiguous, excludes previously bound devices for two-hand roles, and stops unselected `imu` streams on bind, 12-second timeout, cancellation, or game exit.
  - Selected devices transition to 100 Hz `imu` streams and show their role label (`Racket`, `Left Hand`, or `Right Hand`) via `display.set`.
- **Browser WebSocket bridge**: Exposes a loopback-only HTTP + WebSocket bridge forwarding roster updates, binary IMU samples, identification events, and button edges (`button`, `action`, `timestamp_ms`) for assigned devices.
