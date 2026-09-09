# build-macos-arm-release.sh

## English

This file preserves the ARM-only macOS release builder for VoiceStick.
It combines the release packaging path with the local-build conveniences from
`install-macos-local.sh`, but it does not install anything into `/Applications`.

What it does:
- builds only `arm64`
- packages `build/VoiceStick-<version>.app`
- embeds `Sparkle.framework`
- signs the app bundle
- creates `build/VoiceStick-<version>.zip`
- uses local scratch/home directories by default to avoid touching shared caches
- honors `VOICESTICK_BUILD_HOME` and `VOICESTICK_SCRATCH_DIR`
- uses `Xcode.app` automatically when it is installed

Usage:

```sh
SPARKLE_PUBLIC_ED_KEY="..." ./build-macos-arm-release.sh --release
```

Examples:

```sh
./build-macos-arm-release.sh --debug
SPARKLE_PUBLIC_ED_KEY="..." ./build-macos-arm-release.sh --release
```

Optional environment:
- `VOICESTICK_APPCAST_URL` sets the Sparkle feed URL in `Info.plist`
- `SPARKLE_PUBLIC_ED_KEY` writes the public Sparkle key into `Info.plist`
- `SPARKLE_PRIVATE_ED_KEY` signs the ZIP with a local private key
- `SPARKLE_KEY_ACCOUNT` selects the Sparkle keychain account name when no private key is provided
- `ALLOW_ADHOC_RELEASE=1` allows an unsigned local release test when Developer ID is unavailable

Notes:
- The script expects the macOS Swift package under `desktop/macos`.
- The script writes version and Sparkle values only into the bundled app plist, not into the source plist.
- It keeps the SwiftPM scratch/cache under `/private/tmp` by default, unless
  you override the env vars above.
- If you need a DMG afterward, run `scripts/make-dmg.sh` with the built app path.
- It intentionally does not copy the app into `/Applications`.

## Русский

Этот файл сохраняет ARM-only сценарий сборки релиза VoiceStick для macOS.
Он объединяет релизную упаковку и удобства локальной сборки из
`install-macos-local.sh`, но не устанавливает приложение в `/Applications`.

Что делает скрипт:
- собирает только `arm64`
- упаковывает `build/VoiceStick-<version>.app`
- встраивает `Sparkle.framework`
- подписывает app bundle
- создаёт `build/VoiceStick-<version>.zip`
- использует локальные `home` и `scratch` каталоги по умолчанию, чтобы не трогать общие кеши
- использует `VOICESTICK_BUILD_HOME` и `VOICESTICK_SCRATCH_DIR`
- автоматически подхватывает `Xcode.app`, если он установлен

Использование:

```sh
SPARKLE_PUBLIC_ED_KEY="..." ./build-macos-arm-release.sh --release
```

Примеры:

```sh
./build-macos-arm-release.sh --debug
SPARKLE_PUBLIC_ED_KEY="..." ./build-macos-arm-release.sh --release
```

Полезные переменные окружения:
- `VOICESTICK_APPCAST_URL` записывает URL фида Sparkle в `Info.plist`
- `SPARKLE_PUBLIC_ED_KEY` записывает публичный ключ Sparkle в `Info.plist`
- `SPARKLE_PRIVATE_ED_KEY` подписывает ZIP локальным приватным ключом
- `SPARKLE_KEY_ACCOUNT` выбирает имя аккаунта ключа в keychain, если приватный ключ не передан
- `ALLOW_ADHOC_RELEASE=1` разрешает локальный тест релиза без Developer ID

Примечания:
- Скрипт ожидает macOS Swift package в `desktop/macos`.
- Версия и значения Sparkle записываются только в plist внутри собранного приложения, исходный plist не меняется.
- Он хранит SwiftPM scratch/cache в `/private/tmp` по умолчанию, если не переопределить переменные выше.
- Если потом нужен DMG, запусти `scripts/make-dmg.sh` с путём к собранному приложению.
- Он намеренно не копирует приложение в `/Applications`.
