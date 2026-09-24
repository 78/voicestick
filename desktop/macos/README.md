# VoiceStick macOS

Swift/AppKit menu bar app for VoiceStick on macOS.

## Run Locally

```sh
swift build -c debug --arch arm64
```

For an ARM-only app bundle, use:

```sh
./build-macos-arm-release.sh --debug
```

The release script creates `build/VoiceStick-<version>.app` and does not install it into `/Applications`.

## Configuration

The app stores its config at:

```text
~/Library/Application Support/VoiceStick/config.toml
```

OpenAI speech-to-text uses `llm_api_key`, the same key used by translation. During first-run setup, choosing OpenAI as the ASR provider saves the entered key for both OpenAI ASR and translation.

## macOS Permissions

To paste recognized text into the focused app, VoiceStick needs Accessibility permission:

```text
System Settings > Privacy & Security > Accessibility
```

For stable permissions, test the same installed app bundle path and avoid rebuilding/re-signing the app between permission checks.

## Debug Logging

Verbose logs are off by default. Enable them only when diagnosing a problem:

```sh
defaults write app.voicestick.mac VoiceStickDebugLogging -bool true
```

or launch with:

```sh
VOICESTICK_DEBUG_LOGS=1 VoiceStickApp
```

Disable defaults-based logging:

```sh
defaults delete app.voicestick.mac VoiceStickDebugLogging
```

## Русский

Это Swift/AppKit приложение VoiceStick для macOS, которое живёт в menu bar.

### Локальный запуск

```sh
swift build -c debug --arch arm64
```

Для ARM-only app bundle:

```sh
./build-macos-arm-release.sh --debug
```

Скрипт создаёт `build/VoiceStick-<version>.app` и сам не устанавливает приложение в `/Applications`.

### Конфигурация

Конфиг хранится здесь:

```text
~/Library/Application Support/VoiceStick/config.toml
```

OpenAI speech-to-text использует `llm_api_key`, то есть тот же ключ, что и переводчик. При первичной настройке, если выбрать OpenAI как ASR-провайдер, введённый ключ сохраняется сразу и для распознавания, и для перевода.

### Права macOS

Для вставки распознанного текста в активное приложение нужен Accessibility-доступ:

```text
System Settings > Privacy & Security > Accessibility
```

Чтобы macOS не путалась с разрешениями, тестируй один и тот же установленный app bundle и не пересобирай/не переподписывай его между проверками.

### Debug-логи

Подробные логи по умолчанию выключены. Включай их только для диагностики:

```sh
defaults write app.voicestick.mac VoiceStickDebugLogging -bool true
```

или запускай с переменной:

```sh
VOICESTICK_DEBUG_LOGS=1 VoiceStickApp
```

Выключить debug-логи:

```sh
defaults delete app.voicestick.mac VoiceStickDebugLogging
```
