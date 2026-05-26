#!/bin/bash
# Install Guacamole Kin app into the Kin source repository and sync to build/
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$SCRIPT_DIR/kin"
BUILD_DIR="$SCRIPT_DIR/build/repository/Applications/Administration"
CONFIG_FILE="$SCRIPT_DIR/.config.ini"

KIN_BUILD_PATH=""

load_config() {
    if [ -f "$CONFIG_FILE" ]; then
        KIN_BUILD_PATH=$(grep "^KIN_BUILD_PATH=" "$CONFIG_FILE" 2>/dev/null | head -1 | cut -d'=' -f2-)
    fi
}

save_config() {
    if [ -n "$KIN_BUILD_PATH" ]; then
        echo "KIN_BUILD_PATH=$KIN_BUILD_PATH" > "$CONFIG_FILE"
        echo "Saved Kin build path to config: $KIN_BUILD_PATH"
    fi
}

prompt_kin_path() {
    echo ""
    echo "Enter the path to your Kin build directory (e.g. /home/user/Projects/kin/build):"
    echo -n "> "
    read -r KIN_BUILD_PATH
    KIN_BUILD_PATH=$(echo "$KIN_BUILD_PATH" | sed 's:/*$::')
    if [ -z "$KIN_BUILD_PATH" ]; then
        echo "No path provided. Skipping install."
        return 1
    fi
    if [ ! -d "$KIN_BUILD_PATH" ]; then
        echo "Error: Directory does not exist: $KIN_BUILD_PATH"
        return 1
    fi
    return 0
}

install_to_kin() {
    if [ -z "$KIN_BUILD_PATH" ]; then return; fi

    # Kin source tree: sibling of build/ (e.g. .../kin/repository)
    KIN_SOURCE_PATH="${KIN_BUILD_PATH%/build}"
    if [ ! -d "$KIN_SOURCE_PATH/repository" ]; then
        echo "Error: Kin source repository not found at $KIN_SOURCE_PATH/repository"
        echo "       Expected layout: <kin>/repository and <kin>/build"
        return 1
    fi

    KIN_APP_SRC="$KIN_SOURCE_PATH/repository/Applications/Administration/kin_guacamole_admin"
    echo "Installing Guacamole admin app to Kin source: $KIN_APP_SRC"
    mkdir -p "$(dirname "$KIN_APP_SRC")"
    rsync -av --delete "$SOURCE_DIR/" "$KIN_APP_SRC/"

    # Copy service files
    KIN_SERVICE_SRC="$KIN_SOURCE_PATH/services/guacamole"
    echo "Installing Guacamole service to Kin source: $KIN_SERVICE_SRC"
    mkdir -p "$(dirname "$KIN_SERVICE_SRC")"
    rsync -av --delete "$SCRIPT_DIR/services/guacamole.service/" "$KIN_SERVICE_SRC/"

    echo "Syncing repository/ to build/repository/..."
    rsync -av --delete "$KIN_SOURCE_PATH/repository/" "$KIN_BUILD_PATH/repository/"

    echo "Guacamole installed. Restart Kin to pick up changes."
}

load_config

echo ""
echo "=== Guacamole Build Script ==="
echo ""

if [ -z "$KIN_BUILD_PATH" ]; then
    if prompt_kin_path; then
        save_config
    fi
else
    echo "Using Kin build path from config: $KIN_BUILD_PATH"
fi

echo ""
echo "=== Building local copy ==="
mkdir -p "$BUILD_DIR"
if [ -d "$SOURCE_DIR" ]; then
    rsync -av --delete "$SOURCE_DIR/" "$BUILD_DIR/kin_guacamole_admin/"
    echo "Done. Apps built to $BUILD_DIR"
else
    echo "Error: No kin/ directory found at $SOURCE_DIR"
    exit 1
fi

echo ""
echo "=== Installing to Kin ==="
if [ -n "$KIN_BUILD_PATH" ]; then
    install_to_kin
fi

echo ""
echo "=== Build complete ==="
