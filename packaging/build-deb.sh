#!/bin/bash
#
# build-deb.sh — Build the .deb package for Transcriber with bundled turbo model
#
# This script automates the full build process:
#   1. Checks for build dependencies
#   2. Downloads the Whisper turbo model (if not present)
#   3. Builds the application via CMake
#   4. Creates the .deb package using dpkg-deb
#
# Usage:
#   ./packaging/build-deb.sh [--download-model] [--cuda]
#   ./packaging/build-deb.sh --uninstall
#
# Options:
#   --download-model  Force download of the Whisper model (even if cached)
#   --cuda            Enable CUDA GPU acceleration
#   --uninstall       Remove files installed by CMake 'make install'
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-deb"
MODEL_DIR="${PROJECT_DIR}/models"
MODEL_FILE="${MODEL_DIR}/ggml-large-v3-turbo-q8_0.bin"

DOWNLOAD_MODEL=0
ENABLE_CUDA=0
UNINSTALL=0

# Parse arguments
for arg in "$@"; do
    case $arg in
        --download-model)
            DOWNLOAD_MODEL=1
            shift
            ;;
        --cuda)
            ENABLE_CUDA=1
            shift
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--download-model] [--cuda] [--uninstall]"
            echo ""
            echo "Options:"
            echo "  --download-model  Force download of the Whisper model"
            echo "  --cuda            Enable CUDA GPU acceleration"
            echo "  --uninstall       Remove files installed by 'make install'"
            echo "  --help, -h        Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            exit 1
            ;;
    esac
done

# Handle uninstall
if [ "$UNINSTALL" -eq 1 ]; then
    echo "============================================="
    echo "  Transcriber Uninstaller"
    echo "  (Removes files from CMake 'make install')"
    echo "============================================="
    echo ""

    INSTALL_PREFIX="/usr/local"

    FILES_TO_REMOVE=(
        "${INSTALL_PREFIX}/bin/transcriber"
        "${INSTALL_PREFIX}/share/applications/transcriber.desktop"
        "${INSTALL_PREFIX}/share/dbus-1/services/org.xvoice.Controller.service"
        "${INSTALL_PREFIX}/share/pixmaps/redmic.xpm"
        "${INSTALL_PREFIX}/share/pixmaps/greenmic.xpm"
        "${INSTALL_PREFIX}/share/pixmaps/gear.xpm"
        "${INSTALL_PREFIX}/share/icons/hicolor/apps/redmic.png"
        "${INSTALL_PREFIX}/share/icons/hicolor/apps/greenmic.png"
        "${INSTALL_PREFIX}/share/icons/hicolor/apps/gear.png"
    )

    REMOVED=0
    NOT_FOUND=0

    for file in "${FILES_TO_REMOVE[@]}"; do
        if [ -e "$file" ]; then
            rm -f "$file"
            echo "  Removed: $file"
            REMOVED=$((REMOVED + 1))
        else
            echo "  Not found (skipped): $file"
            NOT_FOUND=$((NOT_FOUND + 1))
        fi
    done

    # Update icon cache
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache "${INSTALL_PREFIX}/share/icons/hicolor/" >/dev/null 2>&1 || \
        gtk-update-icon-cache /usr/share/icons/hicolor/ >/dev/null 2>&1 || true
        echo "  Updated icon cache."
    fi

    echo ""
    echo "Uninstall complete: ${REMOVED} files removed, ${NOT_FOUND} not found."
    echo ""

    exit 0
fi

echo "============================================="
echo "  Transcriber .deb Package Builder"
echo "============================================="
echo ""

# -------------------------------------------
# Step 1: Check build dependencies
# -------------------------------------------
echo "[1/5] Checking build dependencies..."

REQUIRED_PACKAGES=(
    "build-essential"
    "cmake"
    "pkg-config"
    "libgtk-3-dev"
    "libasound2-dev"
    "libcjson-dev"
    "git"
    "dpkg-deb"
    "fakeroot"
)

# Check for appindicator (try both variants)
HAS_APPINDICATOR=0
if dpkg -l libayatana-appindicator3-dev 2>/dev/null | grep -q "^ii"; then
    HAS_APPINDICATOR=1
elif dpkg -l libappindicator3-dev 2>/dev/null | grep -q "^ii"; then
    HAS_APPINDICATOR=1
else
    REQUIRED_PACKAGES+=("libayatana-appindicator3-dev")
fi

MISSING_PACKAGES=()
for pkg in "${REQUIRED_PACKAGES[@]}"; do
    if ! dpkg -l "$pkg" 2>/dev/null | grep -q "^ii"; then
        MISSING_PACKAGES+=("$pkg")
    fi
done

if [ ${#MISSING_PACKAGES[@]} -gt 0 ]; then
    echo "Missing build dependencies:"
    printf '  - %s\n' "${MISSING_PACKAGES[@]}"
    echo ""
    echo "Install with:"
    echo "  sudo apt-get install ${MISSING_PACKAGES[*]}"
    echo ""
    read -p "Install missing packages now? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        sudo apt-get update
        sudo apt-get install -y "${MISSING_PACKAGES[@]}"
    else
        echo "Cannot build without dependencies. Aborting."
        exit 1
    fi
fi
echo "  All dependencies satisfied."
echo ""

# -------------------------------------------
# Step 2: Download Whisper model
# -------------------------------------------
echo "[2/5] Preparing Whisper model..."

if [ "$DOWNLOAD_MODEL" -eq 1 ]; then
    rm -f "$MODEL_FILE"
fi

if [ ! -f "$MODEL_FILE" ]; then
    echo "  Model not found. Downloading..."
    bash "${SCRIPT_DIR}/download-model.sh" "$MODEL_DIR"
else
    SIZE=$(du -h "$MODEL_FILE" | cut -f1)
    echo "  Model already present: ${MODEL_FILE} (${SIZE})"
fi
echo ""

# -------------------------------------------
# Step 3: Build the application
# -------------------------------------------
echo "[3/5] Building application..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CMAKE_FLAGS="-DDOWNLOAD_DEFAULT_MODEL=OFF"
if [ "$ENABLE_CUDA" -eq 1 ]; then
    CMAKE_FLAGS="$CMAKE_FLAGS -DGGML_CUDA=ON"
    echo "  CUDA acceleration enabled"
fi

cmake $CMAKE_FLAGS ..
make -j"$(nproc)"

if [ ! -f transcriber ]; then
    echo "Build failed: transcriber binary not found."
    exit 1
fi
echo "  Build successful."
echo ""

# -------------------------------------------
# Step 4: Prepare package directory structure
# -------------------------------------------
echo "[4/5] Creating package structure..."

PACKAGE_DIR="${BUILD_DIR}/transcriber-pkg"
STAGING="${PACKAGE_DIR}/usr"

mkdir -p "${STAGING}/bin"
mkdir -p "${STAGING}/share/applications"
mkdir -p "${STAGING}/share/dbus-1/services"
mkdir -p "${STAGING}/share/pixmaps"
mkdir -p "${STAGING}/share/icons/hicolor/apps"
mkdir -p "${STAGING}/share/transcriber/models"
mkdir -p "${PACKAGE_DIR}/DEBIAN"

# Install binary
cp transcriber "${STAGING}/bin/"
chmod 755 "${STAGING}/bin/transcriber"

# Install desktop file
cp "${PROJECT_DIR}/transcriber.desktop" "${STAGING}/share/applications/"

# Install D-Bus service file
cp "${PROJECT_DIR}/org.xvoice.Controller.service" "${STAGING}/share/dbus-1/services/"

# Install icons
cp "${PROJECT_DIR}/assets/redmic.xpm" "${STAGING}/share/pixmaps/"
cp "${PROJECT_DIR}/assets/greenmic.xpm" "${STAGING}/share/pixmaps/"
cp "${PROJECT_DIR}/assets/gear.xpm" "${STAGING}/share/pixmaps/"
cp "${PROJECT_DIR}/assets/hicolor/redmic.png" "${STAGING}/share/icons/hicolor/apps/"
cp "${PROJECT_DIR}/assets/hicolor/greenmic.png" "${STAGING}/share/icons/hicolor/apps/"
cp "${PROJECT_DIR}/assets/hicolor/gear.png" "${STAGING}/share/icons/hicolor/apps/"

# Install bundled model
if [ -f "$MODEL_FILE" ]; then
    cp "$MODEL_FILE" "${STAGING}/share/transcriber/models/"
    SIZE=$(du -h "${STAGING}/share/transcriber/models/ggml-large-v3-turbo-q8_0.bin" | cut -f1)
    echo "  Bundled model: ${SIZE}"
else
    echo "  WARNING: Model not found. Package will be built without the model."
    echo "  Run with --download-model to include it."
fi

# -------------------------------------------
# Create DEBIAN/control
# -------------------------------------------
ARCH=$(dpkg --print-architecture)
VERSION=$(grep 'project(transcriber VERSION' "${PROJECT_DIR}/CMakeLists.txt" | sed 's/.*VERSION \([^ ]*\).*/\1/')

cat > "${PACKAGE_DIR}/DEBIAN/control" << EOF
Package: transcriber
Version: ${VERSION}
Section: sound
Priority: optional
Architecture: ${ARCH}
Depends: libgtk-3-0, libasound2, libcjson1, libayatana-appindicator3-1 | libappindicator1
Recommends: curl
Maintainer: Piyush Raizada <piyush.raizada@gmail.com>
Description: Offline voice-to-text transcription application using Whisper
 Transcriber is a lightweight, offline voice-to-text application for Linux
 desktops. It captures audio from the microphone and transcribes it into
 text locally using OpenAI's Whisper model via whisper.cpp, ensuring privacy
 and removing the need for an internet connection.
 .
 Features:
  * Voice capture via system tray or main window
  * Local offline transcription with Whisper large-v3-turbo model
  * Text management with clipboard support
  * Global hotkeys via D-Bus
  * NVIDIA GPU (CUDA) acceleration support
  * Real-time volume level indicator
EOF

# -------------------------------------------
# Create DEBIAN/postinst
# -------------------------------------------
cat > "${PACKAGE_DIR}/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e

case "$1" in
    configure)
        # Update icon cache
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache /usr/share/icons/hicolor/ >/dev/null 2>&1 || true
        fi

        # Create user model directory and symlink if system model exists
        MODEL_SRC="/usr/share/transcriber/models/ggml-large-v3-turbo-q8_0.bin"
        if [ -f "$MODEL_SRC" ]; then
            echo "transcriber: Whisper turbo model installed at $MODEL_SRC"
        fi
        ;;
esac

exit 0
EOF
chmod 755 "${PACKAGE_DIR}/DEBIAN/postinst"

# -------------------------------------------
# Create DEBIAN/postrm
# -------------------------------------------
cat > "${PACKAGE_DIR}/DEBIAN/postrm" << 'EOF'
#!/bin/sh
set -e

case "$1" in
    purge)
        rm -rf /usr/share/transcriber
        ;;
esac

exit 0
EOF
chmod 755 "${PACKAGE_DIR}/DEBIAN/postrm"

# -------------------------------------------
# Create DEBIAN/prerm
# -------------------------------------------
cat > "${PACKAGE_DIR}/DEBIAN/prerm" << 'EOF'
#!/bin/sh
set -e

case "$1" in
    remove)
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache /usr/share/icons/hicolor/ >/dev/null 2>&1 || true
        fi
        ;;
esac

exit 0
EOF
chmod 755 "${PACKAGE_DIR}/DEBIAN/prerm"

echo "  Package structure ready."
echo ""

# -------------------------------------------
# Step 5: Build the .deb package
# -------------------------------------------
echo "[5/5] Building .deb package..."

DEB_FILE="${PROJECT_DIR}/transcriber_${VERSION}_${ARCH}.deb"

# Remove old package if exists
rm -f "$DEB_FILE"

fakeroot dpkg-deb --build --root-owner-group "$PACKAGE_DIR" "$DEB_FILE"

echo ""
echo "============================================="
echo "  Build Complete!"
echo "============================================="
echo ""
echo "Package: ${DEB_FILE}"
PKG_SIZE=$(du -h "$DEB_FILE" | cut -f1)
echo "Size:    ${PKG_SIZE}"
echo ""
echo "Install with:"
echo "  sudo dpkg -i ${DEB_FILE}"
echo ""
echo "Or:"
echo "  sudo apt install ./$(basename "$DEB_FILE")"
echo ""
