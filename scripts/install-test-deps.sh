#!/usr/bin/env bash
#
# Install everything the test layers need, then the browser-test npm deps.
#
# Layers and what they require:
#   test-unit / test  -> gcc, make
#   e2e / tunnel-test -> + docker, python3
#   browser-test      -> + chromium, node/npm, puppeteer-core (installed locally
#                        into scripts/browser/node_modules — no browser download)
#
# System packages are installed with your distro's package manager (needs sudo);
# already-present tools are skipped. The npm step is local and needs no sudo.
#
# Usage:
#   scripts/install-test-deps.sh              # everything
#   scripts/install-test-deps.sh --no-system  # skip apt/dnf/pacman, only npm deps
#   scripts/install-test-deps.sh --browser    # only the browser-test stack
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DO_SYSTEM=1
SCOPE="all"   # all | browser
for arg in "$@"; do
    case "$arg" in
        --no-system) DO_SYSTEM=0 ;;
        --browser)   SCOPE="browser" ;;
        -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

have() { command -v "$1" >/dev/null 2>&1; }
say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }

# --- Pick a package manager ---------------------------------------------------
PM=""; PM_INSTALL=""
if   have apt-get; then PM=apt;    PM_INSTALL="sudo apt-get install -y"
elif have dnf;     then PM=dnf;    PM_INSTALL="sudo dnf install -y"
elif have pacman;  then PM=pacman; PM_INSTALL="sudo pacman -S --noconfirm"
fi

# Map a logical tool -> the package name on each distro. Returns empty to skip.
pkg_for() {
    local tool="$1"
    case "$PM:$tool" in
        apt:chromium)    echo chromium ;;
        dnf:chromium)    echo chromium ;;
        pacman:chromium) echo chromium ;;
        apt:node)        echo nodejs npm ;;
        dnf:node)        echo nodejs npm ;;
        pacman:node)     echo nodejs npm ;;
        *:gcc)           echo gcc ;;
        *:make)          echo make ;;
        *:python3)       echo python3 ;;
        apt:docker)      echo docker.io ;;
        dnf:docker)      echo docker ;;
        pacman:docker)   echo docker ;;
        *) echo "" ;;
    esac
}

install_tool() {   # install_tool <cmd-to-check> <logical-name>
    local check="$1" tool="$2"
    if have "$check"; then echo "  ok: $check already present"; return 0; fi
    if [ "$DO_SYSTEM" -eq 0 ]; then echo "  skip (--no-system): $check missing"; return 0; fi
    if [ -z "$PM" ]; then echo "  WARN: no known package manager; install '$check' manually"; return 0; fi
    local pkgs; pkgs="$(pkg_for "$tool")"
    [ -n "$pkgs" ] || { echo "  WARN: don't know the $PM package for '$tool'; install manually"; return 0; }
    echo "  installing: $pkgs"
    # shellcheck disable=SC2086
    $PM_INSTALL $pkgs || echo "  WARN: '$PM_INSTALL $pkgs' failed — install '$check' manually"
}

if [ "$SCOPE" = "all" ]; then
    say "Core build + e2e tools"
    install_tool gcc gcc
    install_tool make make
    install_tool python3 python3
    install_tool docker docker
fi

say "Browser-test tools (chromium + node)"
install_tool chromium chromium
if ! have chromium && have chromium-browser; then echo "  note: found chromium-browser"; fi
install_tool node node

# --- Local npm deps (puppeteer-core; no bundled browser download) -------------
say "Browser-test npm deps (scripts/browser)"
if have npm; then
    ( cd "$SCRIPT_DIR/browser" && PUPPETEER_SKIP_DOWNLOAD=1 npm install --no-audit --no-fund ) \
        && echo "  ok: puppeteer-core installed" \
        || echo "  WARN: npm install failed in scripts/browser"
else
    echo "  WARN: npm not found; cannot install puppeteer-core"
fi

# --- Summary ------------------------------------------------------------------
say "Summary"
CHROMIUM_BIN=""
for c in chromium chromium-browser google-chrome google-chrome-stable chrome; do
    if have "$c"; then CHROMIUM_BIN="$(command -v "$c")"; break; fi
done
printf '  gcc:        %s\n' "$(command -v gcc || echo MISSING)"
printf '  make:       %s\n' "$(command -v make || echo MISSING)"
printf '  docker:     %s\n' "$(command -v docker || echo MISSING)"
printf '  python3:    %s\n' "$(command -v python3 || echo MISSING)"
printf '  node:       %s\n' "$(command -v node || echo MISSING)"
printf '  chromium:   %s\n' "${CHROMIUM_BIN:-MISSING}"
printf '  puppeteer:  %s\n' "$([ -d "$SCRIPT_DIR/browser/node_modules/puppeteer-core" ] && echo installed || echo MISSING)"

echo
echo "Done. Next:"
echo "  make test-unit                                   # no deps"
echo "  KIN_SESSION=... make tunnel-test                 # server-side video path"
echo "  KIN_SESSION=... make browser-test                # real in-browser render"
