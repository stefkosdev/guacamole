#!/usr/bin/env bash
#
# Browser render test — the ONLY layer that exercises the real in-browser viewer:
#   headless Chromium -> deployed guac-viewer.js -> http.service WS tunnel
#   -> guacamole.service -> VNC backend, asserting the display actually paints.
#
# The C/Python probes prove the server streams display data, but they send a
# minimal handshake and so cannot catch browser-specific handshake bugs (cookie
# parsing across headers, subprotocol echo, permessage-deflate, Local Network
# Access). This layer drives an actual Chromium against the live path.
#
# It brings up a Docker VNC target, adds a connection via the API, discovers the
# deployed viewer URL, runs scripts/browser/drive-viewer.mjs, and asserts the
# client reaches CONNECTED with a painted (non-blank) display. Cleans up after.
#
# Requires: a running Kin, docker, chromium, node (+ `npm install` done in
# scripts/browser/ — run scripts/install-test-deps.sh once), and a session:
#   KIN_SESSION=<kin_session cookie> scripts/browser-test-kin.sh [base_url]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
BASE="${1:-http://localhost:9119}"
KIN_SESSION="${KIN_SESSION:-}"
CHROMIUM="${CHROMIUM:-}"
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

fail() { echo "BROWSER TEST FAIL: $*" >&2; exit 1; }

[ -n "$KIN_SESSION" ] || { echo "SKIP: set KIN_SESSION=<kin_session cookie> to run." >&2; exit 2; }
command -v docker >/dev/null 2>&1 || fail "docker not available"
command -v node   >/dev/null 2>&1 || fail "node not available (run scripts/install-test-deps.sh)"

# Locate a Chromium/Chrome binary if not given explicitly.
if [ -z "$CHROMIUM" ]; then
    for c in chromium chromium-browser google-chrome google-chrome-stable chrome; do
        if command -v "$c" >/dev/null 2>&1; then CHROMIUM="$(command -v "$c")"; break; fi
    done
fi
[ -n "$CHROMIUM" ] && [ -x "$CHROMIUM" ] || fail "no chromium/chrome binary (run scripts/install-test-deps.sh)"

[ -d "$SCRIPT_DIR/browser/node_modules/puppeteer-core" ] \
    || fail "puppeteer-core not installed — run scripts/install-test-deps.sh"

echo "=== [1/4] Bring up the VNC target ==="
"$SCRIPT_DIR/setup-vnc-test-env.sh" --port "$PORT" >/dev/null || fail "VNC target did not come up"

echo "=== [2/4] Add a connection to it via the API ==="
add_resp="$(curl -s -m 10 --cookie "kin_session=$KIN_SESSION" -H "content-type: application/json" \
    -X POST --data "{\"action\":\"add\",\"name\":\"browser-test-$$\",\"protocol\":\"vnc\",\"hostname\":\"127.0.0.1\",\"port\":$PORT,\"password\":\"guacvnc\"}" \
    "$BASE/api/guacamole/connections")"
CID="$(printf '%s' "$add_resp" | sed -n 's/.*"connection_id":"\([^"]*\)".*/\1/p')"
[ -n "$CID" ] || fail "could not add connection: $add_resp"
echo "connection id: $CID"

echo "=== [3/4] Discover the deployed viewer URL ==="
VIEWER_URL=""
for path in \
    "/repository/Applications/Administration/kin_guacamole_admin/guac-viewer.js" \
    "/Applications/Administration/kin_guacamole_admin/guac-viewer.js"; do
    code="$(curl -sL -o /dev/null -w '%{http_code}' --cookie "kin_session=$KIN_SESSION" "$BASE$path")"
    if [ "$code" = "200" ]; then VIEWER_URL="$BASE$path"; break; fi
done
[ -n "$VIEWER_URL" ] || fail "could not locate the deployed guac-viewer.js (is the admin app deployed?)"
echo "viewer module: $VIEWER_URL"

echo "=== [4/4] Render in headless Chromium and assert the display paints ==="
SHOT="${SCREENSHOT:-$REPO/tmp/browser-test.png}"
mkdir -p "$(dirname "$SHOT")" 2>/dev/null || true
KIN_BASE="$BASE" KIN_SESSION="$KIN_SESSION" CONN_ID="$CID" VIEWER_URL="$VIEWER_URL" \
    CHROMIUM="$CHROMIUM" SCREENSHOT="$SHOT" \
    node "$SCRIPT_DIR/browser/drive-viewer.mjs" || fail "viewer did not connect/paint (see JSON above)"

echo "BROWSER TEST PASS: the viewer connected and painted a remote desktop (screenshot: $SHOT)."
