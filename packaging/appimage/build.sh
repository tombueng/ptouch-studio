#!/usr/bin/env bash
# Builds an AppImage that runs on any reasonably current distribution.
#
# Qt is bundled but stays dynamically linked (LGPL-3.0): the libraries can be
# replaced, and Qt's sources are available from https://download.qt.io.
#
# Usage:  packaging/appimage/build.sh [build-directory]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-$ROOT/build-appimage}"
APPDIR="$BUILD/AppDir"
APP_ID="io.github.tombueng.PtouchStudio"
ARCH="$(uname -m)"

echo "==> compiling"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF
cmake --build "$BUILD"

echo "==> assembling AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD"

# The CUPS backend belongs in the system, not in the AppImage — it has to run as
# root from /usr/lib/cups/backend. It ships alongside instead.
mkdir -p "$BUILD/dist"
cp "$APPDIR/usr/lib/cups/backend/rfcomm" "$BUILD/dist/ptouch-cups-backend-$ARCH"
rm -rf "$APPDIR/usr/lib/cups"

# files AppDir requires at its root
cp "$APPDIR/usr/share/applications/$APP_ID.desktop" "$APPDIR/"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png" "$APPDIR/"
ln -sf "$APP_ID.png" "$APPDIR/.DirIcon"

echo "==> fetching tools"
TOOLS="$BUILD/tools"
mkdir -p "$TOOLS"
fetch() {
    local url="$1" out="$2"
    [ -f "$out" ] || curl -fsSL "$url" -o "$out"
    chmod +x "$out"
}
BASE="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
fetch "$BASE/linuxdeploy-$ARCH.AppImage" "$TOOLS/linuxdeploy"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" \
      "$TOOLS/linuxdeploy-plugin-qt"

# Inside containers without FUSE the AppImages have to be extracted.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export EXTRA_QT_PLUGINS="platformthemes;iconengines;imageformats"
export PATH="$TOOLS:$PATH"

echo "==> building AppImage"
"$TOOLS/linuxdeploy" \
    --appdir "$APPDIR" \
    --plugin qt \
    --output appimage \
    --desktop-file "$APPDIR/$APP_ID.desktop" \
    --icon-file "$APPDIR/$APP_ID.png"

mv ./*.AppImage "$BUILD/dist/" 2>/dev/null || mv "$BUILD"/*.AppImage "$BUILD/dist/"
echo "==> done:"
ls -la "$BUILD/dist/"
