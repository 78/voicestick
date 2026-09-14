# VoiceStick Linux

GTK4 / libadwaita companion app for GNOME and other Linux desktops.

The app lives in the top bar as a StatusNotifierItem. Frequent actions stay in
that short menu. ASR keys, interaction options, per-device appearance, and
debug settings live in an Adwaita preferences window.

## GNOME top-bar icon

Stock GNOME does not host tray icons. Install and enable the AppIndicator
extension, then log out and back in once:

```sh
sudo apt install gnome-shell-extension-appindicator
gnome-extensions enable ubuntu-appindicators@ubuntu.com
```

Debian's package UUID is `ubuntu-appindicators@ubuntu.com`.

## Dependencies

Debian 13 / Trixie:

```sh
sudo apt install \
  cmake ninja-build g++ pkg-config \
  libgtk-4-dev libadwaita-1-dev libsoup-3.0-dev \
  libglib2.0-dev libatspi2.0-dev libssl-dev \
  gnome-shell-extension-appindicator
```

## Build

```sh
cmake -S desktop/linux -B desktop/linux/build -G Ninja
cmake --build desktop/linux/build
ctest --test-dir desktop/linux/build --output-on-failure
```

Run:

```sh
desktop/linux/build/VoiceStick
```

## Config

```text
~/.config/VoiceStick/config.toml
~/.local/share/VoiceStick/DebugAudio/
~/.local/share/VoiceStick/VoiceStickApp.log
```

Copy [Config/config.example.toml](Config/config.example.toml) if you want a
starting file. First launch also opens an onboarding wizard.

## Paste on GNOME / Wayland

Recognized text is always written to the clipboard. VoiceStick then tries
AT-SPI insert. If that fails, it asks GNOME for a Remote Desktop keyboard
share (`xdg-desktop-portal` `CreateSession` → `SelectDevices` → `Start`) and
injects Ctrl+V. If "Press Return After Paste" is on, Enter is sent 120ms later
so the focused app can finish pasting first. IBus cannot commit into another
app's focused context on mutter. Deny the dialog and a notification still
says to press Ctrl+V.

`wl-copy`, `wtype`, and `/dev/uinput` are not required.

## Notes

- BLE uses BlueZ over D-Bus. The user should be in the `bluetooth` group.
- Overlay windows are undecorated GTK4 toplevels. GNOME has no layer-shell.
- App self-update is not included; use your package manager or rebuild.
- Shared C++ with Windows is still copied, not extracted to `desktop/shared`.
