import { createApp } from '../kin_ui/kin-ui.js';

let connections = [];
let sessions = [];
let editIndex = -1;

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

function rebuildSessionsList(ui) {
    const table = ui.getById('sessions-table');
    if (!table) return;
    table.clearRows();
    const tbody = table.tbody;
    for (let i = 0; i < sessions.length; i++) {
        const s = sessions[i];
        const tr = document.createElement('tr');
        tr.innerHTML =
            '<td title="' + esc(s.id) + '">' + esc(s.id ? s.id.substring(0, 12) + '...' : '-') + '</td>' +
            '<td>' + esc(s.username || '-') + '</td>' +
            '<td>' + esc(s.connection_id ? s.connection_id.substring(0, 12) + '...' : '-') + '</td>' +
            '<td>' + esc(s.protocol || '-') + '</td>' +
            '<td>' + esc(s.hostname || '-') + '</td>' +
            '<td>' + (s.port || '-') + '</td>' +
            '<td>' + formatDate(s.started) + '</td>' +
            '<td>' +
            '<button type="button" class="btn-sm btn-remove" data-disconnect="' + i + '">Disconnect</button>' +
            '</td>';
        tr.querySelector('[data-disconnect]').addEventListener('click', () => disconnectSession(ui, i));
        tbody.appendChild(tr);
    }
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
    setStatus(ui, 'sessions-status', 'Loading sessions...');
    const data = await guacApi('active');
    if (data.response === 'success' && Array.isArray(data.sessions)) {
        sessions = data.sessions;
        rebuildSessionsList(ui);
        setStatus(ui, 'sessions-status', sessions.length + ' active session(s).');
    } else {
        sessions = [];
        rebuildSessionsList(ui);
        setStatus(ui, 'sessions-status', data.message || 'No session data.');
    }
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
        setStatus(ui, 'conn-status', 'Connected to ' + c.name + ' (session: ' + (resp.session_id || '').substring(0, 12) + '...).');
        await loadConnections(ui);
        await loadSessions(ui);
    } else {
        setStatus(ui, 'conn-status', resp.message || 'Connection failed.');
    }
}

async function disconnectSession(ui, idx) {
    const s = sessions[idx];
    if (!s || !s.id) return;
    setStatus(ui, 'sessions-status', 'Disconnecting...');
    const resp = await guacApi('connection', {
        action: 'disconnect',
        id: s.id
    });
    if (resp.response === 'success') {
        await loadSessions(ui);
        await loadConnections(ui);
        setStatus(ui, 'sessions-status', 'Session disconnected.');
    } else {
        setStatus(ui, 'sessions-status', resp.message || 'Disconnect failed.');
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
    const res = await fetch(new URL('./ui.json', import.meta.url), { cache: 'no-store' });
    const spec = await res.json();
    const ui = createApp({ root: host, spec });

    ui.getById('btn-add')?.addEventListener('kin-press', () => openAdd(ui));
    ui.getById('btn-reload')?.addEventListener('kin-press', () => loadConnections(ui));
    ui.getById('btn-edit-save')?.addEventListener('kin-press', () => saveConnection(ui));
    ui.getById('btn-edit-cancel')?.addEventListener('kin-press', () => closeEditor(ui));
    ui.getById('btn-refresh-sessions')?.addEventListener('kin-press', () => loadSessions(ui));
    await Promise.all([
        loadConnections(ui),
        loadSessions(ui),
        loadProtocols(ui),
        loadSettings(ui)
    ]);
}

main();
