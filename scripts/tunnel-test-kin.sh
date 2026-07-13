#!/usr/bin/env bash
#
# Live viewer tunnel test — the full video path:
#   browser -> http.service WS tunnel -> guacamole.service -> VNC backend
#
# Brings up a Docker VNC target, adds a connection to it via the API, opens the
# WebSocket tunnel exactly like the browser (no client handshake — the tunnel
# does the guacd handshake server-side), and asserts that real display data
# streams back. Cleans up the connection and the container afterward.
#
# Requires: a running Kin, docker, and a session:
#   KIN_SESSION=<kin_session cookie> scripts/tunnel-test-kin.sh [base_url]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
BASE="${1:-http://localhost:9119}"
HOSTPORT="${BASE#http://}"
KIN_SESSION="${KIN_SESSION:-}"
PORT=5900
CID=""

cleanup() {
    if [ -n "$CID" ]; then
        curl -s -m 10 --cookie "kin_session=$KIN_SESSION" -H "content-type: application/json" \
            -X POST --data "{\"action\":\"delete\",\"id\":\"$CID\"}" \
            "$BASE/api/guacamole/connections" >/dev/null 2>&1 || true
    fi
    "$SCRIPT_DIR/teardown-vnc-test-env.sh" >/dev/null 2>&1 || true
}
trap cleanup EXIT

fail() { echo "TUNNEL TEST FAIL: $*" >&2; exit 1; }

[ -n "$KIN_SESSION" ] || { echo "SKIP: set KIN_SESSION=<kin_session cookie> to run." >&2; exit 2; }
command -v docker >/dev/null 2>&1 || fail "docker not available"
command -v python3 >/dev/null 2>&1 || fail "python3 not available"

echo "=== [1/4] Bring up the VNC target ==="
"$SCRIPT_DIR/setup-vnc-test-env.sh" --port "$PORT" >/dev/null || fail "VNC target did not come up"

echo "=== [2/4] Add a connection to it via the API ==="
add_resp="$(curl -s -m 10 --cookie "kin_session=$KIN_SESSION" -H "content-type: application/json" \
    -X POST --data "{\"action\":\"add\",\"name\":\"tunnel-test-$$\",\"protocol\":\"vnc\",\"hostname\":\"127.0.0.1\",\"port\":$PORT,\"password\":\"guacvnc\"}" \
    "$BASE/api/guacamole/connections")"
CID="$(printf '%s' "$add_resp" | sed -n 's/.*"connection_id":"\([^"]*\)".*/\1/p')"
[ -n "$CID" ] || fail "could not add connection: $add_resp"
echo "connection id: $CID"

echo "=== [3/4] Open the WS tunnel and check for display data ==="
python3 "$REPO/tests/e2e/tunnel_probe.py" "$HOSTPORT" "$CID" "$KIN_SESSION" || fail "no display data over the tunnel"

echo "=== [4/4] Cleanup ==="
echo "TUNNEL TEST PASS: the live viewer path streams a remote desktop."
