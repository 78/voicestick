# VoiceStick Release Process

VoiceStick releases have three moving parts:

- macOS app: built, signed, notarized, and uploaded by GitHub Actions.
- StickS3, VoiceStick C3, and MagiClick 2P5 firmware: built by GitHub Actions and uploaded to Aliyun OSS and GitHub Releases.
- Windows app: built and signed manually on the Windows signing machine, then uploaded to the matching GitHub Release.

The Windows package is the special case because the signing certificate is local hardware or local machine state. The release process supports either order:

- Build and sign Windows first, then let GitHub Actions publish macOS and firmware.
- Publish macOS and firmware first, then build/sign Windows and upload it afterward.

In both cases, finish by redeploying the website and verifying all update URLs.

## Version Sources

Update both version files before creating a combined desktop and firmware release tag:

```text
VERSION
firmware/version.txt
```

`VERSION` is used by the desktop packaging scripts and the GitHub release workflow. `firmware/version.txt` is the firmware version reported by the device, so it must match the release version for OTA update detection to work correctly.

Set any required minimum desktop app versions in
`firmware/minimum-app-versions.json`. The workflow includes this map as
`minimum_app_versions` in the OTA manifest. Supported keys are `macos`, `windows`,
and `linux`. An omitted key leaves that platform without an app-version
minimum. Use `{}` when the release needs no minimums. The platforms share the
same firmware files for a given board.

For release `0.2.4`, the tag must be:

```text
v0.2.4
```

The GitHub Actions release workflow validates that `v<VERSION>` matches the pushed tag.

## Firmware-Only Release

Set the firmware version in `firmware/version.txt` and push the matching
`firmware-v<version>` tag. The workflow builds all three boards,
publishes the OTA and merged images, and updates their OSS manifests. The
desktop apps keep their separate version and release tags.

For the firmware that introduces BLE bonding, release macOS and firmware together
with `v0.3.7`. The updated macOS app can connect to older firmware before upgrading
the device. Test pairing after the upgrade using the device's pairing button.

Each board's `latest/manifest.json` can include `minimum_app_versions`.
Clients that implement this check apply their platform's minimum when an entry
is present. An omitted field or platform entry means every app version meets
the version requirement for that platform. Older apps ignore the field.
GitHub Pages serves the manifests for current clients. The release also
updates the existing OSS manifests for installed clients that use those URLs.
Verify that both Pages manifests report the new firmware and minimum app
versions and that their OTA and merged URLs are reachable.

## macOS-Only Release

Set `VERSION` to the macOS release version, commit and push the release commit,
then push a matching `macos-v<version>` tag. For example, `macos-v0.4.2`
builds, signs, notarizes, and publishes the macOS packages. The release workflow
then redeploys the website. Verify the macOS DMG link and Sparkle appcast entry.

## Windows-Only Release

Set `VERSION` to the Windows release version, then build and verify the signed
MSI on the Windows signing machine. Commit and push the release commit, then
create a stable GitHub Release with the MSI and a `windows-v<version>` tag.
For example, `windows-v0.4.2` publishes `VoiceStick_0.4.2.msi`. Mark the release
as non-latest so the repository's latest release continues to serve the combined
desktop downloads. Run `deploy-website.yml` from `main` and verify the Windows
appcast entry and MSI download URL.

## Standard Flow

1. Update `VERSION` and `firmware/version.txt` to the new version.
2. Commit the version change and any release workflow changes.
3. Push `main`.
4. Push the release tag:

```sh
git tag -a v0.2.4 -m "VoiceStick 0.2.4"
git push origin main
git push origin v0.2.4
```

Pushing the tag runs `.github/workflows/release.yml`. That workflow builds:

- `VoiceStick-<version>.dmg`
- `VoiceStick-<version>.zip`
- `VoiceStick-<version>.signature`
- `voicestick-firmware-sticks3-ota-<version>.bin`
- `voicestick-firmware-sticks3-merged-<version>.bin`
- `voicestick-firmware-c3-ota-<version>.bin` and `voicestick-firmware-c3-merged-<version>.bin`
- `voicestick-firmware-magiclick_2p5-ota-<version>.bin` and `voicestick-firmware-magiclick_2p5-merged-<version>.bin`
- Firmware checksums and a manifest for each board. The release uses `manifest.json` for StickS3, `manifest-voicestick_c3.json` for C3, and `manifest-magiclick_2p5.json` for MagiClick 2P5.

It also uploads the firmware to Aliyun OSS under both:

```text
voicestick/firmwares/<version>/
voicestick/firmwares/latest/
voicestick/firmwares/voicestick_c3/<version>/
voicestick/firmwares/voicestick_c3/latest/
voicestick/firmwares/magiclick_2p5/<version>/
voicestick/firmwares/magiclick_2p5/latest/
```

After publishing a combined or firmware-only GitHub Release, the workflow
requests a Pages deployment. The deployment refreshes the appcast and copies
the StickS3 and C3 manifests from OSS. The MagiClick 2P5 manifest is available
on GitHub Releases and OSS.

## Windows First

Use this flow when the Windows package has already been built and signed before the macOS/firmware release.

1. Set the new version in `VERSION`.
2. On the Windows signing machine, build and sign the MSI:

```bat
scripts\build-msi.bat
```

The output is:

```text
desktop\windows\build-msi-x64\VoiceStick_<version>.msi
```

3. Confirm `firmware/version.txt` also matches the new version.
4. Commit, push `main`, and push the matching `v<version>` tag.
5. Wait for the release workflow to finish successfully.
6. Upload the signed MSI to the same GitHub Release:

```sh
gh release upload v0.2.4 desktop/windows/build-msi-x64/VoiceStick_0.2.4.msi --repo 78/voicestick
```

7. Re-run the website deploy workflow so the appcast includes the Windows MSI:

```sh
gh workflow run deploy-website.yml --repo 78/voicestick --ref main
```

## macOS and Firmware First

Use this flow when macOS and firmware should be published before the Windows package is ready.

1. Update `VERSION` and `firmware/version.txt`.
2. Commit, push `main`, and push the matching `v<version>` tag.
3. Wait for the release workflow to publish macOS and firmware.
4. Later, on the Windows signing machine, build and sign the MSI:

```bat
scripts\build-msi.bat
```

5. Upload the signed MSI to the already published GitHub Release:

```sh
gh release upload v0.2.4 desktop/windows/build-msi-x64/VoiceStick_0.2.4.msi --repo 78/voicestick
```

6. Re-run the website deploy workflow:

```sh
gh workflow run deploy-website.yml --repo 78/voicestick --ref main
```

Until the MSI is uploaded and the website deploy has run, Windows clients will not see the new Windows update in the appcast.

## Verification

After every release, verify the appcast, firmware manifest, and actual package URLs.

Stable update endpoints:

```text
https://78.github.io/voicestick/appcast.xml
https://78.github.io/voicestick/firmwares/m5stack_stick_s3/latest/manifest.json
https://78.github.io/voicestick/firmwares/voicestick_c3/latest/manifest.json
```

For version `0.2.4`, the appcast should contain:

```text
https://github.com/78/voicestick/releases/download/v0.2.4/VoiceStick_0.2.4.msi
https://github.com/78/voicestick/releases/download/v0.2.4/VoiceStick-0.2.4.zip
```

The firmware manifest should contain:

```text
https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/0.2.4/voicestick-firmware-sticks3-ota-0.2.4.bin
https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/0.2.4/voicestick-firmware-sticks3-merged-0.2.4.bin
```

Use `HEAD` requests or a browser to confirm every URL returns `200`.

```powershell
Invoke-WebRequest -UseBasicParsing https://78.github.io/voicestick/appcast.xml
Invoke-WebRequest -UseBasicParsing https://78.github.io/voicestick/firmwares/m5stack_stick_s3/latest/manifest.json
Invoke-WebRequest -UseBasicParsing https://78.github.io/voicestick/firmwares/voicestick_c3/latest/manifest.json

Invoke-WebRequest -UseBasicParsing -Method Head https://github.com/78/voicestick/releases/download/v0.2.4/VoiceStick_0.2.4.msi
Invoke-WebRequest -UseBasicParsing -Method Head https://github.com/78/voicestick/releases/download/v0.2.4/VoiceStick-0.2.4.zip
Invoke-WebRequest -UseBasicParsing -Method Head https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/0.2.4/voicestick-firmware-sticks3-ota-0.2.4.bin
Invoke-WebRequest -UseBasicParsing -Method Head https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/0.2.4/voicestick-firmware-sticks3-merged-0.2.4.bin
```

Also confirm these workflow runs are successful:

- `Release Build`
- `Deploy Website to GitHub Pages`

The release is complete when:

- macOS appcast entry points to the new Sparkle ZIP.
- Windows appcast entry points to the new signed MSI.
- both `latest/manifest.json` files report the new firmware and minimum app versions.
- OTA and merged firmware URLs are reachable.
- the GitHub Release contains all macOS, Windows, and firmware assets.
