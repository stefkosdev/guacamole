# Guacamole — Kin Remote Desktop Gateway

## Project Overview

Guacamole is **Kin's remote desktop gateway** — a C service that embeds Apache Guacamole's libguac to provide VNC, RDP, SSH, Telnet, and Kubernetes remote access. It replaces the standard `guacd` daemon by integrating directly with Kin's IPC system.

The admin web app (`kin_guacamole_admin`) provides a GUI for managing remote desktop connections and monitoring active sessions.

## Technology Stack

- **Service**: C daemon using libguac (statically linked from apache/guacamole-server)
- **Protocols**: VNC, RDP, SSH, Telnet, Kubernetes (via libguac-client-*.so plugins)
- **IPC**: Kin messaging system via `kin.library`
- **Transport**: Unix domain socket for Guacamole protocol clients
- **Frontend**: JavaScript with Kin UI framework (`kin-ui.js`)

## Architecture

```
guacamole.service (C daemon)
  ├── IPC: kin.library ←→ kin-manager (management API)
  │     GET  /api/guacamole/connections  → list connections
  │     POST /api/guacamole/connections  → add/update/delete
  │     GET  /api/guacamole/active       → list active sessions
  │     POST /api/guacamole/connection   → connect/disconnect
  │     GET  /api/guacamole/protocols    → list supported protocols
  │     GET  /api/guacamole/settings     → service configuration
  │
  ├── Unix socket: /tmp/kin-guacamole-<uid>.sock
  │     Guacamole protocol clients (remote desktop web apps)
  │     connect via "select <protocol>" or "select $<connection_id>"
  │
  └── libguac: statically linked + libguac-client-*.so plugins
        vnc, rdp, ssh, telnet, kubernetes

kin_guacamole_admin (web app)
  ├── manifest.json    # App descriptor
  ├── main.js          # Window bootstrap via kin.classes.Window
  ├── app.js           # Logic: CRUD connections, manage sessions
  ├── ui.json          # Declarative kin-UI layout
  └── guacamole-view.css
```

## Key Files

| File | Purpose |
|------|---------|
| `services/guacamole.service/service.c` | Main daemon — Unix socket, IPC handler, guacamole protocol |
| `services/guacamole.service/management.c` | Connection/session CRUD, JSON API handlers |
| `services/guacamole.service/management.h` | Data structures: GuacConnection, GuacSession |
| `kin/main.js` | Window entry — creates kin.classes.Window |
| `kin/app.js` | Admin UI logic — API calls to guacamole.service |
| `kin/ui.json` | Declarative tabbed UI (Connections, Sessions, Protocols, Settings) |

## Kin Integration Points

### Management API
The service registers event handlers for `"guacamole"` and `"api"` events on the Kin IPC bus. The Kin HTTP service routes `/api/guacamole/*` requests to the guacamole service via IPC.

### Connection Flow
1. Admin adds connection via `kin_guacamole_admin` web app
2. Web app calls `POST /api/guacamole/connections` with connection details
3. Connection stored in-memory in `guacamole.service`
4. Remote desktop app connects via Unix socket with `select $<connection_id>`
5. Service loads the appropriate protocol plugin and sets env vars from stored connection
6. Service creates `guac_user` and calls `guac_user_handle_connection()` (blocks until disconnect)

## Building

```bash
./build-apps.sh              # Install to Kin build
make                         # Build guacamole.service (auto-fetches libguac)
./make-debian.sh             # Build .deb package
```

## Dependencies

- libguac (auto-fetched from apache/guacamole-server and built)
- kin.library (must be pre-built from Kin source)
- System: libcairo2-dev, libpng-dev, libjpeg-dev, libuuid-dev, libwebp-dev
