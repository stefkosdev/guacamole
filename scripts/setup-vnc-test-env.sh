#!/usr/bin/env bash
#
# Bring up a real VNC server as a test target for the Guacamole gateway.
#
# Preferred path: Docker (no root needed beyond docker access, nothing is
# installed on the host). Falls back to printing apt instructions when Docker
# is unavailable.
#
# Usage:
#   scripts/setup-vnc-test-env.sh [--port 5900] [--password guacvnc]
#
# After it runs, connect the gateway to  host=127.0.0.1 port=<PORT>  protocol=vnc
# with the given password. Tear down with scripts/teardown-vnc-test-env.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="kin-guac-vnc-test"
CONTAINER="kin-guac-vnc-test"
PORT=5900
PASSWORD="guacvnc"

while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --password) PASSWORD="$2"; shift 2 ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

print_apt_fallback() {
    cat <<EOF

Docker is not available. To run a VNC target directly on the host instead:

  sudo apt-get update
  sudo apt-get install -y tigervnc-standalone-server tigervnc-common
  # start a headless display :1 on port 5901
  tigervncserver :1 -geometry 1024x768 -localhost no -SecurityTypes VncAuth

Or with x11vnc + Xvfb:

  sudo apt-get install -y x11vnc xvfb fluxbox
  Xvfb :1 -screen 0 1024x768x24 &
  DISPLAY=:1 fluxbox &
  x11vnc -display :1 -rfbport 5901 -passwd "$PASSWORD" -forever -shared

Then point the gateway at host=127.0.0.1 port=5901 protocol=vnc.
EOF
}

if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "[setup] Docker not usable." >&2
    print_apt_fallback
    exit 1
fi

echo "[setup] Building image '$IMAGE' (first run only)..."
docker build -t "$IMAGE" "$REPO_DIR/tests/vnc"

# Replace any previous container.
if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "[setup] Removing existing container '$CONTAINER'..."
    docker rm -f "$CONTAINER" >/dev/null
fi

echo "[setup] Starting VNC target on 127.0.0.1:$PORT ..."
docker run -d --name "$CONTAINER" \
    -p "127.0.0.1:$PORT:5900" \
    -e "VNC_PASSWORD=$PASSWORD" \
    "$IMAGE" >/dev/null

# Wait until the RFB banner is served.
echo "[setup] Waiting for the VNC server to accept connections..."
if command -v python3 >/dev/null 2>&1; then
    for _ in $(seq 1 50); do
        if python3 "$SCRIPT_DIR/vnc-rfb-probe.py" 127.0.0.1 "$PORT" >/dev/null 2>&1; then
            break
        fi
        sleep 0.2
    done
    python3 "$SCRIPT_DIR/vnc-rfb-probe.py" 127.0.0.1 "$PORT" || {
        echo "[setup] VNC server did not come up; container logs:" >&2
        docker logs "$CONTAINER" >&2 || true
        exit 1
    }
else
    sleep 3
fi

cat <<EOF

[setup] VNC test target is ready.
  host:     127.0.0.1
  port:     $PORT
  protocol: vnc
  password: $PASSWORD

Add a connection with those values in the Guacamole admin app (or via the API),
then Connect. Tear down with:  scripts/teardown-vnc-test-env.sh
EOF
