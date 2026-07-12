import { createApp, registerKinUI } from '../kin_ui/kin-ui.js';
import { GuacViewer } from './guac-viewer.js';

let connections = [];
let sessions = [];
let editIndex = -1;
let viewer = null;
let selectedSessionIdx = -1;

function inputValue(el) {
    if (el && typeof el.kinGet === 'function')
        return el.kinGet('value') || '';
    return '';
}

function setInputValue(el, v) {
    if (el && typeof el.kinSet === 'function')
        el.kinSet('value', v == null ? '' : String(v));
}

function switchChecked(el) {
    if (el && typeof el.kinGet === 'function')
        return !!el.kinGet('checked');
    return false;
}

function setSwitchChecked(el, on) {
    if (el && typeof el.kinSet === 'function')
        el.kinSet('checked', !!on);
}

function setHidden(ui, id, hidden) {
    const el = ui.getById(id);
    if (!el) return;
    if (hidden)
        el.setAttribute('hidden', '');
    else
        el.removeAttribute('hidden');
}

function selectValue(el) {
    if (el && typeof el.kinGet === 'function')
        return el.kinGet('value') || '';
    return '';
}

function setSelectValue(el, v) {
    if (el && typeof el.kinSet === 'function')
        el.kinSet('value', v == null ? '' : String(v));
}

async function apiJson(path, options = {}) {
    const r = await fetch(path, { credentials: 'include', ...options });
    try {
        return await r.json();
    } catch (_e) {
        return { response: 'fail', message: 'Invalid JSON response' };
    }
}

function guacApiUrl(command) {
    return '/api/guacamole/' + command;
}

async function guacApi(command, body) {
    const url = guacApiUrl(command);
    if (body) {
        return await apiJson(url, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify(body)
        });
    }
    return await apiJson(url);
}

function setStatus(ui, id, msg) {
    ui.setAttrs(id, { text: msg || '' });
}

function formatDate(ts) {
    if (!ts || ts <= 0) return '-';
    const d = new Date(ts * 1000);
    return d.toLocaleString();
}

function esc(s) {
    if (s == null) return '';
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function rebuildConnList(ui) {
    const table = ui.getById('conn-table');
    if (!table) return;
    table.clearRows();
    const tbody = table.tbody;
    for (let i = 0; i < connections.length; i++) {
        const c = connections[i];
        const tr = document.createElement('tr');
        tr.innerHTML =
            '<td>' + esc(c.name) + '</td>' +
            '<td>' + esc(c.protocol) + '</td>' +
            '<td>' + esc(c.hostname) + '</td>' +
            '<td>' + (c.port || '-') + '</td>' +
            '<td>' + esc(c.username || '-') + '</td>' +
            '<td>' + (c.active ? 'Connected' : 'Idle') + '</td>' +
            '<td>' +
            '<button type="button" class="btn-sm" data-edit="' + i + '">Edit</button> ' +
            '<button type="button" class="btn-sm" data-connect="' + i + '">Connect</button> ' +
            '<button type="button" class="btn-sm btn-remove" data-remove="' + i + '">Remove</button>' +
            '</td>';
        tr.querySelector('[data-edit]').addEventListener('click', () => openEdit(ui, i));
        tr.querySelector('[data-connect]').addEventListener('click', () => connectSession(ui, i));
        tr.querySelector('[data-remove]').addEventListener('click', () => removeConnection(ui, i));
        tbody.appendChild(tr);
    }
}

function sessionShortName(s) {
    const host = s.hostname || s.protocol || 'session';
    return s.protocol ? host + ' (' + s.protocol + ')' : host;
}

function sessionTooltip(s) {
    return [
        'Session: ' + (s.id || '-'),
        'Connection: ' + (s.connection_id || '-'),
        'User: ' + (s.username || '-'),
        'Protocol: ' + (s.protocol || '-'),
        'Host: ' + (s.hostname || '-') + ':' + (s.port || '-'),
        'Started: ' + formatDate(s.started)
    ].join('\n');
}

let elSessionsList = null;
let elViewerContainer = null;
let elViewerStatus = null;

function setViewerStatus(text) {
    if (elViewerStatus) elViewerStatus.textContent = text || '';
}

/* Build the Sessions two-pane layout as plain DOM: a left vertical session list
 * and a right pane whose preview fills the tab, with the buttons underneath.
 * kin-ui's flex containers don't give us reliable fill-the-window sizing, so we
 * own this subtree directly. Idempotent. */
function ensureSessionsLayout(ui) {
    const root = ui.getById('sessions-root');
    if (!root || root._built) return;
    root._built = true;
    root.innerHTML =
        '<div class="sess-wrap">' +
          '<div class="sess-list" id="sessions-list"></div>' +
          '<div class="sess-view">' +
            '<div class="sess-canvas" id="viewer-container"></div>' +
            '<div class="sess-toolbar">' +
              '<button type="button" class="btn-sm" data-act="fullscreen">Fullscreen</button>' +
              '<button type="button" class="btn-sm btn-remove" data-act="disconnect">Disconnect</button>' +
              '<button type="button" class="btn-sm" data-act="refresh">Refresh</button>' +
              '<span class="sess-status" id="viewer-status"></span>' +
            '</div>' +
          '</div>' +
        '</div>';

    elSessionsList = root.querySelector('#sessions-list');
    elViewerContainer = root.querySelector('#viewer-container');
    elViewerStatus = root.querySelector('#viewer-status');

    root.querySelector('[data-act="fullscreen"]').addEventListener('click', () => viewer?.fullscreen());
    root.querySelector('[data-act="refresh"]').addEventListener('click', () => loadSessions(ui));
    root.querySelector('[data-act="disconnect"]').addEventListener('click', () => {
        viewer?.disconnect();
        selectedSessionIdx = -1;
        setViewerStatus('');
        rebuildSessionsList(ui);
    });
}

function rebuildSessionsList(ui) {
    ensureSessionsLayout(ui);
    const list = elSessionsList;
    if (!list) return;
    list.replaceChildren();

    if (!sessions.length) {
        const empty = document.createElement('div');
        empty.className = 'session-empty';
        empty.textContent = 'No active sessions. Use Connect on the Connections tab.';
        list.appendChild(empty);
        return;
    }

    for (let i = 0; i < sessions.length; i++) {
        const s = sessions[i];
        const item = document.createElement('div');
        item.className = 'session-item' + (i === selectedSessionIdx ? ' selected' : '');
        item.title = sessionTooltip(s);   /* hover bubble with the full session info */
        const name = document.createElement('span');
        name.className = 'session-name';
        name.textContent = sessionShortName(s);
        const kill = document.createElement('button');
        kill.type = 'button';
        kill.className = 'btn-sm btn-remove session-kill';
        kill.textContent = '×';
        kill.title = 'Disconnect this session';
        kill.addEventListener('click', (ev) => { ev.stopPropagation(); disconnectSession(ui, i); });
        item.appendChild(name);
        item.appendChild(kill);
        item.addEventListener('click', () => selectSession(ui, i));
        list.appendChild(item);
    }
}

function ensureViewer(ui) {
    if (viewer) return viewer;
    ensureSessionsLayout(ui);
    viewer = new GuacViewer(elViewerContainer, (state) => setViewerStatus('Viewer: ' + state));
    return viewer;
}

/** Open the live remote desktop for a session in the right-hand (fill) pane. */
function selectSession(ui, idx) {
    const s = sessions[idx];
    if (!s || !s.connection_id) return;
    selectedSessionIdx = idx;
    rebuildSessionsList(ui);
    setViewerStatus('Viewer: connecting...');
    ensureViewer(ui).connect(s.connection_id);
}

async function loadConnections(ui) {
    setStatus(ui, 'conn-status', 'Loading connections...');
    const data = await guacApi('connections');
    if (data.response !== 'success' || !Array.isArray(data.connections)) {
        setStatus(ui, 'conn-status', data.message || 'Failed to load connections (admin only).');
        return;
    }
    connections = data.connections.map(c => ({
        id: c.id || '',
        name: c.name || '',
        protocol: c.protocol || 'rdp',
        hostname: c.hostname || '',
        port: c.port || 0,
        username: c.username || '',
        password: c.password || '',
        private_key: c.private_key || '',
        domain: c.domain || '',
        security: c.security || 'any',
        color_depth: c.color_depth || '32',
        enable_audio: !!c.enable_audio,
        enable_video: !!c.enable_video,
        enable_printing: !!c.enable_printing,
        enable_file_transfer: !!c.enable_file_transfer,
        enable_wallpaper: !!c.enable_wallpaper,
        enable_theming: !!c.enable_theming,
        enable_font_smoothing: !!c.enable_font_smoothing,
        enable_full_window_drag: !!c.enable_full_window_drag,
        enable_menu_animation: !!c.enable_menu_animation,
        disable_copy: !!c.disable_copy,
        disable_paste: !!c.disable_paste,
        width: c.width || 1024,
        height: c.height || 768,
        dpi: c.dpi || 96,
        active: !!c.active,
        created: c.created || 0,
        last_used: c.last_used || 0
    }));
    rebuildConnList(ui);
    setStatus(ui, 'conn-status', connections.length + ' connection(s).');
}

async function loadSessions(ui) {
    ensureSessionsLayout(ui);
    const data = await guacApi('active');
    if (data.response === 'success' && Array.isArray(data.sessions)) {
        sessions = data.sessions;
    } else {
        sessions = [];
    }
    if (selectedSessionIdx >= sessions.length) selectedSessionIdx = -1;
    rebuildSessionsList(ui);
}

function openAdd(ui) {
    editIndex = -1;
    ui.setAttrs('edit-frame', { title: 'Add connection' });
    setInputValue(ui.getById('edit-name'), '');
    setSelectValue(ui.getById('edit-protocol'), 'rdp');
    setInputValue(ui.getById('edit-hostname'), '');
    setInputValue(ui.getById('edit-port'), '');
    setInputValue(ui.getById('edit-username'), '');
    setInputValue(ui.getById('edit-password'), '');
    setInputValue(ui.getById('edit-domain'), '');
    setSelectValue(ui.getById('edit-security'), 'any');
    setSelectValue(ui.getById('edit-color-depth'), '32');
    setInputValue(ui.getById('edit-width'), '1024');
    setInputValue(ui.getById('edit-height'), '768');
    setInputValue(ui.getById('edit-dpi'), '96');
    setSwitchChecked(ui.getById('edit-enable-audio'), false);
    setSwitchChecked(ui.getById('edit-enable-video'), false);
    setSwitchChecked(ui.getById('edit-enable-printing'), false);
    setSwitchChecked(ui.getById('edit-enable-file-transfer'), false);
    setSwitchChecked(ui.getById('edit-enable-wallpaper'), false);
    setSwitchChecked(ui.getById('edit-enable-theming'), false);
    setSwitchChecked(ui.getById('edit-disable-copy'), false);
    setSwitchChecked(ui.getById('edit-disable-paste'), false);
    setHidden(ui, 'edit-frame', false);
}

function openEdit(ui, idx) {
    editIndex = idx;
    const c = connections[idx];
    ui.setAttrs('edit-frame', { title: 'Edit connection - ' + (c.name || c.hostname) });
    setInputValue(ui.getById('edit-name'), c.name || '');
    setSelectValue(ui.getById('edit-protocol'), c.protocol || 'rdp');
    setInputValue(ui.getById('edit-hostname'), c.hostname || '');
    setInputValue(ui.getById('edit-port'), c.port ? String(c.port) : '');
    setInputValue(ui.getById('edit-username'), c.username || '');
    setInputValue(ui.getById('edit-password'), c.password || '');
    setInputValue(ui.getById('edit-domain'), c.domain || '');
    setSelectValue(ui.getById('edit-security'), c.security || 'any');
    setSelectValue(ui.getById('edit-color-depth'), c.color_depth || '32');
    setInputValue(ui.getById('edit-width'), String(c.width || 1024));
    setInputValue(ui.getById('edit-height'), String(c.height || 768));
    setInputValue(ui.getById('edit-dpi'), String(c.dpi || 96));
    setSwitchChecked(ui.getById('edit-enable-audio'), c.enable_audio);
    setSwitchChecked(ui.getById('edit-enable-video'), c.enable_video);
    setSwitchChecked(ui.getById('edit-enable-printing'), c.enable_printing);
    setSwitchChecked(ui.getById('edit-enable-file-transfer'), c.enable_file_transfer);
    setSwitchChecked(ui.getById('edit-enable-wallpaper'), c.enable_wallpaper);
    setSwitchChecked(ui.getById('edit-enable-theming'), c.enable_theming);
    setSwitchChecked(ui.getById('edit-disable-copy'), c.disable_copy);
    setSwitchChecked(ui.getById('edit-disable-paste'), c.disable_paste);
    setHidden(ui, 'edit-frame', false);
}

function closeEditor(ui) {
    setHidden(ui, 'edit-frame', true);
    editIndex = -1;
}

function getConnectionFromForm(ui) {
    const name = inputValue(ui.getById('edit-name')).trim();
    const protocol = selectValue(ui.getById('edit-protocol'));
    const hostname = inputValue(ui.getById('edit-hostname')).trim();
    const portRaw = inputValue(ui.getById('edit-port')).trim();
    const username = inputValue(ui.getById('edit-username')).trim();
    const password = inputValue(ui.getById('edit-password'));
    const domain = inputValue(ui.getById('edit-domain')).trim();
    const security = selectValue(ui.getById('edit-security'));
    const color_depth = selectValue(ui.getById('edit-color-depth'));
    const width = parseInt(inputValue(ui.getById('edit-width')) || '1024', 10);
    const height = parseInt(inputValue(ui.getById('edit-height')) || '768', 10);
    const dpi = parseInt(inputValue(ui.getById('edit-dpi')) || '96', 10);

    if (!name || !hostname) {
        setStatus(ui, 'conn-status', 'Name and hostname are required.');
        return null;
    }

    let defaults = { vnc: 5900, rdp: 3389, ssh: 22, telnet: 23, kubernetes: 0 };
    const port = parseInt(portRaw, 10);
    const finalPort = Number.isFinite(port) && port > 0 ? port : (defaults[protocol] || 0);

    return {
        action: editIndex >= 0 ? 'update' : 'add',
        ...(editIndex >= 0 ? { id: connections[editIndex].id } : {}),
        name: name,
        protocol: protocol,
        hostname: hostname,
        port: finalPort,
        username: username || undefined,
        password: password || undefined,
        domain: domain || undefined,
        security: security !== 'any' ? security : undefined,
        color_depth: color_depth,
        width: width,
        height: height,
        dpi: dpi,
        enable_audio: switchChecked(ui.getById('edit-enable-audio')),
        enable_video: switchChecked(ui.getById('edit-enable-video')),
        enable_printing: switchChecked(ui.getById('edit-enable-printing')),
        enable_file_transfer: switchChecked(ui.getById('edit-enable-file-transfer')),
        enable_wallpaper: switchChecked(ui.getById('edit-enable-wallpaper')),
        enable_theming: switchChecked(ui.getById('edit-enable-theming')),
        disable_copy: switchChecked(ui.getById('edit-disable-copy')),
        disable_paste: switchChecked(ui.getById('edit-disable-paste'))
    };
}

async function saveConnection(ui) {
    const data = getConnectionFromForm(ui);
    if (!data) return;

    setStatus(ui, 'conn-status', 'Saving...');
    const resp = await guacApi('connections', data);
    if (resp.response === 'success') {
        closeEditor(ui);
        await loadConnections(ui);
        setStatus(ui, 'conn-status', 'Connection saved.');
    } else {
        setStatus(ui, 'conn-status', resp.message || 'Save failed.');
    }
}

function removeConnection(ui, idx) {
    const c = connections[idx];
    connections.splice(idx, 1);
    rebuildConnList(ui);
    setStatus(ui, 'conn-status', connections.length + ' connection(s). Unsaved removal.');

    guacApi('connections', { action: 'delete', id: c.id }).then(resp => {
        if (resp.response === 'success')
            setStatus(ui, 'conn-status', 'Connection removed.');
        else
            setStatus(ui, 'conn-status', resp.message || 'Remove failed.');
    });
}

async function connectSession(ui, idx) {
    const c = connections[idx];
    if (!c || !c.id) {
        setStatus(ui, 'conn-status', 'No connection selected.');
        return;
    }
    setStatus(ui, 'conn-status', 'Connecting to ' + c.name + '...');
    const resp = await guacApi('connection', {
        action: 'connect',
        id: c.id,
        username: c.username || 'user',
        session_id: 'web-' + Date.now()
    });
    if (resp.response === 'success') {
        setStatus(ui, 'conn-status', 'Connected to ' + c.name + '.');
        await loadConnections(ui);
        await loadSessions(ui);
        /* Move the user to the Sessions tab and open the live view. */
        ui.getById('main-tabs')?.kinSet?.('selectedIndex', 1);
        const newIdx = sessions.findIndex(s => s.id === resp.session_id);
        if (newIdx >= 0) selectSession(ui, newIdx);
    } else {
        setStatus(ui, 'conn-status', resp.message || 'Connection failed.');
    }
}

async function disconnectSession(ui, idx) {
    const s = sessions[idx];
    if (!s || !s.id) return;
    const wasSelected = (idx === selectedSessionIdx);
    setViewerStatus('Disconnecting...');
    const resp = await guacApi('connection', {
        action: 'disconnect',
        id: s.id
    });
    if (resp.response === 'success') {
        if (wasSelected) {
            viewer?.disconnect();
            selectedSessionIdx = -1;
            setViewerStatus('');
        }
        await loadSessions(ui);
        await loadConnections(ui);
    } else {
        setViewerStatus(resp.message || 'Disconnect failed.');
    }
}

async function loadProtocols(ui) {
    const data = await guacApi('protocols');
    if (data.response === 'success' && Array.isArray(data.protocols)) {
        ui.setAttrs('protocols-text', {
            text: 'Supported protocols: ' + data.protocols.join(', ')
        });
    } else {
        ui.setAttrs('protocols-text', {
            text: 'Failed to load protocols.'
        });
    }
}

async function loadSettings(ui) {
    const data = await guacApi('settings');
    if (data.response === 'success' && data.settings) {
        const s = data.settings;
        ui.setAttrs('settings-connections', { text: 'Connections: ' + (s.connection_count || 0) });
        ui.setAttrs('settings-sessions', { text: 'Sessions: ' + (s.session_count || 0) });
        ui.setAttrs('settings-max-connections', { text: 'Max connections: ' + (s.max_connections || 256) });
        ui.setAttrs('settings-storage', {
            text: s.storage_path
                ? 'Storage: ' + s.storage_path + (s.persistent ? ' (persistent)' : '')
                : 'Storage: in-memory only'
        });
    } else {
        setStatus(ui, 'conn-status', data.message || 'Failed to load settings.');
    }
}

async function main() {
    let host = document.getElementById('host');
    if (!host) {
        document.body.replaceChildren();
        host = document.createElement('div');
        host.id = 'host';
        document.body.appendChild(host);
    }
    await registerKinUI();
    const res = await fetch(new URL('./ui.json', import.meta.url), { cache: 'no-store' });
    const spec = await res.json();
    const ui = createApp({ root: host, spec });

    ui.setAttrs('app-status', { text: 'Guacamole Remote Desktop Manager — UI loaded' });

    ui.getById('btn-add')?.addEventListener('kin-press', () => openAdd(ui));
    ui.getById('btn-reload')?.addEventListener('kin-press', () => loadConnections(ui));
    ui.getById('btn-edit-save')?.addEventListener('kin-press', () => saveConnection(ui));
    ui.getById('btn-edit-cancel')?.addEventListener('kin-press', () => closeEditor(ui));
    ensureSessionsLayout(ui);   /* Sessions tab layout + its buttons are custom DOM */
    try {
        await Promise.all([
            loadConnections(ui),
            loadSessions(ui),
            loadProtocols(ui),
            loadSettings(ui)
        ]);
    } catch (e) {
        console.error('Guacamole: API calls failed', e);
        setStatus(ui, 'conn-status', 'API unavailable — ' + (e.message || e));
    }
}

main().catch(e => {
    console.error('Guacamole: main() failed', e);
    const host = document.getElementById('host');
    if (host) host.textContent = 'Error: ' + (e.message || e);
});
