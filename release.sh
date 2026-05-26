#!/bin/bash
# Release Guacamole for Kin
set -e

VERSION=${VERSION:-1.0.0}
KIN_PATH=${KIN_PATH:-~/Projects/Aurae/kin}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Building Guacamole for Kin..."
echo "Kin path: $KIN_PATH"

if [ ! -d "$KIN_PATH" ]; then
    echo "Error: Kin path not found at $KIN_PATH"
    exit 1
fi

# Create app directory in Kin source
APP_DIR="$KIN_PATH/repository/Applications/Administration/kin_guacamole_admin"
mkdir -p "$APP_DIR"

# Copy app files from kin/ directory
echo "Copying app files to $APP_DIR..."
cp -r "$SCRIPT_DIR/kin/"* "$APP_DIR/"

# Copy service files
SERVICE_DIR="$KIN_PATH/services/guacamole"
if [ -d "$SCRIPT_DIR/services/guacamole.service" ]; then
    mkdir -p "$SERVICE_DIR"
    cp -r "$SCRIPT_DIR/services/guacamole.service/"* "$SERVICE_DIR/"
    echo "Building guacamole.service..."
    make -C "$SERVICE_DIR" || echo "guacamole.service build skipped"
fi

# Copy guacamole dependencies if they exist
DEPS_DEST="$KIN_PATH/dependencies"
if [ -d "$SCRIPT_DIR/dependencies/guacamole-server-install" ]; then
    mkdir -p "$DEPS_DEST"
    cp -r "$SCRIPT_DIR/dependencies/guacamole-server-install" "$DEPS_DEST/"
fi

# Build Kin (syncs repository to build/)
echo "Building Kin..."
cd "$KIN_PATH"
make

echo ""
echo "Done! Guacamole installed to Kin."
echo "Run: cd $KIN_PATH/build && ./kin"
