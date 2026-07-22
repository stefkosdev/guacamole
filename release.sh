#!/bin/bash
# Build the package and optionally deploy to a Kin development build tree.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -n "${KIN_BUILD_PATH:-}" ]; then
  "$ROOT/build-apps.sh" "$KIN_BUILD_PATH"
fi
"$ROOT/make-debian.sh"
