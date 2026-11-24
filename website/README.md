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

The generated `dist/` directory is deployed to GitHub Pages. It includes `appcast.xml` and each board's manifest at `firmwares/<board>/latest/manifest.json`. The deployment copies the manifests from OSS into the `m5stack_stick_s3` and `voicestick_c3` directories. The firmware image URLs point to OSS.

## GitHub Actions

- `.github/workflows/release.yml` runs on `v*` or `firmware-v*` tags or manual dispatch. It publishes versioned firmware images and manifests to OSS and GitHub Releases. Combined releases also publish the signed macOS app. The release workflow then requests a GitHub Pages deployment.
- `scripts/build-msi.bat` runs on the local Windows signing machine with the USB signing key inserted. Upload the generated `VoiceStick_<version>.msi` to the matching GitHub Release.
- `.github/workflows/deploy-website.yml` runs on website changes on `main`, after a release, or when manually dispatched after uploading a Windows MSI. It regenerates `public/appcast.xml` from the latest ZIP/signature and MSI assets, copies both OTA manifests, and deploys the built website. The macOS and Windows packages can come from different releases.

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
