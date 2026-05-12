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
- Text insertion through clipboard plus `SendInput`.
- App self-update checks through WinSparkle, using the same appcast feed as the macOS Sparkle updater.

Full BLE GATT characteristic I/O is intentionally left for follow-up hardware validation work.

## Build

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

Upload `desktop\windows\build-msi-x64\VoiceStick_<version>.msi` to the matching GitHub Release, then manually run the `Deploy Website to GitHub Pages` workflow. The MSI is the WinSparkle update package; the appcast entry records the GitHub Release MSI URL, byte length, version, Windows OS marker, and `/passive` installer argument.

If an older `desktop\windows\build` directory was configured from the wrong Visual Studio environment, delete it or ignore it. Mixing an x86 CMake cache with x64 SDK libraries causes link errors.

## Notes

Code follows Google C++ naming style: `snake_case` file names, `CapWords` types, `MixedCase()` functions/methods, `snake_case` variables, and 4-space indentation.
