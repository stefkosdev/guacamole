// Live remote-desktop viewer built on guacamole-common-js.
//
// Connects to a stored connection through the Kin WebSocket tunnel
// (/api/guacamole/tunnel-ws?id=<connection-id>), renders the remote display
// (video), plays audio, and forwards keyboard/mouse. One Viewer instance owns
// one live session; switching sessions creates a fresh Viewer.
import Guacamole from './guacamole-common.min.js';

function tunnelWsUrl() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return proto + '//' + location.host + '/api/guacamole/tunnel-ws';
}

export class GuacViewer {
    /**
     * @param {HTMLElement} container  where the remote display is mounted
     * @param {(state:string)=>void} onStatus  called with human-readable state/errors
     */
    constructor(container, onStatus) {
        this.container = container;
        this.onStatus = onStatus || (() => {});
        this.client = null;
        this.keyboard = null;
        this.mouse = null;
        this.display = null;
        this._connected = false;
    }

    connect(connectionId) {
        this.disconnect();
        this.container.replaceChildren();

        const tunnel = new Guacamole.WebSocketTunnel(tunnelWsUrl());
        const client = new Guacamole.Client(tunnel);
        this.client = client;

        const display = client.getDisplay();
        this.display = display;
        const el = display.getElement();
        el.classList.add('guac-display');
        this.container.appendChild(el);

        client.onstatechange = (state) => {
            // 0 IDLE, 1 CONNECTING, 2 WAITING, 3 CONNECTED, 4 DISCONNECTING, 5 DISCONNECTED
            const names = ['idle', 'connecting', 'waiting', 'connected',
                           'disconnecting', 'disconnected'];
            if (state === 3) {
                this._connected = true;
                this.onStatus('connected');
                this._fit();
            } else {
                this.onStatus(names[state] || String(state));
            }
        };

        client.onerror = (status) => {
            this._connected = false;
            this.onStatus('error: ' + (status && status.message ? status.message : 'connection failed'));
            this.disconnect();
        };

        // Input: mouse over the display, keyboard on the document.
        const mouse = new Guacamole.Mouse(el);
        this.mouse = mouse;
        const send = (s) => { if (this.client) this.client.sendMouseState(s); };
        mouse.onmousedown = mouse.onmouseup = mouse.onmousemove = send;

        const keyboard = new Guacamole.Keyboard(document);
        this.keyboard = keyboard;
        keyboard.onkeydown = (sym) => { if (this.client) this.client.sendKeyEvent(1, sym); };
        keyboard.onkeyup = (sym) => { if (this.client) this.client.sendKeyEvent(0, sym); };

        try {
            client.connect('id=' + encodeURIComponent(connectionId));
        } catch (e) {
            this.onStatus('error: ' + (e && e.message ? e.message : e));
        }

        this._onResize = () => this._fit();
        window.addEventListener('resize', this._onResize);
    }

    /** Scale the remote display to fit the container without stretching. */
    _fit() {
        if (!this.display) return;
        const w = this.display.getWidth();
        const h = this.display.getHeight();
        if (!w || !h) return;
        const cw = this.container.clientWidth || w;
        const ch = this.container.clientHeight || h;
        const scale = Math.min(cw / w, ch / h, 1);
        if (scale > 0) this.display.scale(scale);
    }

    fullscreen() {
        const t = this.container;
        if (document.fullscreenElement) {
            document.exitFullscreen?.();
        } else if (t.requestFullscreen) {
            t.requestFullscreen().then(() => setTimeout(() => this._fit(), 100)).catch(() => {});
        }
    }

    disconnect() {
        if (this._onResize) {
            window.removeEventListener('resize', this._onResize);
            this._onResize = null;
        }
        if (this.keyboard) { this.keyboard.onkeydown = this.keyboard.onkeyup = null; this.keyboard = null; }
        if (this.mouse) { this.mouse.onmousedown = this.mouse.onmouseup = this.mouse.onmousemove = null; this.mouse = null; }
        if (this.client) {
            try { this.client.disconnect(); } catch (_e) { /* ignore */ }
            this.client = null;
        }
        this.display = null;
        this._connected = false;
    }
}
