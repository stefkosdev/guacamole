# Running the tests

This project is tested in layers, from a fast dependency-free core up to the
full live path through Kin (HTTP → polykernel → service → VNC → browser tunnel).
Each layer has a `make` target.

| Layer | Command | Needs | What it checks |
|-------|---------|-------|----------------|
| 1. Unit | `make test-unit` | just gcc | connection CRUD, defaults, JSON, `.info` persistence, connect reachability, RemoteApp — all against `management.c` directly |
| 1+2 | `make test` | gcc (+ optional running service) | unit tests, then a socket smoke test if the service is up |
| 3. E2E | `make e2e` | docker, **Kin stopped** | full flow under a minimal Kin manager stub against a Docker VNC target |
| 4. Integration | `KIN_SESSION=… make integration` | running Kin (+ session) | HTTP path via the polykernel: add → list → delete, RemoteApp round-trip |
| 5. Tunnel/video | `KIN_SESSION=… make tunnel-test` | running Kin, docker (+ session) | the live viewer path: WS tunnel → service → VNC streams a real desktop |
| 6. Browser render | `KIN_SESSION=… make browser-test` | running Kin, docker, **chromium + node** (+ session) | headless Chromium drives the **deployed** `guac-viewer.js` and asserts the display actually paints |

Start with `make test-unit` — it is deterministic, has no external dependencies,
and is the pre-commit gate.

**One-time setup:** `make install-deps` (or `scripts/install-test-deps.sh`)
installs gcc/make, docker, python3, chromium and node, then the browser-test npm
deps (`puppeteer-core`, no bundled browser). It skips whatever is already present.
Pass `--no-system` to install only the local npm deps, or `--browser` for just the
browser-test stack.

## Prerequisites

- **gcc / make** — for the C service and unit tests.
- **docker** — for layers that need a real VNC target (`e2e`, `tunnel-test`).
  Nothing is installed on the host; a small Alpine + x11vnc image is built once.
- **A running Kin** — for `integration`, `tunnel-test` and `browser-test` (they
  go through the live http.service on `http://localhost:9119`).
- **chromium + node** — for `browser-test` only. `make install-deps` sets these
  up (plus `puppeteer-core`, which drives Chromium over the DevTools protocol and
  does **not** download its own browser — it uses the system chromium).
- **A Kin session cookie** (`KIN_SESSION`) — the polykernel enforces auth before
  routing, so the HTTP layers need it (see below).

## Getting a session cookie (KIN_SESSION)

The value of the `kin_session` cookie from an authenticated browser:

- Firefox/Chrome DevTools → **Application/Storage → Cookies → http://localhost:9119
  → `kin_session`** → copy the value.
- Or, on the box running Kin, grab the most recent one from the log:

  ```bash
  grep -oE 'session_[0-9]+_[0-9]+_[0-9]+_[a-f0-9]+' \
      ~/development/kin/build/logs/kin.log | tail -1
  ```

Then:

```bash
KIN_SESSION=session_XXXX make integration
KIN_SESSION=session_XXXX make tunnel-test
KIN_SESSION=session_XXXX make browser-test
```

Without `KIN_SESSION`, the HTTP layers skip (they cannot assert routing, since
unauthenticated calls all return "No valid Kin session").

## The layers in detail

### 1. Unit — `make test-unit`
Compiles `services/guacamole.service/tests/test_management.c` against
`management.c` (no libguac, no kin.library) and runs it. Covers CRUD, per-protocol
default ports, sizing/color-depth defaults, JSON escaping, the `.info` persistence
round-trip (incl. survive-restart), the `reload` action, corrupt-file tolerance,
connect reachability (reachable → ok, dead port / bad host → fail with no
session), and RemoteApp fields. Expected: `NN checks, 0 failures / ALL TESTS PASSED`.

### 3. E2E — `make e2e`
`scripts/e2e-vnc.sh` runs the whole flow **without the rest of Kin**: it starts
`guacamole.service` under a minimal Kin manager stub (`tests/e2e/fake_manager.c`),
brings up a Docker VNC target, seeds a stored connection, runs the socket smoke
test and a stored-connection probe, and asserts the VNC backend logged an
authenticated client. Tears everything down on exit.

> Run this only when **Kin is not running** — the stub's service binds the same
> Unix socket as the live one.

### 4. Integration — `KIN_SESSION=… make integration`
`scripts/integration-test-kin.sh` drives the real HTTP path
`browser → http.service → polykernel → guacamole.service → .info` and asserts
add → list-shows-it → RemoteApp round-trip → delete → gone. This covers what the
unit tests cannot: polykernel routing, the `sessionid` strip, the IPC message
pump, and persistence — together. Without `KIN_SESSION` it runs only the no-auth
socket layer.

### 5. Tunnel / video — `KIN_SESSION=… make tunnel-test`
`scripts/tunnel-test-kin.sh` exercises the viewer's video path end-to-end: it
brings up a Docker VNC target, adds a connection to it via the API, opens the
WebSocket tunnel exactly like the browser (`tests/e2e/tunnel_probe.py`, no client
handshake — the tunnel does the guacd handshake server-side), and asserts real
display data (`ready/img/blob/sync/cursor`) streams back. Cleans up the connection
and the container afterward. This is the automated proof that the *server* streams
a desktop — but it sends a minimal handshake and so cannot catch bugs that only
real browsers trip. That is what layer 6 is for.

### 6. Browser render — `KIN_SESSION=… make browser-test`
`scripts/browser-test-kin.sh` is the only layer that exercises the **real
in-browser viewer**. It brings up a Docker VNC target, adds a connection, discovers
the deployed `guac-viewer.js` URL, and runs `scripts/browser/drive-viewer.mjs`,
which launches **headless Chromium** (via `puppeteer-core` + the system chromium),
imports the deployed viewer module from the genuine Kin origin, instantiates
`GuacViewer` exactly like the Sessions tab, and asserts the client reaches
`CONNECTED` with a **non-blank** 1024×768 display. A screenshot is written to
`tmp/browser-test.png`. Cleans up the connection and container afterward.

Why this exists: the C/Python probes send a minimal WebSocket handshake, so they
missed two `http.service` bugs that broke **every** real browser (cookie parsing
across headers, and the `Sec-WebSocket-Protocol` echo). See
[docs/browser-testing.md](docs/browser-testing.md) for the harness internals, the
Chromium gotchas (Local Network Access, cookies/origin), and those bugs in detail.

## Manual VNC target

To click **Connect** / **Open in window** in the admin app by hand:

```bash
make vnc-up      # Docker VNC on 127.0.0.1:5900, password guacvnc
make vnc-down    # tear it down
```

See `docs/testing.md` for the layered strategy and design rationale.
