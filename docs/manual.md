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

Shows service configuration: connection count, session count, limits, and the
path of the persistent connection store (`storage_path`).

## Saving & Loading Connections

Connections you create are saved automatically — there is no separate "Save"
button. The service stores them in a JSON `.info` file, the same convention Kin
apps use for their settings (e.g. `Wallpaper.info`). Your connections therefore
survive a service restart or a reboot.

- **Where**: `Guacamole.info`, under your Kin data directory
  (`$XDG_DATA_HOME/kin/guacamole/` or `~/.local/share/kin/guacamole/`).
  Set `KIN_GUACAMOLE_STATE` to override the full path.
- **What is saved**: every connection and all of its parameters (including
  stored credentials), plus its `created` / `last_used` timestamps.
- **What is NOT saved**: active sessions. These are live and are cleared when
  the service stops; on the next start every connection shows as `Idle`.
- **When**: the store is rewritten on every add, edit, remove, and connect.
- **Manual reload**: the connection list can be re-read from disk at any time
  via `POST /api/guacamole/connections {"action":"reload"}` (or the app's
  Reload button, which refreshes from the service).

> Note: the file contains credentials in plain text. It is written with
> owner-only permissions (`0600`) inside your user data directory.

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
