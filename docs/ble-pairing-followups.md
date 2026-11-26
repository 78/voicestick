# BLE Pairing Follow-ups (Open)

Items deferred from the Windows pairing fix in the firmware and Windows app releases after 0.4.5.

## Background

Re-pairing a host that the device still had keys for failed on Windows. NimBLE in ESP-IDF 6.1 (`BT_NIMBLE_DEFER_CONN_EVENTS_UNTIL_CONNECT`) delivers `BLE_GAP_EVENT_CONNECT` only after it has read the remote version and features, about 100 ms after the link comes up. A pairing request from a bonded peer in that window gets SMP Pairing Failed (0x08) instead of a `BLE_GAP_EVENT_REPEAT_PAIRING` event, because the app has not seen the connection yet. Windows sent its pairing request about 60 ms after connecting because the app called `PairAsync` before the link existed. The Windows app now pairs 300 ms after the link is up.

Devices on firmware 0.4.5 still send their Security Request immediately to a new host. The Windows app then falls back to the system pairing prompt for the first pairing. Newer firmware waits 1.5 seconds, so the app pairs without any prompt.

## Privacy of the reconnect advertisement

The reconnect advertisement is no longer discoverable, but it still carries the `VS-XXXX` name in the scan response and uses the fixed public address. Any BLE scanner can see that a VoiceStick is nearby and track it.

- Dropping the name needs the Windows, macOS, and Linux apps to identify devices without it (by address or by data in the advertisement). Apps released before that change would stop reconnecting after an OTA, so it needs a minimum app version.
- With a fixed public address, dropping the name alone does not stop tracking. The real fix is LE privacy (resolvable private addresses). Test Windows reconnection by stored address, macOS, and whether existing bonds survive the OTA.
- Linux: check that BlueZ still reports the non-discoverable reconnect advertisement for bonded devices.

## Forgetting the device on both sides

Removing a device in an app only forgets it on that computer. The device still treats the computer as its owner and keeps its keys.

- A "forget this host" control command would let the Windows app clear both sides together with `UnpairAsync`.
- Do not use it on macOS: the Mac keeps its keys until the user removes the device in System Settings, and a Mac with stale keys cannot pair again without that step. Keeping the device's bond lets the Mac re-pair from the app.
- The command only works while the device is connected.
