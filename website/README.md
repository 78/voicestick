# VoiceStick Website

Vue + Vite source for the VoiceStick homepage and Sparkle/WinSparkle appcast. The site uses
`vue-i18n` for Simplified Chinese and English, and picks Chinese automatically
when the browser language starts with `zh`.

Suggested GitHub Pages URL for this repository:

```text
https://78.github.io/voicestick/
```

The macOS and Windows apps check the generated root-level appcast:

```text
https://78.github.io/voicestick/appcast.xml
```

## Release Flow

1. Generate Sparkle keys once and keep the private key out of git:

   ```bash
   cd desktop/macos
   swift package resolve
   find .build -name generate_keys -type f -perm -111 -print -quit
   .build/artifacts/sparkle/Sparkle/bin/generate_keys --account voicestick
   ```

2. Put the public key into the app before a release build:

   ```bash
   SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
   ```

3. Create the DMG:

   ```bash
   scripts/make-dmg.sh
   ```

4. Upload these files to GitHub Release `v<version>`:

   ```text
   build/VoiceStick-<version>.dmg
   build/VoiceStick-<version>.zip
   ```

5. Update `website/public/appcast.xml`:

   - `url`: GitHub Release URL for the ZIP, not the DMG
   - `sparkle:edSignature`: content from `build/VoiceStick-<version>.signature`
   - `length`: byte size from `wc -c build/VoiceStick-<version>.zip`

The homepage download section links directly to the current versioned macOS DMG and Windows MSI. Keep `website/package.json` in sync with the latest public release so those direct links and the firmware fallback URL point at the right version. The browser flasher reads the firmware manifest from `VITE_FIRMWARE_MANIFEST_URL` and uses `merged_url`; if the manifest cannot be loaded, it falls back to the versioned merged firmware URL derived from `website/package.json`.

## Develop

```bash
cd website
npm install
npm run dev
```

## Build

```bash
cd website
npm run build
```

The generated `dist/` directory is deployed to GitHub Pages. `public/appcast.xml` is copied to `dist/appcast.xml`. The firmware manifest itself is hosted on OSS, not GitHub Pages.

## GitHub Actions

- `.github/workflows/release.yml` runs on `v*` tags or manual dispatch. It builds the macOS app, signs and notarizes the DMG, uploads DMG/ZIP/signature assets to the matching GitHub Release, builds versioned OTA and merged firmware images, publishes the firmware manifest to OSS, rewrites `public/appcast.xml`, builds the Vue site, and deploys `website/dist` to GitHub Pages.
- `scripts/build-msi.bat` runs on the local Windows signing machine with the USB signing key inserted. Upload the generated `VoiceStick_<version>.msi` to the matching GitHub Release.
- `.github/workflows/deploy-website.yml` runs when `website/**` changes on `main` or when manually dispatched after uploading a Windows MSI. Before deploying, it reads the current live appcast and latest GitHub Release, then regenerates `public/appcast.xml` from the latest ZIP/signature and optional MSI assets. If the latest Release has no Windows MSI yet, the previous Windows item is preserved.

Required repository secrets for release builds:

```text
SPARKLE_PUBLIC_ED_KEY
SPARKLE_PRIVATE_ED_KEY
MACOS_CERTIFICATE_P12
MACOS_CERTIFICATE_PASSWORD
APPLE_ID
APPLE_TEAM_ID
APPLE_APP_SPECIFIC_PASSWORD
```
