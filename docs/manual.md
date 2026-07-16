# Guacamole — User Manual

## Overview

Guacamole provides remote desktop access to machines running VNC, RDP,
SSH, Telnet, and Kubernetes. It integrates with Kin OS as a system service
and provides an admin web app for managing connections.

There are two ways to manage connections: the **admin web app** (below) and the
**`guacamole` KinDOS command** (see [Command-line interface](#command-line-interface-kindos)).
Both talk to the same service over the same API, so they are fully interchangeable.

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
| Ignore Cert | RDP: accept the server's untrusted/self-signed TLS certificate (needed for most standalone Windows hosts) |
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

## Command-line interface (KinDOS)

The `guacamole` command does everything the admin app's Connections and Sessions
tabs do, from the KinDOS shell (or over SSH / the web terminal). It talks to
`guacamole.service` over the same IPC the web app uses and prints the service's
JSON response, so it scripts cleanly.

```
guacamole op=<operation> [field=value ...]
```

### Operations

| Operation | Arguments | Does |
|-----------|-----------|------|
| `list` | — | list all stored connections |
| `get` | `id=<id>` | show one connection (incl. stored password) |
| `add` | connection fields | create a connection; returns its `connection_id` |
| `update` | `id=<id>` + fields | change text fields and port of a connection |
| `delete` | `id=<id>` | remove a connection |
| `reload` | — | re-read connections from disk |
| `sessions` | — | list active sessions |
| `protocols` | — | list supported protocols |
| `connect` | `id=<id>` | start a session for a stored connection |
| `disconnect` | `id=<id>` | end an active session |

### Connection fields (for `add` / `update`)

`name` `protocol` `hostname` `port` `username` `password` `private_key`
`domain` `security` `color_depth` `remote_app` `remote_app_dir` `remote_app_args`
`width` `height` `dpi` — and the boolean feature flags `enable_audio`
`enable_video` `enable_printing` `enable_file_transfer` `enable_wallpaper`
`enable_theming` `enable_font_smoothing` `enable_full_window_drag`
`enable_menu_animation` `disable_copy` `disable_paste` `ignore_cert`.

> **RDP to a self-signed host** (a standalone Windows box): set `ignore_cert=true`,
> otherwise guacd rejects the connection with *"SSL/TLS connection failed
> (untrusted/self-signed certificate?)"*. Leave `security=any` so guacd negotiates
> NLA/TLS as the server requires.

Booleans accept `1`/`true`/`yes`/`on`. Only the fields you pass are sent; the rest
take service defaults on `add`, or stay unchanged on `update`.

> `update` changes the text fields and the port (this matches the service, and so
> the web app's Edit). To change a feature flag or the display size, delete the
> connection and `add` it again with the new values.

### Examples

```bash
# Create a VNC connection and capture its id
guacamole op=add name=Office protocol=vnc hostname=10.0.0.5 port=5900 password=secret

# List, inspect, retarget, and remove it
guacamole op=list
guacamole op=get id=7d3ad049421b50ac38235a44b1f7cfe7
guacamole op=update id=7d3ad049421b50ac38235a44b1f7cfe7 hostname=10.0.0.6
guacamole op=delete id=7d3ad049421b50ac38235a44b1f7cfe7

# A Windows RDP host (self-signed cert -> ignore_cert=true)
guacamole op=add name="Windows VM" protocol=rdp hostname=10.0.0.9 port=3389 \
    username=alice password=secret security=any ignore_cert=true

# An RDP RemoteApp (single published application)
guacamole op=add name=Calc protocol=rdp hostname=win.example.com port=3389 \
    username=alice password=secret ignore_cert=true remote_app=calc

# Sessions and protocols
guacamole op=sessions
guacamole op=protocols
```

Run `guacamole --help` for the full argument list. Inside Kin the shell passes
`manager_pid` for you; when running the binary standalone, pass `manager_pid=<pid>`
or set `KIN_MANAGER_PID`.

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

Get everything ready to build from source with:

```bash
make install-build-deps        # or: scripts/install-build-deps.sh
```

The script does two things:

1. **Installs the system `-dev` libraries + toolchain** (apt / dnf / pacman).
2. **Fetches and builds libguac itself** — we do not use a distro libguac; the
   service statically links our own `libguac.a`. This git-clones
   apache/guacamole-server (if missing) and compiles it against the libraries
   from step 1 (idempotent once `dependencies/.../libguac.a` exists).

Flags: `--no-libguac` (only system packages), `--libguac` (only fetch + build
libguac), `--list` (print the package set and exit).

The system packages it installs — the `-dev` libraries `libguac` and the protocol
plugins (VNC, RDP, SSH, Telnet, Kubernetes) need:

- **build tools**: build-essential (gcc, g++, make), git, autoconf, automake,
  libtool, pkg-config
- **libguac core**: cairo, jpeg, png, uuid, webp, ssl
- **protocol plugins**: libvncserver (VNC), freerdp2 (RDP), pango + libssh2 (SSH),
  libtelnet (Telnet), libwebsockets (Kubernetes), pulse + vorbis (audio)

Not installed by the script: **kin.library** — that is produced by the Kin build
and wired in by `build-apps.sh`.
