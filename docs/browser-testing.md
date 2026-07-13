# Browser render testing (Chromium + Kin)

This is the deep reference for **layer 6** of the test suite — driving a real
browser against the live viewer. For the layer overview see
[../TESTING.md](../TESTING.md).

The other layers (unit → tunnel-test) prove the *server* works: the WebSocket
tunnel selects the connection, does the guacd handshake server-side, and streams
display instructions. They send a **minimal** WebSocket handshake, though, so they
cannot see what a real browser does differently. Two `http.service` handshake bugs
slipped past every C/Python probe and yet broke **every** browser (Firefox and
Chrome alike). Layer 6 exists to catch exactly that class of bug.

## What it does

`make browser-test` → `scripts/browser-test-kin.sh`:

1. Brings up the Docker VNC target (`scripts/setup-vnc-test-env.sh`, 127.0.0.1:5900).
2. Adds a VNC connection via the HTTP API and captures its id.
3. Discovers the deployed viewer URL by probing the repository path for a `200`.
4. Runs `scripts/browser/drive-viewer.mjs` under **headless Chromium**.
5. Asserts the client reached `CONNECTED` with a non-blank display; writes a
   screenshot to `tmp/browser-test.png`; deletes the connection and tears down VNC.

`drive-viewer.mjs` (the driver) uses **`puppeteer-core`** — the "core" package
speaks the Chrome DevTools Protocol but does **not** bundle/download a browser; it
drives the **system chromium** (`/usr/bin/chromium`, or `$CHROMIUM`). It:

1. Sets the `kin_session` cookie for the Kin host.
2. Navigates to the deployed `guac-viewer.js` URL — a **real 200 document on the
   genuine Kin origin**. (Why not the app's `index.html`? The Kin shell
   self-navigates and destroys the page's JS execution context. The viewer module
   file is a stable same-origin document that does not.)
3. From page context, `import()`s the deployed module and instantiates
   `GuacViewer` against a `<div>` — exactly what the Sessions tab does.
4. Waits for the `onStatus('connected')` callback, then samples the display
   `<canvas>` for any non-black pixel and reads `display.getWidth()/getHeight()`.
5. Prints a JSON verdict and exits `0` (painted) or `1` (did not).

Configuration is entirely via env vars: `KIN_BASE`, `KIN_SESSION`, `CONN_ID`,
`VIEWER_URL`, `CHROMIUM`, `SCREENSHOT`, `TIMEOUT_MS`.

## Prerequisites & install

```bash
make install-deps          # gcc/make, docker, python3, chromium, node + puppeteer-core
# or just the browser stack:
scripts/install-test-deps.sh --browser
# or, if system packages are already present, only the local npm deps:
scripts/install-test-deps.sh --no-system
```

`puppeteer-core` is installed into `scripts/browser/node_modules` (git-ignored).
`PUPPETEER_SKIP_DOWNLOAD=1` is set so npm does not fetch a browser — the system
chromium is used.

Then, with a running Kin and a session cookie (see TESTING.md for how to get one):

```bash
KIN_SESSION=session_XXXX make browser-test
```

## How Kin serves the admin app (what the harness relies on)

- The app is a Kin Application deployed under
  `build/repository/Applications/Administration/kin_guacamole_admin/`. Its files
  are served at `…/repository/Applications/Administration/kin_guacamole_admin/…`
  (the harness probes this path for the viewer module).
- Static assets are cache-busted under a build-id (`/<build-id>/…`, where the
  build-id is the Kin repo git HEAD, pinned at http.service startup). **Hot-patching
  files into `build/` does not bust the browser cache** — change the build-id (a new
  Kin commit + restart) or disable the browser cache while iterating.
- The WebSocket tunnel endpoint is `GET /api/guacamole/tunnel-ws?id=<conn-id>`,
  authenticated by the `kin_session` cookie. The tunnel does the guacd handshake
  server-side (servlet-style) and relays display instructions to the browser.

## Chromium gotchas (learned the hard way)

**Local / Private Network Access.** Recent Chromium blocks a request to a
loopback address (`ws://localhost:9119`) when the initiating document's IP address
space is classified "unknown" or "public" — you get
`net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS`. A **synthetic** page (e.g. one
served via request interception, or `about:blank`) has an unknown address space and
trips this. Two defenses, both used by the driver:
- Navigate to a **real** `localhost` document (the viewer module URL), so the page
  and the loopback WS are the same address space.
- Launch chromium with `--disable-features=LocalNetworkAccessChecks,`
  `BlockInsecurePrivateNetworkRequests,PrivateNetworkAccessSendPreflights,`
  `PrivateNetworkAccessForNavigations` for robustness across versions.

**Cookies are port-agnostic but origin matters.** A `kin_session` cookie set for
host `localhost` is sent to `localhost:9119` even from a page on a different port,
but the page must be a genuine `http://localhost` origin for the cookie and WS to
behave like the real app. The driver navigates to the real origin instead of
faking one.

**Subprotocols are strict.** `guacamole-common-js`'s `WebSocketTunnel` opens the
socket with the `guacamole` subprotocol. If the client offers a subprotocol the
server **must** echo `Sec-WebSocket-Protocol` in the `101`, or Chromium aborts the
handshake with *"Sent non-empty 'Sec-WebSocket-Protocol' header but no response was
received."* (This was bug 2 below.)

**`webSocketHandshakeResponseReceived` not firing** in a CDP trace means Chromium
rejected the upgrade before surfacing it — treat it as "no valid 101", not "no data".

## The two bugs this layer caught (Kin `http.service`)

Both are header-parsing bugs that only trip on request shapes browsers produce but
the minimal probes did not. Fixed in the Kin repo (`services/http/server/…`).

**Bug 1 — `parse_cookie()` over-read the cookie value (→ 403).**
It located `kin_session=` then searched for the terminating `;` with a *global*
`strchr()`. The session value has no `;`, so the scan ran past the Cookie line's
CRLF into a later header and stopped at a `;` there — and **every browser** sends
`Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits` on a WS
handshake. The returned "session" was the real value + CRLF + header junk, so
session validation failed and the tunnel handshake was rejected with **403**.
*Fix:* bound the `;` search to the Cookie header line.

**Bug 2 — `hdr_line_value_ci()` dropped the last header line (→ no subprotocol).**
It found line boundaries with `strstr(p, "\r\n")` and did `if(!cr) break;`. The
request as passed to the handshake builder has its terminating `\r\n\r\n` stripped,
so the **final** header line has no trailing CRLF and was skipped. Browsers place
`Sec-WebSocket-Protocol` **last** (after `Sec-WebSocket-Extensions`), so it was not
echoed in the `101` and Chromium aborted the handshake. *Fix:* treat end-of-string
as a line terminator so the last header is still matched.

Order matters because Chromium sends `Sec-WebSocket-Extensions` before
`Sec-WebSocket-Protocol`; a probe that put Protocol first happened to dodge bug 2.

## Diagnostic techniques (for the next weird one)

These are the tools that localized the bugs above — reach for them when the browser
fails but the server-side probes pass:

- **CDP WebSocket events.** In the driver, attach a `CDPSession` and log
  `Network.webSocketCreated / willSendHandshakeRequest / handshakeResponseReceived /
  webSocketFrameReceived / webSocketFrameError / webSocketClosed`. This shows the
  exact request headers, the `101` (or its absence), and the close code — the
  network-layer truth, below the JS library.
- **Bare `WebSocket` in page context.** `new WebSocket(url, 'guacamole')` with
  `onopen/onmessage/onerror/onclose` isolates the transport from
  `guacamole-common-js`. Toggling the subprotocol (offer vs. none) is how bug 2 was
  pinned: with no subprotocol the socket opened and streamed; with `guacamole` it
  1006'd.
- **Logging TCP proxy.** A tiny `localhost:9120 → :9119` proxy that dumps both
  directions gives the **exact bytes** Chromium sends and receives — this is how the
  missing `Sec-WebSocket-Protocol` in the `101` was seen directly.
- **Raw-socket handshake bisection.** A Python script that sends a chosen header
  set and reads the status line, then removes/reorders headers one at a time, is how
  bug 1 was narrowed to "any header with a `;` in its value" and bug 2 to header
  order. Reproducing a browser bug with a 20-line raw socket makes it debuggable.
- **Temporary server-side dump.** When parsing is suspect, a short-lived
  `fopen("/tmp/…","a")` + escaped dump of the exact buffer the parser receives beats
  guessing — it revealed the stripped trailing CRLF that caused bug 2.
