// Headless-Chromium driver for the live remote-desktop viewer.
//
// Loads the DEPLOYED guac-viewer.js module from the real Kin origin, instantiates
// GuacViewer exactly like the Sessions tab does, connects it to a stored VNC/RDP
// connection through the http.service WebSocket tunnel, and asserts the client
// reaches CONNECTED and paints a non-blank display. This is the automated proof
// that the in-browser viewer works — the C/Python probes only exercise the server
// side and cannot catch browser-specific handshake bugs.
//
// Configuration is entirely via environment variables (see scripts/browser-test-kin.sh):
//   KIN_BASE     base URL of the running Kin http.service (default http://localhost:9119)
//   KIN_SESSION  value of the kin_session cookie (required)
//   CONN_ID      id of a stored connection to open (required)
//   VIEWER_URL   absolute URL of the deployed guac-viewer.js (required; a real
//                same-origin document we navigate to before importing the module)
//   CHROMIUM     path to the chromium/chrome binary (default /usr/bin/chromium)
//   SCREENSHOT   optional path to write a PNG of the rendered display
//   TIMEOUT_MS   how long to wait for CONNECTED + first frames (default 20000)
//
// Exit code 0 = the viewer connected and painted; 1 = it did not. The full
// diagnostic JSON is printed to stdout either way.
import puppeteer from 'puppeteer-core';

const ORIGIN = process.env.KIN_BASE || 'http://localhost:9119';
const SESSION = process.env.KIN_SESSION || '';
const CONN_ID = process.env.CONN_ID || '';
const VIEWER_URL = process.env.VIEWER_URL || '';
const CHROMIUM = process.env.CHROMIUM || '/usr/bin/chromium';
const SCREENSHOT = process.env.SCREENSHOT || '';
const TIMEOUT_MS = parseInt(process.env.TIMEOUT_MS || '20000', 10);

function die(msg) {
  console.error('browser-test: ' + msg);
  process.exit(2);
}
if (!SESSION) die('KIN_SESSION is required');
if (!CONN_ID) die('CONN_ID is required');
if (!VIEWER_URL) die('VIEWER_URL is required');

const host = new URL(ORIGIN).hostname;

const browser = await puppeteer.launch({
  executablePath: CHROMIUM,
  headless: 'new',
  args: [
    '--no-sandbox', '--disable-setuid-sandbox', '--use-gl=swiftshader',
    // Chromium's Local/Private Network Access checks block a loopback WebSocket
    // when the initiating document's IP address space is "unknown" (e.g. a
    // synthetic page). We navigate to a genuine localhost document below, but
    // keep these off so the harness is robust across Chromium versions.
    '--disable-features=LocalNetworkAccessChecks,BlockInsecurePrivateNetworkRequests,PrivateNetworkAccessSendPreflights,PrivateNetworkAccessForNavigations',
  ],
});

try {
  const page = await browser.newPage();
  await page.setViewport({ width: 900, height: 700 });

  const guacLogs = [];
  page.on('console', (m) => { const t = m.text(); if (t.includes('[guac]')) guacLogs.push(t); });
  page.on('pageerror', (e) => guacLogs.push('PAGEERROR ' + e.message));

  await page.setCookie({ name: 'kin_session', value: SESSION, domain: host, path: '/' });

  // Navigate to a REAL 200 document on the genuine Kin origin (the viewer module
  // file itself). This gives a real local-origin page — so the cookie and the
  // loopback WebSocket are treated as same address space — that does not
  // self-navigate the way the Kin shell (index.html) does. We then import the
  // module from page context and drive it.
  await page.goto(VIEWER_URL, { waitUntil: 'domcontentloaded', timeout: 30000 });

  const result = await page.evaluate(async (VIEWER_URL, CONN_ID, TIMEOUT_MS) => {
    const box = document.createElement('div');
    box.style.cssText = 'position:fixed;left:0;top:0;width:800px;height:600px;background:#111;z-index:2147483647';
    document.body.appendChild(box);

    const { GuacViewer } = await import(VIEWER_URL);
    const states = [];
    const viewer = new GuacViewer(box, (s) => states.push(s));
    viewer.connect(CONN_ID);

    const t0 = Date.now();
    while (Date.now() - t0 < TIMEOUT_MS) {
      await new Promise((r) => setTimeout(r, 250));
      if (states.includes('connected') && viewer.display && viewer.display.getWidth() > 0) {
        await new Promise((r) => setTimeout(r, 1500)); // let frames paint
        break;
      }
      if (states.some((s) => String(s).startsWith('error'))) break;
    }

    let w = 0, h = 0, nonBlank = false;
    try {
      w = viewer.display.getWidth();
      h = viewer.display.getHeight();
      for (const c of box.querySelectorAll('canvas')) {
        if (!c.width || !c.height) continue;
        const ctx = c.getContext('2d');
        if (!ctx) continue;
        const d = ctx.getImageData(0, 0, Math.min(c.width, 200), Math.min(c.height, 200)).data;
        for (let i = 0; i < d.length; i += 4) {
          if (d[i] || d[i + 1] || d[i + 2]) { nonBlank = true; break; }
        }
        if (nonBlank) break;
      }
    } catch (_e) { /* ignore */ }

    return { states, width: w, height: h, canvasCount: box.querySelectorAll('canvas').length, nonBlank };
  }, VIEWER_URL, CONN_ID, TIMEOUT_MS);

  if (SCREENSHOT) await page.screenshot({ path: SCREENSHOT }).catch(() => {});

  const connected = result.states.includes('connected') && result.width > 0;
  console.log(JSON.stringify({ pass: connected, result, guacLogs, screenshot: SCREENSHOT || null }, null, 2));
  process.exitCode = connected ? 0 : 1;
} finally {
  await browser.close();
}
