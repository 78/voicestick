# BLE Pairing Notes

How pairing between VoiceStick and the desktop apps works, what went wrong on Windows after firmware 0.4.5, how it was debugged, and what is still open.

## How pairing works now

- The device advertises the pairing UUID only in its pairing window. That advertisement is the only discoverable one, so system "add device" lists and the app's Pair dialog only show a device in its pairing window.
- When a link comes up without encryption, the firmware waits 1.5 seconds before it sends an SMP Security Request. This gives the host time to pair or restore encryption on its own.
- The Windows app first removes any Windows bond for the device. It then opens the device and waits for the link to come up. About 300 ms later it pairs with `DeviceInformationCustomPairing` (Just Works, no Windows UI). If that fails, it falls back to the system pairing prompt. Only after pairing does it read the encrypted `authorized` characteristic.

## The Windows re-pairing failure

### Symptom

Clicking Pair showed the system "connection failed" dialog after about a second. A while later Windows showed an "add device" notification, and pairing from there succeeded. Pairing alternated between failing and working, and it never failed for a device that had no keys for this PC.

### Cause

ESP-IDF 6.1 enables `CONFIG_BT_NIMBLE_DEFER_CONN_EVENTS_UNTIL_CONNECT`. As a peripheral, NimBLE delivers `BLE_GAP_EVENT_CONNECT` only after it has read the remote version and features, about 100 ms after the link comes up. If a pairing request arrives in that window from a peer the device still has keys for, `ble_sm_chk_repeat_pairing` returns `BLE_HS_EBUSY`. The device then answers with Pairing Failed (reason 0x08, Unspecified) instead of raising `BLE_GAP_EVENT_REPEAT_PAIRING` to the app.

The old Windows app called `PairAsync` before the link existed, so Windows sent its pairing request about 60 ms after connecting. Windows then dropped the link and reported failure. The device's own Security Request later reached Windows as the "add device" notification. That attempt went through `REPEAT_PAIRING`, which deleted the old bond, so the next pairing worked and the one after it failed again.

### Fix

The Windows app now pairs 300 ms after the link is up, and the firmware no longer sends its Security Request right away. Devices on firmware 0.4.5 still send it immediately to a new host. The new app then falls back to the system pairing prompt for the first pairing; re-pairing works without a prompt.

## Debugging tips

### Getting a serial log

Release builds use light sleep. On the StickS3, the USB serial port drops whenever the chip sleeps, so the port disappears or reports an error. To read logs, build into a separate test build directory and set `CONFIG_FREERTOS_USE_TICKLESS_IDLE` to `n` in its `sdkconfig`. Light sleep then never starts. Do not change the release configuration for this. To flash while the port is gone, hold BOOT and press reset to enter download mode.

### NimBLE debug logs

NimBLE debug output needs all of the following in the test build:

- `CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG=y`
- `CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT` off and `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`
- `esp_log_level_set("NimBLE", ESP_LOG_DEBUG)` early in `voice_ble_init`, because the default log level stays at INFO

The debug log dumps every ACL packet. SMP uses L2CAP channel 0x0006, so the packet starts with a two-byte length followed by `06 00`. The next byte is the SMP opcode: `01` is Pairing Request, `05` is Pairing Failed, and `0b` is Security Request. For Pairing Failed, the byte after that is the reason. In the GAP event log, the numbers that mattered were 0 (connect), 1 (disconnect), 10 (encryption change), 17 (repeat pairing), 27 (pairing complete), and 38 (link established). A disconnect reason of 531 is 0x213, which means the remote host terminated the link.

### Windows app log

The Windows app writes `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`. The useful lines are `starting Windows pairing`, `Windows custom pair: result=`, `Windows system pair: result=`, and `authorized read`. Results are `DevicePairingResultStatus` values: 0 is Paired, 15 is OperationAlreadyInProgress, and 19 is Failed.

## Lessons

- Read both sides' logs with timestamps before changing code. Two guesses were tested and turned out wrong: that the immediate Security Request was the cause, and that Windows needed time after removing the bond. The cause only showed up in the NimBLE SMP packets.
- When pairing works only some of the time, look for a second path that succeeds. Here, the "add device" notification was a separate pairing that also deleted the old bond.
- A bug that only hits hosts the device still has keys for stays hidden when testing with a freshly erased device. Test re-pairing the same host as well as first pairing.
- Once the cause is known, remove the workarounds added while hunting for it. Use the logs to confirm which retries actually run. The extra pairing attempt after an unauthorized read and the wait for Windows to report the device as paired never ran after the fix, so both were removed.

## Open follow-ups

### Privacy of the reconnect advertisement

The reconnect advertisement is no longer discoverable, but it still carries the `VS-XXXX` name in the scan response and uses the fixed public address. Any BLE scanner can see that a VoiceStick is nearby and track it.

- Dropping the name needs the Windows, macOS, and Linux apps to identify devices without it (by address or by data in the advertisement). Apps released before that change would stop reconnecting after an OTA, so it needs a minimum app version.
- With a fixed public address, dropping the name alone does not stop tracking. The real fix is LE privacy (resolvable private addresses). Test Windows reconnection by stored address, macOS, and whether existing bonds survive the OTA.
- Linux: check that BlueZ still reports the non-discoverable reconnect advertisement for bonded devices.

### Forgetting the device on both sides

Removing a device in an app only forgets it on that computer. The device still treats the computer as its owner and keeps its keys.

- A "forget this host" control command would let the Windows app clear both sides together with `UnpairAsync`.
- Do not use it on macOS: the Mac keeps its keys until the user removes the device in System Settings, and a Mac with stale keys cannot pair again without that step. Keeping the device's bond lets the Mac re-pair from the app.
- The command only works while the device is connected.
