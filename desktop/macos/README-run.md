# Running the ARM-only macOS Script

## English

Run the script from the macOS app folder:

```sh
cd desktop/macos
./build-macos-arm-release.sh --debug
```

Release build with Sparkle public key:

```sh
cd desktop/macos
SPARKLE_PUBLIC_ED_KEY="your_public_key_here" ./build-macos-arm-release.sh --release
```

If the script is not executable yet:

```sh
chmod +x desktop/macos/build-macos-arm-release.sh
```

## Русский

Запускай скрипт из папки macOS-приложения:

```sh
cd desktop/macos
./build-macos-arm-release.sh --debug
```

Релизная сборка с публичным ключом Sparkle:

```sh
cd desktop/macos
SPARKLE_PUBLIC_ED_KEY="your_public_key_here" ./build-macos-arm-release.sh --release
```

Если скрипт ещё не исполняемый:

```sh
chmod +x desktop/macos/build-macos-arm-release.sh
```
