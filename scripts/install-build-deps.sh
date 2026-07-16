#!/usr/bin/env bash
#
# Install every system library needed to BUILD guacamole from source.
#
# `make` (the service build) fetches the Apache guacamole-server source and
# compiles libguac plus the protocol plugins (VNC, RDP, SSH, Telnet, Kubernetes),
# then statically links libguac into guacamole.service. That needs a toolchain,
# the autotools, and a set of -dev libraries. This script installs all of them.
#
# It does NOT install kin.library — that is produced by the Kin build and wired
# in by build-apps.sh (via the repo-root libraries/ symlinks).
#
# System packages are installed with your distro's package manager (needs sudo);
# anything already present is skipped.
#
# Usage:
#   scripts/install-build-deps.sh          # install everything
#   scripts/install-build-deps.sh --list   # just print the package list and exit
set -uo pipefail

LIST_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --list) LIST_ONLY=1 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

have() { command -v "$1" >/dev/null 2>&1; }
say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }

# --- Pick a package manager --------------------------------------------------
PM=""; PM_INSTALL=""
if   have apt-get; then PM=apt;    PM_INSTALL="sudo apt-get install -y"
elif have dnf;     then PM=dnf;    PM_INSTALL="sudo dnf install -y"
elif have pacman;  then PM=pacman; PM_INSTALL="sudo pacman -S --needed --noconfirm"
fi

# Logical build dependencies. Each maps to the package name(s) per distro.
# Grouped by purpose (comments) but installed together.
LOGICAL=(
    # toolchain + autotools + pkg-config
    toolchain git autoconf automake libtool pkgconfig
    # libguac core
    cairo jpeg png uuid webp ssl
    # protocol plugins: VNC / RDP / SSH+Telnet text / SSH / Telnet / Kubernetes / audio
    vnc rdp pango ssh2 telnet websockets pulse vorbis
)

pkg_for() {  # pkg_for <logical>  -> space-separated distro package(s)
    case "$PM:$1" in
        apt:toolchain)     echo build-essential ;;
        apt:git)           echo git ;;
        apt:autoconf)      echo autoconf ;;
        apt:automake)      echo automake ;;
        apt:libtool)       echo libtool libtool-bin ;;
        apt:pkgconfig)     echo pkg-config ;;
        apt:cairo)         echo libcairo2-dev ;;
        apt:jpeg)          echo libjpeg62-turbo-dev ;;
        apt:png)           echo libpng-dev ;;
        apt:uuid)          echo libossp-uuid-dev uuid-dev ;;
        apt:webp)          echo libwebp-dev ;;
        apt:ssl)           echo libssl-dev ;;
        apt:vnc)           echo libvncserver-dev ;;
        apt:rdp)           echo freerdp2-dev ;;
        apt:pango)         echo libpango1.0-dev ;;
        apt:ssh2)          echo libssh2-1-dev ;;
        apt:telnet)        echo libtelnet-dev ;;
        apt:websockets)    echo libwebsockets-dev ;;
        apt:pulse)         echo libpulse-dev ;;
        apt:vorbis)        echo libvorbis-dev ;;

        dnf:toolchain)     echo gcc gcc-c++ make ;;
        dnf:git)           echo git ;;
        dnf:autoconf)      echo autoconf ;;
        dnf:automake)      echo automake ;;
        dnf:libtool)       echo libtool ;;
        dnf:pkgconfig)     echo pkgconf-pkg-config ;;
        dnf:cairo)         echo cairo-devel ;;
        dnf:jpeg)          echo libjpeg-turbo-devel ;;
        dnf:png)           echo libpng-devel ;;
        dnf:uuid)          echo libuuid-devel ;;
        dnf:webp)          echo libwebp-devel ;;
        dnf:ssl)           echo openssl-devel ;;
        dnf:vnc)           echo libvncserver-devel ;;
        dnf:rdp)           echo freerdp-devel ;;
        dnf:pango)         echo pango-devel ;;
        dnf:ssh2)          echo libssh2-devel ;;
        dnf:telnet)        echo libtelnet-devel ;;
        dnf:websockets)    echo libwebsockets-devel ;;
        dnf:pulse)         echo pulseaudio-libs-devel ;;
        dnf:vorbis)        echo libvorbis-devel ;;

        pacman:toolchain)  echo base-devel ;;
        pacman:git)        echo git ;;
        pacman:autoconf)   echo autoconf ;;
        pacman:automake)   echo automake ;;
        pacman:libtool)    echo libtool ;;
        pacman:pkgconfig)  echo pkgconf ;;
        pacman:cairo)      echo cairo ;;
        pacman:jpeg)       echo libjpeg-turbo ;;
        pacman:png)        echo libpng ;;
        pacman:uuid)       echo util-linux-libs ;;
        pacman:webp)       echo libwebp ;;
        pacman:ssl)        echo openssl ;;
        pacman:vnc)        echo libvncserver ;;
        pacman:rdp)        echo freerdp ;;
        pacman:pango)      echo pango ;;
        pacman:ssh2)       echo libssh2 ;;
        pacman:telnet)     echo libtelnet ;;
        pacman:websockets) echo libwebsockets ;;
        pacman:pulse)      echo libpulse ;;
        pacman:vorbis)     echo libvorbis ;;
        *) echo "" ;;
    esac
}

# Collect the full package list for the detected PM.
PKGS=()
for l in "${LOGICAL[@]}"; do
    for p in $(pkg_for "$l"); do PKGS+=("$p"); done
done

if [ "$LIST_ONLY" -eq 1 ]; then
    printf '%s\n' "package manager: ${PM:-unknown}"
    printf '%s\n' "${PKGS[@]}"
    exit 0
fi

if [ -z "$PM" ]; then
    echo "No supported package manager (apt/dnf/pacman) found." >&2
    echo "Install these build dependencies manually, then run 'make':" >&2
    echo "  gcc/g++, make, git, autoconf, automake, libtool, pkg-config," >&2
    echo "  and -dev packages for: cairo, jpeg, png, uuid, webp, ssl, pango," >&2
    echo "  libvncserver, freerdp2, libssh2, libtelnet, libwebsockets, pulse, vorbis" >&2
    exit 1
fi

say "Installing guacamole build dependencies via $PM (${#PKGS[@]} packages)"
echo "  ${PKGS[*]}"
# shellcheck disable=SC2086
$PM_INSTALL "${PKGS[@]}" || { echo "Package install failed — see errors above." >&2; exit 1; }

# --- Summary: verify the key -dev libraries are visible to pkg-config ---------
say "Verifying key libraries"
ok=1
for probe in cairo libpng libjpeg zlib; do
    if pkg-config --exists "$probe" 2>/dev/null; then
        printf "  %-10s ok\n" "$probe"
    else
        printf "  %-10s (pkg-config can't see it — may still be fine)\n" "$probe"
    fi
done
have gcc && have make && have git && have autoreconf || { echo "  toolchain incomplete"; ok=0; }

say "Done"
echo "Build guacamole with:"
echo "  make            # fetches guacamole-server, builds libguac + the service"
echo "  ./build-apps.sh # deploy the app/service/command into a Kin build"
[ "$ok" -eq 1 ] || { echo "(some checks were inconclusive — try 'make' and see if it builds)"; }
