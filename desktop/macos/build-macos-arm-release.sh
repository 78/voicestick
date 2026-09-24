#!/bin/bash
# Build VoiceStick for macOS as an ARM-only app bundle with local build
# conveniences from the install script, but without installing to /Applications.
#
# Produces:
#   ../../build/VoiceStick-<version>.app
#   ../../build/VoiceStick-<version>.zip
#   ../../build/VoiceStick-<version>.signature  (when Sparkle sign_update is available)
#
# Optional environment:
#   VOICESTICK_APPCAST_URL=https://78.github.io/voicestick/appcast.xml
#   SPARKLE_PUBLIC_ED_KEY=<public key from Sparkle generate_keys>
#   SPARKLE_PRIVATE_ED_KEY=<private key exported by Sparkle generate_keys -x>
#   SPARKLE_KEY_ACCOUNT=voicestick
#   ALLOW_ADHOC_RELEASE=1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DESKTOP_DIR="$SCRIPT_DIR"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BUILD_HOME="${VOICESTICK_BUILD_HOME:-/private/tmp/voicestick-home}"
SCRATCH_DIR="${VOICESTICK_SCRATCH_DIR:-/private/tmp/voicestick-scratch-arm64}"
SOURCE_PLIST="$DESKTOP_DIR/Sources/VoiceStickApp/Info.plist"
ENTITLEMENTS="$DESKTOP_DIR/Resources/VoiceStick.entitlements"
VERSION="$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")"
CONFIG="${1:---release}"
TARGET_ARCHS="arm64"
SPARKLE_KEY_ACCOUNT="${SPARKLE_KEY_ACCOUNT:-voicestick}"

case "$CONFIG" in
    --release)
        SWIFT_CONFIG="release"
        ;;
    --debug)
        SWIFT_CONFIG="debug"
        ;;
    *)
        echo "Usage: $0 [--release|--debug]"
        exit 1
        ;;
esac

if [ -z "$VERSION" ]; then
    echo "Error: VERSION is empty"
    exit 1
fi

mkdir -p "$BUILD_DIR"
mkdir -p "$BUILD_HOME"
mkdir -p "$SCRATCH_DIR"

if [ -d "/Applications/Xcode.app/Contents/Developer" ]; then
    export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
fi

echo "===================================="
echo " VoiceStick macOS Build v$VERSION"
echo " Apple Silicon Binary: $TARGET_ARCHS"
echo "===================================="

if [ -n "${SPARKLE_PUBLIC_ED_KEY:-}" ]; then
    :
elif /usr/libexec/PlistBuddy -c "Print :SUPublicEDKey" "$SOURCE_PLIST" | grep -q "REPLACE_WITH"; then
    echo "WARNING: SUPublicEDKey is still a placeholder."
    echo "         Generate Sparkle keys before shipping a public release."
fi

echo ""
echo "Building VoiceStickApp for $TARGET_ARCHS..."
HOME="$BUILD_HOME" \
CLANG_MODULE_CACHE_PATH="$BUILD_HOME/clang-module-cache" \
swift build \
    --package-path "$DESKTOP_DIR" \
    -c "$SWIFT_CONFIG" \
    --arch "$TARGET_ARCHS" \
    --disable-sandbox \
    --scratch-path "$SCRATCH_DIR"

APP_DIR="$BUILD_DIR/VoiceStick-${VERSION}.app"
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources" "$APP_DIR/Contents/Frameworks"

ARM_BUILD="$SCRATCH_DIR/arm64-apple-macosx/$SWIFT_CONFIG"

echo ""
echo "Copying ARM executable..."
cp "$ARM_BUILD/VoiceStickApp" "$APP_DIR/Contents/MacOS/VoiceStickApp"

BUNDLE_PLIST="$APP_DIR/Contents/Info.plist"
cp "$SOURCE_PLIST" "$BUNDLE_PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$BUNDLE_PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$BUNDLE_PLIST"

if [ -n "${VOICESTICK_APPCAST_URL:-}" ]; then
    /usr/libexec/PlistBuddy -c "Set :SUFeedURL $VOICESTICK_APPCAST_URL" "$BUNDLE_PLIST"
fi

if [ -n "${SPARKLE_PUBLIC_ED_KEY:-}" ]; then
    /usr/libexec/PlistBuddy -c "Set :SUPublicEDKey $SPARKLE_PUBLIC_ED_KEY" "$BUNDLE_PLIST"
fi

ICON_PATH="$DESKTOP_DIR/Resources/AppIcon.icns"
if [ -f "$ICON_PATH" ]; then
    cp "$ICON_PATH" "$APP_DIR/Contents/Resources/AppIcon.icns"
else
    echo "WARNING: App icon was not found: $ICON_PATH"
fi

SPARKLE_FRAMEWORK="$(find -L "$SCRATCH_DIR/artifacts" -name Sparkle.framework -type d 2>/dev/null | head -1 || true)"
if [ -n "$SPARKLE_FRAMEWORK" ]; then
    cp -R "$SPARKLE_FRAMEWORK" "$APP_DIR/Contents/Frameworks/"
    install_name_tool -add_rpath "@loader_path/../Frameworks" "$APP_DIR/Contents/MacOS/VoiceStickApp" 2>/dev/null || true
else
    echo "WARNING: Sparkle.framework was not found in SwiftPM artifacts."
fi

CODESIGN_IDENTITY="-"
if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID Application"; then
    CODESIGN_IDENTITY="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk -F'"' '{print $2}')"
fi

if [ "$SWIFT_CONFIG" = "release" ] && [ "$CODESIGN_IDENTITY" = "-" ] && [ "${ALLOW_ADHOC_RELEASE:-0}" != "1" ]; then
    echo "Error: Developer ID Application signing identity was not found."
    echo "       Release builds should be Developer ID signed for stable macOS permissions and Sparkle updates."
    echo "       For local testing only, rerun with ALLOW_ADHOC_RELEASE=1."
    exit 1
fi

echo ""
echo "Signing app..."
xattr -cr "$APP_DIR" 2>/dev/null || true
if [ "$CODESIGN_IDENTITY" != "-" ]; then
    echo "Using: $CODESIGN_IDENTITY"
    codesign --deep --force --entitlements "$ENTITLEMENTS" --sign "$CODESIGN_IDENTITY" "$APP_DIR"
else
    echo "Using ad-hoc signature."
    codesign --deep --force --entitlements "$ENTITLEMENTS" --sign - "$APP_DIR"
fi

echo "Verifying app signature..."
codesign --verify --deep --strict --verbose=2 "$APP_DIR"

ZIP_PATH="$BUILD_DIR/VoiceStick-${VERSION}.zip"
SIGNATURE_PATH="${ZIP_PATH%.zip}.signature"
STAGING_DIR="$BUILD_DIR/.sparkle-staging"
rm -rf "$STAGING_DIR" "$ZIP_PATH" "$SIGNATURE_PATH"
mkdir -p "$STAGING_DIR"
ditto --norsrc --noextattr "$APP_DIR" "$STAGING_DIR/VoiceStick.app"

echo ""
echo "Creating Sparkle ZIP..."
ditto -c -k --norsrc --noextattr --keepParent "$STAGING_DIR/VoiceStick.app" "$ZIP_PATH"
rm -rf "$STAGING_DIR"

SIGN_TOOL="$(find -L "$SCRATCH_DIR/artifacts" -name sign_update -type f 2>/dev/null | head -1 || true)"
if [ -n "$SIGN_TOOL" ] && [ -x "$SIGN_TOOL" ]; then
    echo "Signing Sparkle ZIP..."
    if [ -n "${SPARKLE_PRIVATE_ED_KEY:-}" ]; then
        SIGN_OUTPUT="$(printf '%s' "$SPARKLE_PRIVATE_ED_KEY" | "$SIGN_TOOL" --ed-key-file - "$ZIP_PATH" 2>&1 || true)"
    else
        SIGN_OUTPUT="$("$SIGN_TOOL" --account "$SPARKLE_KEY_ACCOUNT" "$ZIP_PATH" 2>&1 || true)"
    fi
    echo "$SIGN_OUTPUT"
    ED_SIGNATURE="$(printf '%s\n' "$SIGN_OUTPUT" | sed -nE 's/.*sparkle:edSignature="([^"]+)".*/\1/p' | head -1)"
    if [ -n "$ED_SIGNATURE" ]; then
        printf '%s\n' "$ED_SIGNATURE" > "$SIGNATURE_PATH"
    else
        rm -f "$SIGNATURE_PATH"
        echo "WARNING: Sparkle ZIP signature was not generated."
    fi
else
    echo "WARNING: Sparkle sign_update tool was not found."
fi

echo ""
echo "Build complete:"
echo "  App: $APP_DIR"
echo "  ZIP: $ZIP_PATH"
if [ -f "$SIGNATURE_PATH" ]; then
    echo "  Sig: $SIGNATURE_PATH"
fi
echo ""
echo "Next: $ROOT_DIR/scripts/make-dmg.sh $APP_DIR"
