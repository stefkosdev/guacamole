# Guacamole — Architecture & API Reference

## Overview

Guacamole is a Kin remote desktop gateway service that embeds Apache
Guacamole's libguac to provide VNC, RDP, SSH, Telnet, and Kubernetes
remote access. It replaces the standard `guacd` daemon with native
Kin IPC integration.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    Kin Workspace (WebKit)                     │
│  ┌────────────────────────────────────────────────────────┐  │
│  │           kin_guacamole_admin (web app)                │  │
│  │  CRUD connections  │  Monitor sessions                │  │
│  └─────────┬──────────┴──────────┬────────────────────────┘  │
│            │ HTTP API            │ Guacamole protocol         │
│            ▼                     ▼                            │
│  ┌────────────────────────────────────────────────────────┐  │
│  │              Kin HTTP Service                           │  │
│  │  /api/guacamole/* → IPC → guacamole.service           │  │
│  └────────────────────────────────────────────────────────┘  │
└────────────────────────────────┬─────────────────────────────┘
                                 │
                    ┌────────────┴────────────┐
                    │   Kin IPC Bus (kin.library)
                    ▼                         ▼
          ┌──────────────────┐    ┌──────────────────────┐
          │  kin-manager     │    │  guacamole.service   │
          │  (IPC router)    │    │  (C daemon)          │
          └──────────────────┘    │                      │
                                  │  Unix socket:        │
                                  │  /tmp/kin-guacamole- │
                                  │  <uid>.sock          │
                                  └──────────┬───────────┘
                                             │
                                   ┌─────────▼──────────┐
                                   │  Remote Desktop    │
                                   │  Apps (Guacamole   │
                                   │  protocol clients) │
                                   └────────────────────┘
```

## Management API

All requests go through Kin IPC via `/api/guacamole/<command>`.
The Kin HTTP service routes these to `guacamole.service` via IPC events.

### Connections

| Method | Endpoint | Action |
|--------|----------|--------|
| GET | `/api/guacamole/connections` | List all connections |
| POST | `/api/guacamole/connections` | `action: add` — create connection |
| POST | `/api/guacamole/connections` | `action: update` — modify connection |
| POST | `/api/guacamole/connections` | `action: delete` — remove connection |
| POST | `/api/guacamole/connections` | `action: get` — get single connection |

### Sessions

| Method | Endpoint | Action |
|--------|----------|--------|
| GET | `/api/guacamole/active` | List active sessions |
| POST | `/api/guacamole/connection` | `action: connect` — start session |
| POST | `/api/guacamole/connection` | `action: disconnect` — end session |

### System

| Method | Endpoint | Action |
|--------|----------|--------|
| GET | `/api/guacamole/protocols` | List supported protocols |
| GET | `/api/guacamole/settings` | Service configuration |

## Connection Data Model

```json
{
  "id": "a1b2c3d4...",
  "name": "My Server",
  "protocol": "rdp|vnc|ssh|telnet|kubernetes",
  "hostname": "192.168.1.100",
  "port": 3389,
  "username": "admin",
  "password": "***",
  "private_key": "ssh-rsa ...",
  "domain": "WORKGROUP",
  "security": "any|nla|tls|rdp",
  "color_depth": "8|16|24|32",
  "width": 1024,
  "height": 768,
  "dpi": 96,
  "enable_audio": false,
  "enable_video": false,
  "enable_printing": false,
  "enable_file_transfer": false,
  "enable_wallpaper": false,
  "enable_theming": false,
  "enable_font_smoothing": false,
  "enable_full_window_drag": false,
  "enable_menu_animation": false,
  "disable_copy": false,
  "disable_paste": false,
  "active": false,
  "created": 1712345678,
  "last_used": 1712345678
}
```

## Session Data Model

```json
{
  "id": "session-id-64-chars",
  "connection_id": "parent-connection-id",
  "username": "admin",
  "session_id": "web-1712345678000",
  "protocol": "rdp",
  "hostname": "192.168.1.100",
  "port": 3389,
  "started": 1712345678,
  "last_active": 1712345678,
  "active": true
}
```

## Connection Flow

1. Admin adds connection via `kin_guacamole_admin` web app
2. Web app calls `POST /api/guacamole/connections` with connection details
3. Connection stored in-memory in `guacamole.service`
4. Remote desktop app connects via Unix socket with `select $<connection_id>`
5. Service looks up connection, loads protocol plugin, sets env vars
6. Service creates `guac_user` and calls `guac_user_handle_connection()`
7. Blocks until disconnect, then cleans up

## Kin IPC Integration

The service registers event handlers for `"guacamole"` and `"api"` events
on the Kin IPC bus via `kin_message_callback()`. Management commands are
parsed from the message body and dispatched to `guac_mgmt_handle()`.

Commands follow the format: `guacamole/<command>|<json-body>`
Or: `<command>|<json-body>` (when event is already `"guacamole"` or `"api"`)
