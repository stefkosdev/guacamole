// Standalone viewer window: renders one guacamole connection full-window.
// Opened via kin.classes.Window with query { connid, name }.
import { GuacViewer } from './guac-viewer.js';

function qp(name) {
    try { return new URLSearchParams(location.search).get(name) || ''; }
    catch (_e) { return ''; }
}

function main() {
    const connid = qp('connid');
    const name = qp('name');
    document.title = name ? ('Guacamole — ' + name) : 'Guacamole viewer';

    const host = document.getElementById('host') || document.body;
    host.replaceChildren();

    const wrap = document.createElement('div');
    wrap.className = 'vw-wrap';
    const canvas = document.createElement('div');
    canvas.className = 'vw-canvas';
    const bar = document.createElement('div');
    bar.className = 'vw-bar';
    const title = document.createElement('span');
    title.className = 'vw-title';
    title.textContent = name || connid || '';
    const status = document.createElement('span');
    status.className = 'vw-status';
    const fs = document.createElement('button');
    fs.type = 'button';
    fs.className = 'btn-sm';
    fs.textContent = 'Fullscreen';
    bar.append(title, fs, status);
    wrap.append(canvas, bar);
    host.appendChild(wrap);

    if (!connid) {
        status.textContent = 'No connection id';
        return;
    }

    const viewer = new GuacViewer(canvas, (s) => { status.textContent = 'Viewer: ' + s; });
    fs.addEventListener('click', () => viewer.fullscreen());
    viewer.connect(connid);
    window.addEventListener('beforeunload', () => viewer.disconnect());
}

main();
