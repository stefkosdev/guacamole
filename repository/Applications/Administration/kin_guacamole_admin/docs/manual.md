# Guacamole Remote Desktop

Guacamole adds browser-based remote desktop connections to Kin. Open **Guacamole** from Administration to create and manage VNC, RDP, SSH, Telnet, or Kubernetes connections.

## Connections

Use the Connections tab to add a destination and its protocol-specific settings. Credentials are stored by `guacamole.service` in its Kin data directory. Select **Open** to launch the viewer in a separate Kin window.

## Sessions

The Sessions tab lists currently active remote desktops. Closing a viewer disconnects its session. Tunnel access uses a short-lived, single-use ticket tied to your Kin user and the selected stored connection.

## Availability

The integration is loaded when Kin starts. After installing, upgrading, or removing `kin-guacamole`, restart Kin before using the module.
