#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION="$(sed -n '1s/.*(\([^)]*\)).*/\1/p' "$ROOT/debian/changelog")"
ARCH="$(dpkg-architecture -qDEB_HOST_ARCH 2>/dev/null || echo amd64)"
KIN_INCLUDE="${KIN_INCLUDE:-/usr/include}"

command -v fakeroot >/dev/null || { echo "install fakeroot" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "install dpkg-deb" >&2; exit 1; }

make -C "$ROOT/polykernel/modules" KIN_INCLUDE="$KIN_INCLUDE"
make -C "$ROOT/services/guacamole.service"
make -C "$ROOT/commands/guacamole.cmd"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/kin-guacamole-deb.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

install -Dm755 "$ROOT/polykernel/modules/guacamole.module" \
  "$STAGE/usr/lib/kin/polykernel/modules/guacamole.module"
install -Dm755 "$ROOT/services/guacamole.service/guacamole.service" \
  "$STAGE/usr/lib/kin/services/modules/guacamole.service"
install -Dm755 "$ROOT/commands/guacamole.cmd/guacamole" \
  "$STAGE/usr/lib/kin/commands/guacamole"
install -d "$STAGE/usr/lib/kin/repository/Applications/Administration/kin_guacamole_admin"
cp -a "$ROOT/repository/Applications/Administration/kin_guacamole_admin/." \
  "$STAGE/usr/lib/kin/repository/Applications/Administration/kin_guacamole_admin/"
install -Dm644 "$ROOT/config/30-guacamole.conf" \
  "$STAGE/etc/nginx/kin-modules/30-guacamole.conf"

if [ -d "$ROOT/dependencies/guacamole-server-install/lib" ]; then
  install -d "$STAGE/usr/lib/kin/guacamole/lib"
  cp -a "$ROOT/dependencies/guacamole-server-install/lib/." "$STAGE/usr/lib/kin/guacamole/lib/"
  find "$STAGE/usr/lib/kin/guacamole/lib" -type f -name '*.a' -delete
fi

mkdir -p "$STAGE/DEBIAN"
SIZE="$(du -sk "$STAGE/usr" "$STAGE/etc" | awk '{n+=$1} END{print n+0}')"
cat >"$STAGE/DEBIAN/control" <<EOF
Package: kin-guacamole
Version: $VERSION
Section: misc
Priority: optional
Architecture: $ARCH
Maintainer: Kin <packages@os-kin.com>
Installed-Size: $SIZE
Depends: kin (>= 2.1.0-1), libcairo2, libpng16-16, libjpeg62-turbo, libuuid1, libwebp7, libssl3t64 | libssl3, nginx
Description: Guacamole remote desktop module for Kin
 Adds an externally packaged native module, supervised service, KinDOS command,
 Administration application, dynamic manual page, and browser tunnel.
EOF

cat >"$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v nginx >/dev/null 2>&1; then
  nginx -t
  systemctl reload nginx.service 2>/dev/null || true
fi
systemctl try-restart kin.service 2>/dev/null || true
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

cat >"$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
case "$1" in remove|purge|abort-install|abort-upgrade|disappear)
  if command -v nginx >/dev/null 2>&1 && nginx -t >/dev/null 2>&1; then
    systemctl reload nginx.service 2>/dev/null || true
  fi
  if systemctl is-active --quiet kin.service 2>/dev/null; then
    systemctl try-restart kin.service 2>/dev/null || true
  fi
esac
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postrm"

mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/kin-guacamole_${VERSION}_${ARCH}.deb"
fakeroot dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
echo "Built $OUT"
