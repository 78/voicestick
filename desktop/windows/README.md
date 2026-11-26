# VoiceStick Windows

Native Win32/C++20 implementation workspace for the Windows desktop client.

## Windows support

VoiceStick supports 64-bit Windows 10 version 1903 / 1909 and newer across
normal Windows 10 editions, including Education. Newer builds expose the BLE
advertisement address type directly; 2019 Windows 10 builds fall back to the
address-only BLE connection path.

Current scope:

- Win32 tray app shell with overlay window.
- Windows config at `%APPDATA%\VoiceStick\config.toml`.
- Debug audio cache at `%LOCALAPPDATA%\VoiceStick\DebugAudio`.
- BLE advertisement scanning through C++/WinRT.
- Core VoiceStick protocol parsing, Ogg Opus muxing, ASR binary framing, and coordinator state machine.
- Tray menu **Send Confirmation** offers immediate paste, a 1, 2, 3, 5, 10, or 15 second countdown, or manual confirmation. Press the main button during the countdown to pause. In manual confirmation, edit the draft, then choose **Send** or press the main button. Choose **Cancel**, use the C3 side button, or shake a StickS3 or MagiClick 2P5 running firmware with motion support.
- Text insertion uses the clipboard and `SendInput`. If the original app cannot be focused or the paste fails, the confirmation window keeps the draft and offers **Paste Here**.
- Settings offers VoiceStick Cloud and OpenAI Realtime. Use **Text Output** in the device menu to choose Original, Refine, or a translation. VoiceStick Cloud uses the Cloud key for text processing. OpenAI Realtime uses the same OpenAI key for transcription and GPT-6 Luna text processing. Older Cloud relays can use the optional LLM fields in `config.toml`. A saved Volcengine provider selection migrates to VoiceStick Cloud and uses the Cloud key.
- App self-update checks through WinSparkle, using the same appcast feed as the macOS Sparkle updater.
- To open pairing with current firmware, press the main voice button while the device is disconnected. With 0.4.2 firmware, hold the secondary button for three seconds.
- StickS3 and VoiceStick C3 use separate firmware manifests. An optional `minimum_app_versions.windows` entry sets the minimum Windows app version for BLE updates. When it is present, older app versions are prompted to update VoiceStick first.
- C3 battery and charging status appear in the device menu and in the tray tooltip when a single device is connected.
- The device menu offers **Check for Firmware Updates** or **Update to** when an eligible update is available. A connected device can prompt for a firmware update when voice input is idle. VoiceStick remembers each device and target version already shown, including updates seen through a manual check.

## Build

CMake downloads libopus for OpenAI Realtime and includes its license in the MSI.

From a normal PowerShell prompt:

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S desktop\windows -B desktop\windows\build-x64 -G Ninja -DCMAKE_MAKE_PROGRAM="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"'
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C desktop\windows\build-x64'
```

Run tests:

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir desktop\windows\build-x64 --output-on-failure'
```

Run:

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

Build a signed MSI locally:

```powershell
$env:SIGNING_SHA1 = "YOUR_CERT_THUMBPRINT"
scripts\build-msi.bat
```

Upload `desktop\windows\build-msi-x64\VoiceStick_<version>.msi` to the matching GitHub Release, then manually run the `Deploy Website to GitHub Pages` workflow. WinSparkle passes `/qb!+ /norestart VOICESTICK_RELAUNCH=1` to the MSI. Windows Installer shows the installation result and relaunches VoiceStick for the signed-in user after a successful upgrade.

If an older `desktop\windows\build` directory was configured from the wrong Visual Studio environment, delete it or ignore it. Mixing an x86 CMake cache with x64 SDK libraries causes link errors.

## Notes

Code follows Google C++ naming style: `snake_case` file names, `CapWords` types, `MixedCase()` functions/methods, `snake_case` variables, and 4-space indentation.
