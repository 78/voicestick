# BLE Idle Power on Windows (Open Investigation)

Status: paused. Nothing in this document has shipped beyond what the firmware
already does.

## Goal

While connected and not recording, the device should spend as little time as
possible on the radio, without falling back to deep sleep. Deep sleep stays
reserved for one hour without use (`CONFIG_VOICE_STICK_DEEP_SLEEP_TIMEOUT_S`).

The firmware already saves what it can on its own side when idle: the CPU drops
to the crystal frequency and enters light sleep, the LED turns off, the PDM
microphone is disabled, and controller modem sleep is on. What is left is the
connection interval and peripheral latency, and the central decides those.

## Current behavior

- After a recording or a failed OTA the firmware asks for 50-200 ms, latency 8,
  6 s supervision timeout. When recording or OTA starts it asks for 15-30 ms,
  latency 0, 2 s timeout.
- Both requests use the L2CAP Connection Parameter Update procedure
  (`ble_l2cap_sig_update`). The LL procedure is not used because some Windows
  controllers advertise it but never answer, which dropped the link after the
  40 s LL response timeout (reason 0x22 / 546).
- During OTA the Windows app holds a `ThroughputOptimized` request
  (`OtaConnectionPreference` in `ble_central_win.cc`).
- macOS should honor the slow request, but this is untested.

## Findings so far (Windows)

Test setup: Windows 11 build 26300, a generic USB Bluetooth adapter
(Actions, `VID_10D7&PID_B012`) with the inbox driver. The results may not hold
for Intel or Realtek laptop radios.

| Experiment | Trials | Accepted | Applied |
| --- | --- | --- | --- |
| Device asks for 50-200 ms, latency 8, 30 s after connecting | 4 | 4 | 0 |
| Device asks for 60-120 ms, latency 0, 30 s after connecting | 4 | 4 | 0 |
| Device asks for 60-120 ms, latency 0, about 2 s after connecting | 4 | 4 | 0 |
| Device asks for 15 ms from the 30 ms default | 1 | 1 | 1 |
| Device asks for 30 ms from 15 ms | 2 | 2 | 2 |
| App holds `PowerOptimized` | ~10 | n/a | 0 |
| App holds `ThroughputOptimized` | ~8 | n/a | 1 |

What these show:

- Windows accepts the L2CAP request every time and responds within about 60 ms,
  but it never applied a request for a slower interval than its 30 ms default.
  Neither latency nor timing changed the outcome.
- Faster requests are applied within a few hundred milliseconds, and going back
  from 15 ms to 30 ms also works. So the link does not stay at 15 ms after an OTA.
- While the app holds any `RequestPreferredConnectionParameters` request,
  Windows rejects every device request (status 16, about 20 of 20).
- After a quick reconnect (the device stays up and is gone for about a second),
  Windows applies the parameters that last took effect on its own within
  0.3-0.5 s of connecting. This did not happen after the device rebooted and was
  gone for about a minute.
- A single 60-100 ms success earlier in the day was probably this remembered
  state, not the request itself.

What we still cannot tell apart: whether the Windows host decides not to send
the update, or whether it sends it and this adapter fails it. Only an HCI trace
can show that.

## What others report

- Windows accepting an L2CAP request without sending `LL_CONNECTION_UPDATE_IND`
  is a known behavior, seen with sniffers on Intel radios as well
  ([Microsoft Q&A](https://learn.microsoft.com/en-us/answers/questions/848142/windows-10-ble-connection-parameter-update-issue),
  [Nordic DevZone](https://devzone.nordicsemi.com/f/nordic-q-a/27235/win-10-ble-stacks-acting-as-a-central-problems-accepting-ble-connection-parameter-changes)).
- Windows is said to honor the GAP Peripheral Preferred Connection Parameters
  (PPCP) characteristic, and the
  [accessory guidelines](https://learn.microsoft.com/en-us/windows-hardware/design/accessory-guidelines/bluetooth-accessory-guidelines/bluetooth-accessory-guidelines-pairing-bonding-connecting)
  ask that it reflect the steady-state preference. Intervals should be multiples
  of 7.5 ms, a range should include 30 ms or a multiple of it, and general
  accessories should use 30 ms or more.
- Others measured the app presets at 180 ms / 6 s (`PowerOptimized`),
  60 ms / 4 s (`Balanced`) and 15 ms / 2 s (`ThroughputOptimized`). Intel
  radios on Windows 11 default to 60 ms. Microsoft's
  [API docs](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.requestpreferredconnectionparameters)
  recommend `ThroughputOptimized` only for firmware updates.

## Next steps

1. Capture an HCI trace with the Bluetooth Virtual Sniffer (`btvs.exe`, in the
   WDK) and Wireshark, and check whether Windows sends `LE Connection Update`
   for a slow request, and what the controller returns.
2. Repeat the table on an Intel or Realtek laptop radio, since that is what most
   users have.
3. Try PPCP (`CONFIG_BT_NIMBLE_SVC_GAP_PPCP_*`, all 0 today, so the
   characteristic does not exist). Risk: adding it moves every later attribute
   handle, including Service Changed. NimBLE stores CCCDs by handle and does not
   announce static database changes. So bonded centrals that cache the GATT
   database, macOS in particular, may need to pair again after the OTA. The
   Windows app always discovers uncached and re-subscribes, so it is not
   affected.
4. Only then decide the app side. If Windows does honor `PowerOptimized`
   somewhere, holding it blocks the device's own requests, so it should only be
   held while idle on hardware where it is shown to take effect.

## Test tooling

The reconnect trial script used for the table restarts the Windows app in a
loop, reads the device log on the serial port, and records whether each request
was accepted and applied. It is not in the repository. Its logic is simple to
recreate. Per cycle:

- kill and relaunch `VoiceStick.exe`;
- read the serial log for about 80 s;
- match the `requested ... conn interval`, `conn params request accepted` and
  `conn updated` lines.
