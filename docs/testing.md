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

`scripts/setup-vnc-test-env.sh` stands up a real VNC target so a stored
connection can be exercised for real. It prefers **Docker** (nothing is
installed on the host): it builds `tests/vnc/` (Alpine + Xvfb + x11vnc +
fluxbox) and runs it, then verifies the RFB banner with
`scripts/vnc-rfb-probe.py`.

```bash
make vnc-up                    # or: scripts/setup-vnc-test-env.sh --port 5900
# host=127.0.0.1 port=5900 protocol=vnc password=guacvnc
make vnc-down                  # tear down
```

Then, in the Guacamole admin app (or via the API), add a VNC connection with
those values and click **Connect**. When Docker is unavailable the setup script
prints `apt` instructions for `tigervnc` / `x11vnc` instead.

> The `vnc-rfb-probe.py` helper (`scripts/vnc-rfb-probe.py <host> <port>`) can be
> used on its own to confirm any VNC server is reachable and speaks RFB.

## Continuous / pre-commit

Layer 1 is the gate — it is deterministic, fast, and has no external
dependencies, so run `make test-unit` before every commit. Layers 2 and 3 are
for verifying the live gateway and are opt-in.
