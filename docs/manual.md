# Guacamole — User Manual

## Overview

Guacamole provides remote desktop access to machines running VNC, RDP,
SSH, Telnet, and Kubernetes. It integrates with Kin OS as a system service
and provides an admin web app for managing connections.

## Admin Web App

The `kin_guacamole_admin` app is available in the Administration category
of the Kin application catalog. It provides four tabs:

### Connections Tab

- **List**: Shows all configured remote desktop connections with status
- **Add**: Create a new connection by specifying name, protocol, host, port,
  and credentials
- **Edit**: Modify an existing connection's parameters
- **Connect**: Manually trigger a connection session
- **Remove**: Delete a connection
- **Reload**: Refresh the connection list from the service

### Sessions Tab

- **List**: Shows active remote desktop sessions
- **Disconnect**: End an active session
- **Refresh**: Update the session list

### Protocols Tab

Displays the list of supported protocols: VNC, RDP, SSH, Telnet, Kubernetes

### Settings Tab

Shows service configuration: connection count, session count, and limits.

## Connection Parameters

| Field | Description |
|-------|-------------|
| Name | Display name for the connection |
| Protocol | VNC, RDP, SSH, Telnet, or Kubernetes |
| Hostname | Target hostname or IP address |
| Port | TCP port (defaults: VNC=5900, RDP=3389, SSH=22, Telnet=23) |
| Username | Login username |
| Password | Login password |
| Domain | Windows domain (RDP only) |
| Security | RDP security: any/nla/tls/rdp |
| Color Depth | Bits per pixel: 8/16/24/32 |
| Width/Height | Display resolution (default: 1024x768) |
| DPI | Dots per inch (default: 96) |
| Features | Audio, video, printing, file transfer, etc. |

## Remote Desktop Client Access

Remote desktop apps connect to `guacamole.service` via its Unix domain
socket. The socket is located at:

- `$XDG_RUNTIME_DIR/kin/guacamole.sock`
- Fallback: `/tmp/kin-guacamole-<uid>.sock`

Clients send a Guacamole protocol `select` instruction:
- `select <protocol>` — e.g. `select vnc` to connect directly
- `select $<connection_id>` — e.g. `select $a1b2c3d4...` to use a stored
  connection (connection ID from the admin web app)

## Building

```bash
# Build from source (auto-fetches libguac from apache/guacamole-server)
make

# Install into Kin build tree
./build-apps.sh

# Build .deb package
./make-debian.sh
```

## Dependencies

- kin.library (pre-built from Kin source)
- libcairo2-dev
- libpng-dev
- libjpeg-dev
- libuuid-dev
- libwebp-dev
- build-essential (gcc, make)
- autoconf, automake, libtool (for building libguac)
