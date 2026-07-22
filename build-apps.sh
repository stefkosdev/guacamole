#!/bin/bash
# Deploy only to Kin's build-time discovery paths. Kin source is never modified.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIN_BUILD_PATH="${KIN_BUILD_PATH:-${1:-}}"
if [ -z "$KIN_BUILD_PATH" ] || [ ! -d "$KIN_BUILD_PATH" ]; then
  echo "usage: KIN_BUILD_PATH=/path/to/kin/build $0" >&2
  exit 2
fi
KIN_SOURCE="$(cd "$KIN_BUILD_PATH/.." && pwd)"
TMP_INCLUDE="$(mktemp -d "${TMPDIR:-/tmp}/kin-guacamole-include.XXXXXX")"
trap 'rm -rf "$TMP_INCLUDE"' EXIT
mkdir -p "$TMP_INCLUDE/kin"
cp "$KIN_SOURCE/polykernel/module_api.h" "$TMP_INCLUDE/kin/polykernel_module.h"

mkdir -p "$ROOT/libraries"
ln -sfn "$KIN_BUILD_PATH/libraries/kin.library" "$ROOT/libraries/kin.library"
ln -sfn "$KIN_SOURCE/libraries/kin" "$ROOT/libraries/kin"
make -C "$ROOT/polykernel/modules" KIN_INCLUDE="$TMP_INCLUDE"
make -C "$ROOT/services/guacamole.service"
make -C "$ROOT/commands/guacamole.cmd"

install -Dm755 "$ROOT/polykernel/modules/guacamole.module" \
  "$KIN_BUILD_PATH/polykernel/modules/guacamole.module"
install -Dm755 "$ROOT/services/guacamole.service/guacamole.service" \
  "$KIN_BUILD_PATH/services/modules/guacamole.service"
install -Dm755 "$ROOT/commands/guacamole.cmd/guacamole" \
  "$KIN_BUILD_PATH/commands/guacamole"
mkdir -p "$KIN_BUILD_PATH/repository/Applications/Administration/kin_guacamole_admin"
rsync -a --delete "$ROOT/repository/Applications/Administration/kin_guacamole_admin/" \
  "$KIN_BUILD_PATH/repository/Applications/Administration/kin_guacamole_admin/"
if [ -d "$ROOT/dependencies/guacamole-server-install/lib" ]; then
  mkdir -p "$KIN_BUILD_PATH/guacamole/lib"
  rsync -a --delete "$ROOT/dependencies/guacamole-server-install/lib/" \
    "$KIN_BUILD_PATH/guacamole/lib/"
fi
echo "Guacamole deployed to Kin build discovery paths. Restart Kin."
