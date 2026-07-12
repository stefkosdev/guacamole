#!/usr/bin/env bash
# Stop and remove the VNC test target started by setup-vnc-test-env.sh.
set -euo pipefail

CONTAINER="kin-guac-vnc-test"

if command -v docker >/dev/null 2>&1 && docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "[teardown] Removing container '$CONTAINER'..."
    docker rm -f "$CONTAINER" >/dev/null
    echo "[teardown] Done."
else
    echo "[teardown] No container '$CONTAINER' found (nothing to do)."
fi
