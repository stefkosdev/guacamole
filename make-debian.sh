#!/bin/bash
# Build kin-guacamole_<version>_<arch>.deb into dist/
# Installs to /opt/kin/modules/kin-guacamole/ with Kin app + guacamole.service
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

if ! command -v fakeroot >/dev/null 2>&1; then
	echo "install fakeroot: sudo apt install fakeroot" >&2
	exit 1
fi
if ! command -v dpkg-deb >/dev/null 2>&1; then
	echo "install dpkg-deb (dpkg package)" >&2
	exit 1
fi

# Version: top entry in debian/changelog
if [[ -f "$ROOT/debian/changelog" ]]; then
    VERSION="$(head -1 "$ROOT/debian/changelog" | sed -n 's/.*(\([^)]*\)).*/\1/p')"
fi
if [[ -z "${VERSION:-}" ]]; then
    VERSION="0.0.0-1"
fi

if command -v dpkg-architecture >/dev/null 2>&1; then
	ARCH="$(dpkg-architecture -qDEB_HOST_ARCH)"
else
	ARCH="$(uname -m)"
	case "$ARCH" in
	x86_64) ARCH=amd64 ;;
	aarch64) ARCH=arm64 ;;
	esac
fi

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/kin-guacamole-deb.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

MODULE_DIR="$STAGE/opt/kin/modules/kin-guacamole"
mkdir -p "$MODULE_DIR"

# Copy Kin app (repository/Applications/Administration/kin_guacamole_admin/)
if [[ -d "$ROOT/kin" ]]; then
	mkdir -p "$MODULE_DIR/repository/Applications/Administration"
	cp -a "$ROOT/kin" "$MODULE_DIR/repository/Applications/Administration/kin_guacamole_admin"
fi

# Copy services/ (guacamole.service)
if [[ -d "$ROOT/services" ]]; then
	cp -a "$ROOT/services" "$MODULE_DIR/"
fi

# Copy dependencies/ (guacamole-server install) if present
if [[ -d "$ROOT/dependencies/guacamole-server-install" ]]; then
	mkdir -p "$MODULE_DIR/dependencies"
	cp -a "$ROOT/dependencies/guacamole-server-install" "$MODULE_DIR/dependencies/"
fi

# Build guacamole.service if Makefile exists
if [[ -f "$ROOT/services/guacamole.service/Makefile" ]]; then
	make -C "$ROOT/services/guacamole.service" 2>/dev/null || true
	if [[ -f "$ROOT/services/guacamole.service/guacamole.service" ]]; then
		cp "$ROOT/services/guacamole.service/guacamole.service" "$MODULE_DIR/services/guacamole.service/"
	fi
fi

# Control
SIZE="$(du -sk "$MODULE_DIR" 2>/dev/null | cut -f1)"
SIZE="${SIZE:-0}"

mkdir -p "$STAGE/DEBIAN"

cat >"$STAGE/DEBIAN/control" <<EOF
Package: kin-guacamole
Version: $VERSION
Section: misc
Priority: optional
Architecture: $ARCH
Maintainer: Kin <packages@os-kin.com>
Installed-Size: $SIZE
Depends: kin (>= 2.0), libcairo2, libpng16-16, libjpeg62-turbo, libuuid1, libwebp7
Recommends: guacamole
Description: Kin Remote Desktop Gateway — Guacamole for Kin OS
  Guacamole provides remote desktop access via VNC, RDP, SSH, Telnet,
  and Kubernetes through Apache Guacamole's libguac. It replaces the
  standard guacd daemon with native Kin IPC integration.
  Installs to /opt/kin/modules/kin-guacamole/.
EOF

cat >"$STAGE/DEBIAN/postinst" <<'POSTINST'
#!/bin/bash
set -e

case "$1" in
    configure) ;;
    abort-upgrade|abort-deconfigure|abort-remove) exit 0 ;;
    *) exit 0 ;;
esac

mkdir -p /opt/kin/modules
chown kin:kin /opt/kin/modules 2>/dev/null || true

# Install Kin app into runtime repository
if [ -d /opt/kin/modules/kin-guacamole/repository/Applications ]; then
    mkdir -p /usr/lib/kin/repository/Applications
    cp -a /opt/kin/modules/kin-guacamole/repository/Applications/. /usr/lib/kin/repository/Applications/
fi

# Install guacamole service
if [ -d /opt/kin/modules/kin-guacamole/services/guacamole.service ]; then
    mkdir -p /usr/lib/kin/services
    cd /opt/kin/modules/kin-guacamole/services/guacamole.service
    if [ -f Makefile ]; then
        make 2>/dev/null || echo "guacamole.service: build skipped (install build tools and rebuild)" >&2
    fi
    if [ -f guacamole.service ]; then
        cp guacamole.service /usr/lib/kin/services/guacamole.service
    fi
fi

# Install guacamole-server dependencies if present
if [ -d /opt/kin/modules/kin-guacamole/dependencies/guacamole-server-install ]; then
    cp -a /opt/kin/modules/kin-guacamole/dependencies/guacamole-server-install /usr/lib/kin/dependencies/
fi
POSTINST
chmod 755 "$STAGE/DEBIAN/postinst"

cat >"$STAGE/DEBIAN/prerm" <<'PRERM'
#!/bin/bash
set -e
# Remove guacamole service on removal
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    rm -f /usr/lib/kin/services/guacamole.service 2>/dev/null || true
fi
PRERM
chmod 755 "$STAGE/DEBIAN/prerm"

mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/kin-guacamole_${VERSION}_${ARCH}.deb"
fakeroot dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
echo "Built $OUT"
