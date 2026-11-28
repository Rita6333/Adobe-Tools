#!/usr/bin/env bash
# Adobe Tools (by RITA) — macOS Universal App (.app) + Installer (.dmg & .pkg) Builder
# 在 macOS (Xcode Command Line Tools) 环境下执行:
#   chmod +x macos/build-macos.sh && ./macos/build-macos.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist/macos"
APP_NAME="Adobe Tools"
BUNDLE_ID="com.rita.adobetools"
VERSION="2.2.1"
APP_BUNDLE="$DIST_DIR/${APP_NAME}.app"
DMG_OUT="$DIST_DIR/Adobe-Tools-macOS-Universal.dmg"
PKG_OUT="$DIST_DIR/Adobe-Tools-macOS-Universal.pkg"

rm -rf "$DIST_DIR"
mkdir -p "$APP_BUNDLE/Contents/MacOS" "$APP_BUNDLE/Contents/Resources"

echo "[1/5] Generating macOS AppIcon.icns from docs/logo.png..."
ICONSET_DIR="$DIST_DIR/AppIcon.iconset"
mkdir -p "$ICONSET_DIR"
LOGO_SRC="$ROOT_DIR/docs/logo.png"
if [ -f "$LOGO_SRC" ]; then
  for sz in 16 32 128 256 512; do
    sips -z "$sz" "$sz" "$LOGO_SRC" --out "$ICONSET_DIR/icon_${sz}x${sz}.png" >/dev/null
    sz2=$((sz * 2))
    sips -z "$sz2" "$sz2" "$LOGO_SRC" --out "$ICONSET_DIR/icon_${sz}x${sz}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET_DIR" -o "$APP_BUNDLE/Contents/Resources/AppIcon.icns"
  rm -rf "$ICONSET_DIR"
fi

echo "[2/5] Compiling native Objective-C++17 Universal Binary (arm64 + x86_64)..."
BIN_PATH="$APP_BUNDLE/Contents/MacOS/$APP_NAME"
if clang++ -std=c++17 -O2 -ObjC++ -fobjc-arc \
    -arch arm64 -arch x86_64 \
    -mmacosx-version-min=11.0 \
    -framework Cocoa -framework WebKit \
    "$SCRIPT_DIR/main.mm" -o "$BIN_PATH" 2>/dev/null; then
  echo "      Built Universal Binary (arm64 + x86_64)"
else
  echo "      Universal target unavailable; building for host architecture ($(uname -m))..."
  clang++ -std=c++17 -O2 -ObjC++ -fobjc-arc \
    -mmacosx-version-min=11.0 \
    -framework Cocoa -framework WebKit \
    "$SCRIPT_DIR/main.mm" -o "$BIN_PATH"
fi

echo "[3/5] Packaging App Bundle resources & Info.plist..."
cp "$ROOT_DIR/src/ui.html" "$APP_BUNDLE/Contents/Resources/ui.html"
cp "$ROOT_DIR/adobe-domains.txt" "$APP_BUNDLE/Contents/Resources/adobe-domains.txt"

cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>zh_CN</string>
  <key>CFBundleDisplayName</key>
  <string>${APP_NAME}</string>
  <key>CFBundleExecutable</key>
  <string>${APP_NAME}</string>
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>CFBundleIdentifier</key>
  <string>${BUNDLE_ID}</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>${APP_NAME}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${VERSION}</string>
  <key>CFBundleVersion</key>
  <string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key>
  <string>11.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>NSHumanReadableCopyright</key>
  <string>Copyright © RITA</string>
</dict>
</plist>
EOF

# Run 13-point native selftest before packaging
"$BIN_PATH" selftest

# Ad-hoc sign so Apple Silicon macOS executes cleanly
codesign --force --deep --sign - "$APP_BUNDLE"

echo "[4/5] Building macOS Drag-to-Applications DMG ($DMG_OUT)..."
DMG_STAGE="$DIST_DIR/dmg-stage"
mkdir -p "$DMG_STAGE"
cp -R "$APP_BUNDLE" "$DMG_STAGE/"
ln -s /Applications "$DMG_STAGE/Applications"
hdiutil create -volname "$APP_NAME" -srcfolder "$DMG_STAGE" -ov -format UDZO "$DMG_OUT" >/dev/null
rm -rf "$DMG_STAGE"

echo "[5/5] Building macOS Native Installer PKG ($PKG_OUT)..."
COMPONENT_PKG="$DIST_DIR/AdobeTools-Component.pkg"
pkgbuild \
  --component "$APP_BUNDLE" \
  --install-location "/Applications" \
  --identifier "$BUNDLE_ID" \
  --version "$VERSION" \
  "$COMPONENT_PKG" >/dev/null

productbuild \
  --package "$COMPONENT_PKG" \
  "$PKG_OUT" >/dev/null
rm -f "$COMPONENT_PKG"

echo ""
echo "Done! Built macOS installers:"
ls -lh "$DMG_OUT" "$PKG_OUT"
