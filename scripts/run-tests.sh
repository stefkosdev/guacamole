#!/usr/bin/env bash
#
# Run the Guacamole test suite.
#
# Layer 1 (always): management/persistence unit+integration tests. No external
#                   dependencies — pure gcc against management.c.
# Layer 2 (opt-in): socket smoke test against a running service. Skipped when
#                   the service socket is absent.
#
# Usage: scripts/run-tests.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Layer 1: management / persistence tests ==="
make -C "$REPO_DIR/services/guacamole.service/tests" run

echo
echo "=== Layer 2: service socket smoke test (skipped if service not running) ==="
set +e
python3 "$SCRIPT_DIR/guac-smoke-test.py"
rc=$?
set -e
if [ "$rc" -eq 2 ]; then
    echo "(smoke test skipped — start the service under Kin to run it)"
elif [ "$rc" -ne 0 ]; then
    echo "smoke test FAILED" >&2
    exit "$rc"
fi

echo
echo "All available tests passed."
