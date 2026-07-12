#!/usr/bin/env bash
#
# Integration test for the FULL live path through Kin:
#
#   browser -> http.service -> polykernel (router) -> guacamole.service -> .info
#
# It proves the pieces that unit tests cannot: the polykernel route family, the
# sessionid-strip (a read must not be mistaken for an action), the service's IPC
# message pump, and persistence — all together.
#
# The polykernel enforces auth BEFORE routing, so the HTTP assertions need a
# valid session. Provide it via KIN_SESSION (the value of the `kin_session`
# cookie from an authenticated browser: DevTools > Application > Cookies). The
# no-auth layer (the service's protocol socket) always runs.
#
# Usage:
#   KIN_SESSION=<kin_session-cookie> scripts/integration-test-kin.sh [base_url]
#   scripts/integration-test-kin.sh            # runs only the no-auth layer
#
# base_url defaults to http://localhost:9119 (Kin http.service).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE="${1:-http://localhost:9119}"
KIN_SESSION="${KIN_SESSION:-}"
fails=0

note() { printf '%s\n' "$*"; }
ok()   { printf '  PASS: %s\n' "$*"; }
bad()  { printf '  FAIL: %s\n' "$*"; fails=$((fails+1)); }

# ---- Layer A: the guacamole.service protocol socket is up (no auth) ----------
note "=== Layer A: guacamole.service socket ==="
if python3 "$SCRIPT_DIR/guac-smoke-test.py" >/dev/null 2>&1; then
    ok "service answers the Guacamole protocol handshake"
else
    rc=$?
    if [ "$rc" -eq 2 ]; then
        bad "service socket not found — is Kin (and guacamole.service) running?"
    else
        bad "service did not complete the select handshake"
    fi
fi

# ---- Layer B: full HTTP path through the polykernel (needs a session) --------
note "=== Layer B: HTTP API via the polykernel ==="
api() {
    # api <method> <path> [json-body]
    local method="$1" path="$2" body="${3:-}"
    if [ -n "$body" ]; then
        curl -s -m 15 --cookie "kin_session=$KIN_SESSION" \
            -H "content-type: application/json" -X "$method" \
            --data "$body" "$BASE$path"
    else
        curl -s -m 15 --cookie "kin_session=$KIN_SESSION" -X "$method" "$BASE$path"
    fi
}

if [ -z "$KIN_SESSION" ]; then
    note "  SKIP: set KIN_SESSION=<kin_session cookie> to run the authenticated path."
    note "        (unauthenticated calls all return \"No valid Kin session\" — the"
    note "         polykernel checks auth before routing, so they cannot assert routing.)"
else
    marker="itest-$$"
    add_body="{\"action\":\"add\",\"name\":\"$marker\",\"protocol\":\"vnc\",\"hostname\":\"127.0.0.1\",\"port\":5999}"

    add_resp="$(api POST /api/guacamole/connections "$add_body")"
    if printf '%s' "$add_resp" | grep -q '"response":"success"'; then
        ok "add returned success"
    else
        bad "add failed: $add_resp"
    fi

    # The core regression: after add, a plain GET (a read) must list the new
    # connection — not fail with "Unknown action" from a mis-forwarded body.
    list_resp="$(api GET /api/guacamole/connections)"
    if printf '%s' "$list_resp" | grep -q "\"name\":\"$marker\""; then
        ok "list shows the added connection (routing + sessionid-strip + IPC pump)"
    else
        bad "list did not contain $marker: $list_resp"
    fi

    cid="$(printf '%s' "$add_resp" | sed -n 's/.*"connection_id":"\([^"]*\)".*/\1/p')"
    if [ -n "$cid" ]; then
        del_resp="$(api POST /api/guacamole/connections "{\"action\":\"delete\",\"id\":\"$cid\"}")"
        printf '%s' "$del_resp" | grep -q '"response":"success"' \
            && ok "delete cleaned up the test connection" \
            || bad "delete failed: $del_resp"

        list2="$(api GET /api/guacamole/connections)"
        printf '%s' "$list2" | grep -q "\"name\":\"$marker\"" \
            && bad "connection still present after delete" \
            || ok "connection gone after delete"
    else
        bad "no connection_id returned from add; cannot clean up"
    fi
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "integration: all executed checks passed"
    exit 0
fi
echo "integration: $fails check(s) failed"
exit 1
