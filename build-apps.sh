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

# Forget a remembered path so the NEXT run asks again.
forget_config() {
    rm -f "$CONFIG_FILE"
}

# A valid Kin build dir exists AND its source tree sits beside it: the parent
# (strip a trailing /build) must contain repository/. This is exactly what
# install_to_kin needs, so we reject a bad path up front instead of half-installing.
validate_kin_path() {
    local p="$1"
    [ -n "$p" ] || return 1
    if [ ! -d "$p" ]; then
        echo "  Not a directory: $p" >&2
        return 1
    fi
    local src="${p%/build}"
    if [ ! -d "$src/repository" ]; then
        echo "  '$p' doesn't look like a Kin build directory." >&2
        echo "  Expected its source tree at '$src/repository' (layout: <kin>/build and <kin>/repository)." >&2
        return 1
    fi
    return 0
}

# Ask until a valid path is entered, or the user presses Enter to skip.
prompt_kin_path() {
    while true; do
        echo ""
        echo "Enter the path to your Kin build directory (e.g. /home/user/Projects/kin/build):"
        echo -n "> "
        read -r KIN_BUILD_PATH
        KIN_BUILD_PATH=$(echo "$KIN_BUILD_PATH" | sed 's:/*$::')
        if [ -z "$KIN_BUILD_PATH" ]; then
            echo "No path provided. Skipping install to Kin."
            return 1
        fi
        if validate_kin_path "$KIN_BUILD_PATH"; then
            return 0
        fi
        echo "Please try again (or press Enter to skip)."
    done
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

    # Build the service binary and deploy it where the Kin manager launches
    # workers (build/services/guacamole.service). A source-only copy is not
    # enough: the manager runs the compiled binary, so it must be built (against
    # the Kin library) and installed here, otherwise a clean Kin build has no
    # guacamole.service to launch.
    echo "Building guacamole.service binary..."
    mkdir -p "$SCRIPT_DIR/libraries"
    ln -sfn "$KIN_BUILD_PATH/libraries/kin.library" "$SCRIPT_DIR/libraries/kin.library"
    ln -sfn "$KIN_SOURCE_PATH/libraries/kin" "$SCRIPT_DIR/libraries/kin"
    if make -C "$SCRIPT_DIR/services/guacamole.service"; then
        mkdir -p "$KIN_BUILD_PATH/services"
        # rm first: the running worker may hold the file open (Text file busy).
        rm -f "$KIN_BUILD_PATH/services/guacamole.service"
        cp "$SCRIPT_DIR/services/guacamole.service/guacamole.service" \
           "$KIN_BUILD_PATH/services/guacamole.service"
        echo "Deployed guacamole.service binary to $KIN_BUILD_PATH/services/"
    else
        echo "WARNING: guacamole.service build failed; binary not deployed." >&2
    fi

    # Install and build the KinDOS `guacamole` command. Like the service, a
    # source-only copy is not enough — the shell runs the compiled binary from
    # build/commands/, so it must be built (against the Kin library) and deployed
    # there. We build the local copy (its Makefile uses the repo-root libraries/
    # symlinks set up above) and install the binary into both the Kin source
    # tree (commands/) and the runtime location (build/commands/).
    KIN_CMD_SRC="$KIN_SOURCE_PATH/commands/guacamole.cmd"
    echo "Installing guacamole command to Kin source: $KIN_CMD_SRC"
    mkdir -p "$KIN_CMD_SRC"
    rsync -av --delete --exclude '*.o' --exclude 'guacamole' \
        "$SCRIPT_DIR/command/guacamole.cmd/" "$KIN_CMD_SRC/"
    echo "Building guacamole command binary..."
    if make -C "$SCRIPT_DIR/command/guacamole.cmd"; then
        for dest in "$KIN_SOURCE_PATH/commands/guacamole" \
                    "$KIN_CMD_SRC/guacamole" \
                    "$KIN_BUILD_PATH/commands/guacamole"; do
            mkdir -p "$(dirname "$dest")"
            rm -f "$dest"   # a running copy may hold the file open (Text file busy)
            cp "$SCRIPT_DIR/command/guacamole.cmd/guacamole" "$dest"
        done
        echo "Deployed guacamole command to $KIN_BUILD_PATH/commands/"
    else
        echo "WARNING: guacamole command build failed; binary not deployed." >&2
    fi

    echo "Syncing repository/ to build/repository/..."
    rsync -av --delete "$KIN_SOURCE_PATH/repository/" "$KIN_BUILD_PATH/repository/"

    echo "Guacamole installed. Restart Kin to pick up changes."
}

load_config

echo ""
echo "=== Guacamole Build Script ==="
echo ""

# Only trust a remembered path if it is still valid; otherwise forget it (so this
# run AND the next one ask again) rather than silently reusing a bad directory.
if [ -n "$KIN_BUILD_PATH" ]; then
    if validate_kin_path "$KIN_BUILD_PATH"; then
        echo "Using Kin build path from config: $KIN_BUILD_PATH"
    else
        echo "Saved Kin build path is no longer valid: $KIN_BUILD_PATH"
        echo "Forgetting it and asking again."
        forget_config
        KIN_BUILD_PATH=""
    fi
fi

if [ -z "$KIN_BUILD_PATH" ]; then
    if prompt_kin_path; then
        save_config
    fi
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
