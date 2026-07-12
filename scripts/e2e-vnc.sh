#!/usr/bin/env bash
#
# Full end-to-end test: a stored connection drives a REAL VNC session.
#
# Brings up a VNC target (Docker), starts guacamole.service under a minimal Kin
# manager stub (no full Kin, no nginx, no DB), seeds a stored VNC connection in
# the .info store, and:
#   1. socket smoke test        — select a bogus protocol → expect `error`
#   2. stored-connection probe  — select $<id> with EMPTY client argv → expect
#                                 `ready` (host/port/password come from the store)
#   3. backend assertion        — the VNC target logs an authenticated client,
#                                 proving the stored credentials reached the plugin
#
# Everything is torn down on exit. Requires: docker, a built guacamole.service,
# and kin.library (from ./libraries or ../kin/build/libraries).
#
# Usage: scripts/e2e-vnc.sh [--port 5905] [--keep]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
PORT=5905
KEEP=0
CONN_ID="e2e00000000000000000000000000001"
CONTAINER="kin-guac-vnc-test"

while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

SVC="$REPO/services/guacamole.service/guacamole.service"
GUAC_LIB_DIR="$REPO/dependencies/guacamole-server-install/lib"
WORK="$(mktemp -d)"
STORE="$WORK/Guacamole.info"
STUB="$WORK/fake_manager"
STUB_PID=""
SVC_PID=""

# Resolve kin.library (symlinked into ./libraries by e2e setup, or the Kin build).
KIN_LIB=""
for c in "$REPO/libraries/kin.library" "$REPO/../kin/build/libraries/kin.library"; do
    [ -e "$c" ] && { KIN_LIB="$c"; break; }
done

if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    SOCK="$XDG_RUNTIME_DIR/kin/guacamole.sock"
else
    SOCK="/tmp/kin-guacamole-$(id -u).sock"
fi

cleanup() {
    [ -n "$SVC_PID" ] && kill "$SVC_PID" 2>/dev/null
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    sleep 0.3
    rm -f /dev/shm/kin_shm_"$STUB_PID" 2>/dev/null || true
    rm -f "$SOCK" 2>/dev/null || true
    if [ "$KEEP" -eq 0 ]; then
        "$SCRIPT_DIR/teardown-vnc-test-env.sh" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "E2E FAIL: $*" >&2; exit 1; }

[ -x "$SVC" ] || fail "guacamole.service not built — run 'make' first ($SVC)"
[ -n "$KIN_LIB" ] || fail "kin.library not found (looked in ./libraries and ../kin/build/libraries)"
command -v docker >/dev/null 2>&1 || fail "docker not available"

echo "=== [1/6] Build the Kin manager stub ==="
gcc -O0 -g "$REPO/tests/e2e/fake_manager.c" -o "$STUB" \
    -L"$(dirname "$KIN_LIB")" -l:kin.library \
    -Wl,-rpath,"$(dirname "$KIN_LIB")" -lpthread -lrt \
    || fail "could not build fake_manager stub"

echo "=== [2/6] Bring up the VNC target on 127.0.0.1:$PORT ==="
"$SCRIPT_DIR/setup-vnc-test-env.sh" --port "$PORT" >/dev/null || fail "VNC target did not come up"
python3 "$SCRIPT_DIR/vnc-rfb-probe.py" 127.0.0.1 "$PORT" || fail "VNC target not serving RFB"

echo "=== [3/6] Seed a stored VNC connection in the .info store ==="
cat > "$STORE" <<EOF
{
  "version": 1,
  "connections": [
    {"id":"$CONN_ID","name":"E2E VNC","protocol":"vnc","hostname":"127.0.0.1","port":$PORT,"username":"","password":"guacvnc","private_key":"","domain":"","security":"","color_depth":"24","width":1024,"height":768,"dpi":96}
  ]
}
EOF

echo "=== [4/6] Start guacamole.service under the stub ==="
export LD_LIBRARY_PATH="$(dirname "$KIN_LIB"):$GUAC_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export KIN_GUACAMOLE_STATE="$STORE"
"$STUB" > "$WORK/stub.out" 2>&1 &
STUB_PID=$!
sleep 0.5
kill -0 "$STUB_PID" 2>/dev/null || { cat "$WORK/stub.out"; fail "stub died"; }
MGR="$(head -1 "$WORK/stub.out")"
[ -n "$MGR" ] || fail "stub did not announce a manager pid"

"$SVC" "$MGR" > "$WORK/svc.out" 2>&1 &
SVC_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { cat "$WORK/svc.out"; fail "service socket never appeared at $SOCK"; }
echo "service up (manager key $MGR), socket at $SOCK"

echo "=== [5/6] Socket smoke test + stored-connection probe ==="
python3 "$SCRIPT_DIR/guac-smoke-test.py" "$SOCK" || fail "socket smoke test failed"
python3 "$REPO/tests/e2e/guac_client_probe.py" "$SOCK" "\$$CONN_ID" empty \
    || fail "stored-connection probe did not reach 'ready'"

echo "=== [6/6] Assert the VNC backend saw an authenticated client ==="
sleep 0.5
if docker logs "$CONTAINER" 2>&1 | grep -qE "accepted_client|autorepeat"; then
    echo "VNC target logged an authenticated client — stored credentials reached the plugin."
else
    fail "VNC target shows no authenticated client (stored connection did not reach the plugin)"
fi

echo
echo "E2E PASS: a persisted connection drove a real, authenticated VNC session."
