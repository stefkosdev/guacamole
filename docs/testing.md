# Guacamole — Testing Guide

The gateway is tested in layers, from a fast dependency-free core up to a full
end-to-end connection against a real VNC server. Run everything with:

```bash
make test
```

## Layer 1 — Management / persistence tests (always run)

Dependency-free C tests compiled directly against `management.c` (no libguac,
no kin.library). They drive the public dispatcher `guac_mgmt_handle()` exactly
as the service does and cover:

- connection CRUD (`add` / `get` / `update` / `delete`) and list
- protocol validation and per-protocol default ports
- default width / height / dpi / color depth
- JSON escaping (quotes / backslashes in names and passwords)
- **`.info` persistence round-trip** — add, simulate a restart, verify the
  connection (incl. credentials) is restored and `active` is reset
- the `reload` action (and that it does not duplicate)
- `settings` reporting `persistent` / `storage_path`
- tolerance of a corrupt store file and of incomplete objects
- `connect` recording and persisting `last_used`

Run directly:

```bash
make test-unit
# or
cd services/guacamole.service/tests && make run
```

Each test isolates its store via `KIN_GUACAMOLE_STATE` pointing at a temp file,
so it never touches your real `Guacamole.info`.

## Layer 2 — Service socket smoke test (opt-in)

`scripts/guac-smoke-test.py` connects to the running service's Unix socket,
performs a Guacamole `select` handshake for a bogus protocol, and expects an
`error` instruction back. This exercises the accept loop, connection thread,
plugin lookup, and error reporting — without a real backend.

It **requires the service to be running**. Because `guacamole.service` calls
`kin_init()` with a manager PID, it normally runs under the Kin manager. Start
Kin (or the service with a manager PID), then:

```bash
python3 scripts/guac-smoke-test.py
```

The socket path is auto-discovered (`$XDG_RUNTIME_DIR/kin/guacamole.sock`, else
`/tmp/kin-guacamole-<uid>.sock`). It prints `SKIP` and exits 2 when the socket
is absent, so `make test` stays green when the service is not running.

## Layer 3 — End-to-end against a real VNC server

### Automated (`make e2e`)

`scripts/e2e-vnc.sh` runs the whole path automatically and asserts on the
result:

```bash
make e2e            # or: scripts/e2e-vnc.sh [--port 5905] [--keep]
```

It brings up the VNC target (Docker), starts `guacamole.service` under a
**minimal Kin manager stub** (`tests/e2e/fake_manager.c` — no full Kin, no
nginx, no DB), seeds a stored VNC connection in a temporary `.info` store, then:

1. runs the socket smoke test (`select` a bogus protocol → expect `error`);
2. runs the stored-connection probe (`tests/e2e/guac_client_probe.py`) which
   does `select $<id>` with **empty client argv** and expects `ready`;
3. asserts the VNC target logged an **authenticated** client.

Step 3 is the strong assertion: with empty client argv, the host / port /
password can only have come from the stored connection, so an authenticated
VNC session proves the persisted connection reached the backend plugin. It all
tears down on exit (add `--keep` to leave the target up).

> How the stub works: `kin_init(NULL, key)` runs the Kin library in server mode
> and creates the `/kin_shm_<key>` IPC segment; the service, launched with the
> stub's PID as its manager key, attaches as a client. The Guacamole
> Unix-socket path is independent of Kin IPC, so this is enough to exercise the
> full connection flow without the rest of Kin.

### Manual target only

To stand up just the VNC target and click **Connect** in the admin app:

```bash
make vnc-up                    # or: scripts/setup-vnc-test-env.sh --port 5900
# host=127.0.0.1 port=5900 protocol=vnc password=guacvnc
make vnc-down                  # tear down
```

Docker is preferred (nothing is installed on the host): it builds `tests/vnc/`
(Alpine + Xvfb + x11vnc + fluxbox). When Docker is unavailable the setup script
prints `apt` instructions for `tigervnc` / `x11vnc` instead.

> The `vnc-rfb-probe.py` helper (`scripts/vnc-rfb-probe.py <host> <port>`) can be
> used on its own to confirm any VNC server is reachable and speaks RFB.

## Layer 4 — Live Kin integration (through the polykernel)

`scripts/integration-test-kin.sh` (`make integration`) exercises the full path a
real request takes:

```
browser -> http.service -> polykernel (router) -> guacamole.service -> .info
```

It proves what the unit tests cannot: the polykernel route family for
`/api/guacamole/*`, the `sessionid` strip (a read must not be mistaken for an
action — the bug where added connections never appeared in the list), the
service's IPC message pump, and persistence — all together.

The polykernel enforces auth **before** routing, so the HTTP assertions need a
valid session. Pass it via `KIN_SESSION` (the `kin_session` cookie from an
authenticated browser — DevTools ▸ Application ▸ Cookies):

```bash
KIN_SESSION=<kin_session> make integration
```

It adds a connection, asserts a subsequent list shows it, then deletes it and
asserts it is gone. Without `KIN_SESSION`, only the no-auth layer runs (the
service's protocol socket), since unauthenticated API calls all return
"No valid Kin session" and cannot assert routing.

## Continuous / pre-commit

Layer 1 is the gate — it is deterministic, fast, and has no external
dependencies, so run `make test-unit` before every commit. Layers 2 and 3 are
for verifying the live gateway and are opt-in.
