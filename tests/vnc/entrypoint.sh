#!/bin/bash
# Bring up a headless X display and serve it over VNC (RFB).
set -e

DISPLAY="${DISPLAY:-:0}"
GEOMETRY="${GEOMETRY:-1024x768x24}"
VNC_PORT="${VNC_PORT:-5900}"
VNC_PASSWORD="${VNC_PASSWORD:-guacvnc}"

echo "[vnc-target] starting Xvfb on ${DISPLAY} (${GEOMETRY})"
Xvfb "${DISPLAY}" -screen 0 "${GEOMETRY}" -nolisten tcp &
XVFB_PID=$!

# Wait for the X socket to appear.
for _ in $(seq 1 50); do
    [ -e "/tmp/.X11-unix/X${DISPLAY#:}" ] && break
    sleep 0.1
done

echo "[vnc-target] starting fluxbox"
fluxbox >/dev/null 2>&1 &

# Something visible so a captured frame is not blank.
xterm -geometry 100x30+10+10 -e "echo 'Kin Guacamole VNC test target'; exec bash" >/dev/null 2>&1 &

echo "[vnc-target] storing password and starting x11vnc on :${VNC_PORT}"
mkdir -p /root/.vnc
x11vnc -storepasswd "${VNC_PASSWORD}" /root/.vnc/passwd >/dev/null 2>&1

exec x11vnc \
    -display "${DISPLAY}" \
    -rfbport "${VNC_PORT}" \
    -rfbauth /root/.vnc/passwd \
    -forever -shared -noxdamage
