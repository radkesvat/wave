const assert = require('node:assert/strict');
const { execFile, spawn } = require('node:child_process');
const { once } = require('node:events');
const { copyFile, mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile } = require('node:fs/promises');
const { request: httpRequest } = require('node:http');
const { tmpdir } = require('node:os');
const { basename, dirname, join } = require('node:path');
const { createInterface } = require('node:readline');
const { after, before, test } = require('node:test');
const { promisify } = require('node:util');

const sourceBackend = process.env.WAVE_BACKEND;
const run = promisify(execFile);
const timeout = 5000;
const siteDir = 'fixture site';
const assets = {
    'index.html': Buffer.from('<!doctype html><title>Wave fixture</title>'),
    'index.js': Buffer.from('const waveFixture = true;\n'),
    'index.wasm': Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]),
};
let cwd;
let backend;
let database;
let server;
let closed;
let url;
let cookie;

before(async () => {
    assert.ok(sourceBackend, 'Run this test through a Wave backend CTest preset.');
    cwd = await mkdtemp(join(tmpdir(), 'wave-backend-'));
    backend = join(cwd, 'bin', basename(sourceBackend));
    await mkdir(dirname(backend));
    await copyFile(sourceBackend, backend);
    database = join(dirname(backend), 'wave-db.json');
    await writeFile(database, JSON.stringify({ listen: { host: '127.0.0.1', port: 0 } }));
    await mkdir(join(cwd, siteDir));
    for (const [name, contents] of Object.entries(assets)) {
        await writeFile(join(cwd, siteDir, name), contents);
    }

    server = spawn(backend, ['--site-dir', siteDir], {
        cwd,
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stderr = '';
    server.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    closed = once(server, 'close');
    const lines = createInterface({ input: server.stdout });
    try {
        const [line] = await Promise.race([
            once(lines, 'line', { signal: AbortSignal.timeout(timeout) }),
            closed.then(([code, signal]) => {
                throw new Error(`Backend stopped during startup (${signal ?? code}). ${stderr}`);
            }),
        ]);
        const match = /^Listening on (http:\/\/127\.0\.0\.1:[1-9][0-9]*\/)$/.exec(line);
        assert.ok(match, `Unexpected startup output: ${line}`);
        url = new URL(match[1]);
    } finally {
        lines.close();
    }
});

after(async () => {
    try {
        if (server) {
            server.kill();
            await closed;
        }
    } finally {
        if (cwd) await rm(cwd, { recursive: true, force: true });
    }
});

test('startup persists the default listener in new and existing databases beside the executable', async () => {
    const freshBackend = join(cwd, 'fresh', basename(backend));
    const freshDatabase = join(dirname(freshBackend), 'wave-db.json');
    await mkdir(dirname(freshBackend));
    await copyFile(backend, freshBackend);
    // Stop these starts before binding so the smoke suite only opens loopback sockets.
    const start = () => expectStartupFailure([], /Cannot access site directory.*web-release/, freshBackend);
    const defaults = { host: '0.0.0.0', port: 2045 };
    await start();
    assert.deepEqual(JSON.parse(await readFile(freshDatabase, 'utf8')), { listen: defaults, admin: [] });
    const existing = { tls: { enabled: false }, panel_name: 'Test settings' };
    await writeFile(freshDatabase, JSON.stringify(existing));
    await start();
    assert.deepEqual(JSON.parse(await readFile(freshDatabase, 'utf8')), { ...existing, listen: defaults, admin: [] });

    for (const partial of [{ host: '127.0.0.1' }, { port: 8443 }]) {
        await writeFile(freshDatabase, JSON.stringify({ ...existing, listen: partial }));
        await start();
        assert.deepEqual(JSON.parse(await readFile(freshDatabase, 'utf8')), {
            ...existing, listen: { ...defaults, ...partial }, admin: [],
        });
    }
    await assert.rejects(stat(join(cwd, 'wave-db.json')), { code: 'ENOENT' });
    const response = await fetch(new URL('/wave-db.json', url), { signal: AbortSignal.timeout(timeout) });
    assert.equal(response.status, 401);
    await response.arrayBuffer();
});

test('serves public entry assets with caching disabled', async () => {
    for (const path of ['/', '/index.html', '/index.js', '/index.wasm']) {
        const response = await fetch(new URL(path, url), { signal: AbortSignal.timeout(timeout) });
        assert.equal(response.status, 200, path);
        assert.equal(response.headers.get('cache-control'), 'no-store', path);
        assert.deepEqual(Buffer.from(await response.arrayBuffer()), assets[path.slice(1) || 'index.html']);
        if (path === '/index.wasm') {
            assert.equal(response.headers.get('content-type'), 'application/wasm');
        }
    }
});

test('HTTP requires authentication and stores the administrator and JWT session in wave-db.json', async () => {
    const credentials = { username: 'fixture-user', password: 'fake fixture test passphrase' };
    const request = (path, options = {}) =>
        fetch(new URL(path, url), { redirect: 'manual', signal: AbortSignal.timeout(timeout), ...options });
    const post = (action, fields) => request(`/api/auth/${action}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Wave-CSRF': '1', Origin: url.origin },
        body: JSON.stringify(fields),
    });
    const files = (await readdir(cwd, { recursive: true })).sort();
    assert.equal((await request('/panel')).headers.get('location'), '/setup');
    assert.equal((await request('/api/session')).status, 401);
    for (const path of ['/ws', '/ws/overview', '/ws/preview', '/ws/panel']) {
        const status = await new Promise((resolve, reject) => {
            const probe = httpRequest(new URL(path, url), {
                headers: {
                    Origin: url.origin, Connection: 'Upgrade', Upgrade: 'websocket',
                    'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': Buffer.alloc(16).toString('base64'),
                },
                signal: AbortSignal.timeout(timeout),
            });
            probe.on('response', response => {
                response.resume();
                resolve(response.statusCode);
            });
            probe.on('upgrade', (_response, socket) => {
                socket.destroy();
                resolve(101);
            });
            probe.on('error', reject);
            probe.end();
        });
        assert.equal(status, 401, path);
    }
    assert.equal((await post('setup', credentials)).status, 201);
    assert.equal((await post('setup', credentials)).status, 409);
    assert.equal((await request('/panel')).headers.get('location'), '/sign-in');
    const login = await post('sign-in', credentials);
    assert.equal(login.status, 204);
    const [value, ...attributes] = login.headers.get('set-cookie').split('; ');
    assert.match(value, /^wave_session=[\w-]+\.[\w-]+\.[\w-]+$/);
    assert.ok(attributes.includes('HttpOnly') && attributes.includes('SameSite=Strict'));
    assert.ok(!attributes.includes('Secure'));
    cookie = value;
    const headers = { Cookie: cookie };
    assert.equal((await (await request('/api/session', { headers })).json()).username, credentials.username);
    assert.equal((await request('/missing.js', { headers })).status, 404);
    assert.equal((await request('/wave-db.json', { headers })).status, 404);
    const contents = await readFile(database, 'utf8');
    const { admin, auth } = JSON.parse(contents);
    assert.deepEqual(admin, [{ username: credentials.username, password: admin[0].password }]);
    assert.match(admin[0].password, /^\$argon2id\$/);
    assert.ok(!contents.includes(credentials.password));
    assert.equal(auth.sessions.length, 1);
    assert.deepEqual((await readdir(cwd, { recursive: true })).sort(), files);
});

function connectSocket(path) {
    const endpoint = new URL(path, url);
    endpoint.protocol = 'ws:';
    return new WebSocket(endpoint, { headers: { Cookie: cookie, Origin: url.origin } });
}

test('echoes WebSocket text and closes the connection cleanly', async t => {
    const socket = connectSocket('/ws');
    t.after(() => socket.close());
    const signal = AbortSignal.timeout(timeout);
    await once(socket, 'open', { signal });

    for (const text of ['Hello, Wave!', 'Wave 🌊', '']) {
        const received = once(socket, 'message', { signal });
        socket.send(text);
        const [message] = await received;
        assert.equal(message.data, text);
    }

    const closing = once(socket, 'close', { signal });
    socket.close(1000);
    const [event] = await closing;
    assert.equal(event.code, 1000);
    assert.equal(event.wasClean, true);
});

test('sends one deterministic Overview snapshot as WebSocket text on each connection', async t => {
    const expected = {
        cpu_percent: 28,
        memory_used_gib: Math.fround(3.2),
        memory_total_gib: 8,
        uptime_minutes: 103 * 60 + 23,
        download_mib_per_second: [2.1, 3.0, 2.8, 5.5, 4.6, 7.9, 6.8, 10.2, 8.3,
            8.9, 5.7, 6.2, 7.4].map(Math.fround),
    };

    for (let attempt = 0; attempt < 2; attempt++) {
        const socket = connectSocket('/ws/overview');
        t.after(() => socket.close());
        const frames = [];
        socket.addEventListener('message', event => frames.push(event.data));
        const [event] = await once(socket, 'close', { signal: AbortSignal.timeout(timeout) });

        assert.equal(event.code, 1000);
        assert.equal(event.wasClean, true);
        assert.equal(frames.length, 1);
        assert.equal(typeof frames[0], 'string');
        assert.deepEqual(JSON.parse(frames[0]), expected);
    }
});

async function nextPreviewSnapshot(socket, matches = () => true) {
    const signal = AbortSignal.timeout(timeout);
    for (;;) {
        const [message] = await once(socket, 'message', { signal });
        assert.equal(typeof message.data, 'string');
        const frame = JSON.parse(message.data);
        assert.equal(frame.type, 'snapshot');
        if (matches(frame)) return frame;
    }
}

async function connectPreview(t) {
    const socket = connectSocket('/ws/preview');
    t.after(() => socket.close());
    return { socket, initial: (await nextPreviewSnapshot(socket)).data };
}

let requestId = 0;
function requestPreview(socket, action, fields = {}) {
    const id = ++requestId;
    const reply = nextPreviewSnapshot(socket, frame => frame.request_id === id);
    socket.send(JSON.stringify({ action, request_id: id, ...fields }));
    return reply.then(frame => {
        assert.equal(frame.action, action);
        assert.equal(typeof frame.ok, 'boolean');
        assert.equal(typeof frame.feedback, 'string');
        return frame;
    });
}

test('publishes populated preview data and shares user changes and logs across browsers', async t => {
    const { socket, initial } = await connectPreview(t);
    const observer = await connectPreview(t);
    assert.ok(initial.users.length > 20);
    assert.ok(initial.users.some(user => !user.enabled));
    assert.ok(initial.users.some(user => user.upload_rate > 0 && user.download_total > 0));
    assert.equal(initial.server.users_total, initial.users.length);
    assert.equal(initial.server.users_enabled, initial.users.filter(user => user.enabled).length);
    for (const series of ['cpu', 'memory', 'upload', 'download']) {
        assert.ok(initial.server[`${series}_history`].length > 10);
    }
    assert.ok(initial.logs.some(log => log.message.includes('WaterWall is disconnected')));
    assert.ok(initial.topology.nodes.length > 0 && initial.topology.links.length > 0);
    assert.ok(initial.topology.routes.length > 0);
    assert.ok(initial.settings.domain.endsWith('.invalid'));
    assert.equal(initial.bot.connected, false);
    assert.ok(initial.console.output.length > 0);

    const name = 'Mona Preview';
    const otherTab = nextPreviewSnapshot(observer.socket, frame => frame.data.users.some(user => user.name === name));
    const create = await requestPreview(socket, 'create_user', {
        name, notes: 'Demo office', remaining_minutes: 120,
        options: { protocol: 'tcp_websocket', ip_limit: 3 },
    });
    assert.equal(create.ok, true, create.feedback);
    const created = create.data.users.find(user => user.name === name);
    assert.ok(created.configuration.startsWith('wave-preview://'));
    assert.equal(created.options.ip_limit, 3);
    assert.equal(create.data.server.users_total, initial.users.length + 1);
    assert.equal((await otherTab).data.users.find(user => user.name === name).id, created.id);

    const edited = await requestPreview(socket, 'edit_user', { id: created.id, notes: 'New sample notes' });
    assert.equal(edited.ok, true, edited.feedback);
    assert.equal(edited.data.users.find(user => user.id === created.id).notes, 'New sample notes');
    const invalid = await requestPreview(socket, 'edit_user', { id: created.id, name: 'Uncommitted', options: { ip_limit: -2 } });
    assert.equal(invalid.ok, false);
    assert.equal(invalid.data.users.find(user => user.id === created.id).name, name);

    const disabled = await requestPreview(socket, 'set_user_enabled', { id: created.id, enabled: false });
    assert.equal(disabled.ok, true, disabled.feedback);
    assert.equal(disabled.data.users.find(user => user.id === created.id).enabled, false);
    assert.equal(disabled.data.server.users_enabled, initial.server.users_enabled);
    assert.match(disabled.data.logs.at(-1).message, /Disabled sample user Mona Preview/);

    const bot = await requestPreview(socket, 'user_bot_action', { kind: 'configuration', id: created.id });
    assert.equal(bot.ok, true, bot.feedback);
    assert.match(bot.data.logs.at(-1).message, /Simulated bot configuration for Mona Preview/);
    const deleted = await requestPreview(socket, 'delete_user', { id: created.id });
    assert.equal(deleted.ok, true, deleted.feedback);
    assert.equal(deleted.data.users.some(user => user.id === created.id), false);
    assert.equal(deleted.data.server.users_total, initial.users.length);
});

test('validates and shares sample node changes and removes references to deleted nodes', async t => {
    const { socket, initial } = await connectPreview(t);
    const observer = await connectPreview(t);
    const topology = initial.topology;
    for (const name of [undefined, 4, '  ', 'line\nbreak', 'bad\0name', 'x'.repeat(65), ' Local cache ']) {
        const rejected = await requestPreview(socket, 'add_node', { name });
        assert.equal(rejected.ok, false);
        assert.deepEqual(rejected.data.topology, topology);
    }

    const name = 'Sample test relay';
    const observedAdd = nextPreviewSnapshot(observer.socket, frame =>
        frame.data.topology.nodes.some(node => node.label === name));
    const added = await requestPreview(socket, 'add_node', { name: ` ${name} ` });
    assert.equal(added.ok, true, added.feedback);
    const node = added.data.topology.nodes.at(-1);
    assert.equal(node.label, name);
    assert.equal(node.type, 'Node');
    assert.equal(typeof node.id, 'string');
    assert.ok(!topology.nodes.some(item => item.id === node.id || (item.x === node.x && item.y === node.y)));
    assert.ok(node.x >= 0 && node.x <= 1 && node.y >= 0 && node.y <= 1);
    assert.deepEqual(added.data.topology, { ...topology, nodes: [...topology.nodes, node] });
    assert.deepEqual((await observedAdd).data.topology, added.data.topology);

    const id = 'cache';
    assert.ok(topology.links.some(link => link.from === id || link.to === id));
    assert.ok(topology.routes.some(route => route.path.includes(id)));
    const observedDelete = nextPreviewSnapshot(observer.socket, frame =>
        !frame.data.topology.nodes.some(item => item.id === id));
    const deleted = await requestPreview(socket, 'delete_node', { id });
    assert.equal(deleted.ok, true, deleted.feedback);
    const remaining = {
        nodes: added.data.topology.nodes.filter(item => item.id !== id),
        links: topology.links.filter(link => link.from !== id && link.to !== id),
        routes: topology.routes.filter(route => !route.path.includes(id)),
    };
    assert.deepEqual(deleted.data.topology, remaining);
    assert.deepEqual((await observedDelete).data.topology, remaining);
    assert.match(deleted.data.logs.at(-1).message, /Deleted sample node Local cache/);
    for (const invalidId of [id, 1, undefined]) {
        const rejected = await requestPreview(observer.socket, 'delete_node', { id: invalidId });
        assert.equal(rejected.ok, false);
        assert.deepEqual(rejected.data.topology, remaining);
    }

    assert.equal((await requestPreview(socket, 'delete_node', { id: node.id })).ok, true);
    const replacement = await requestPreview(socket, 'add_node', { name });
    assert.equal(replacement.ok, true, replacement.feedback);
    assert.notEqual(replacement.data.topology.nodes.at(-1).id, node.id);
});

test('simulates core lifecycle and panel version changes independently', async t => {
    const { socket, initial } = await connectPreview(t);
    const observer = await connectPreview(t);
    const { core, version: initialVersion, available_panel_versions: available } = initial.server;
    assert.equal(core.state, 'running');
    assert.ok(available.includes(initialVersion));
    assert.deepEqual(core.capabilities, { lifecycle: true, configs: true });
    const target = available.find(version => version !== initialVersion);
    assert.ok(target);

    const change = async (action, fields, ok, state, version = initialVersion) => {
        const reply = await requestPreview(socket, action, fields);
        assert.equal(reply.ok, ok, reply.feedback);
        assert.deepEqual(reply.data.server.core, { ...core, state });
        assert.equal(reply.data.server.version, version);
        assert.deepEqual(reply.data.server.available_panel_versions, available);
        assert.equal(reply.data.server.platform, initial.server.platform);
        return reply;
    };
    await change('start_core', {}, false, 'running');
    for (const version of [initialVersion, 'unknown', 1, undefined]) {
        await change('update_panel', { version }, false, 'running');
    }
    await change('update_core', { version: target }, false, 'running');
    const stopped = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'stopped');
    const stop = await change('stop_core', {}, true, 'stopped');
    assert.equal((await stopped).data.server.core.state, 'stopped');
    for (const series of ['cpu', 'memory', 'upload', 'download']) {
        assert.equal(stop.data.server[`${series}_history`].length, 19, 'Preview charts still report samples.');
    }
    for (const action of ['stop_core', 'pause_core', 'resume_core']) {
        await change(action, {}, false, 'stopped');
    }
    const versionChanged = nextPreviewSnapshot(observer.socket, frame => frame.data.server.version === target);
    await change('update_panel', { version: target }, true, 'stopped', target);
    assert.deepEqual((await versionChanged).data.server.core, { ...core, state: 'stopped' });
    await change('update_panel', { version: target }, false, 'stopped', target);
    await change('start_core', {}, true, 'running', target);
    await change('start_core', {}, false, 'running', target);
});

test('validates in-memory config changes and protects the active config', async t => {
    const { socket, initial } = await connectPreview(t);
    const observer = await connectPreview(t);
    const core = initial.server.core;
    const active = core.configs.find(config => config.active);
    assert.ok(active?.enabled);
    const files = await readdir(cwd);

    const rejected = async (action, fields, snapshot = core) => {
        const reply = await requestPreview(socket, action, fields);
        assert.equal(reply.ok, false);
        assert.deepEqual(reply.data.server.core, snapshot);
    };
    await rejected('set_core_config_enabled', { id: active.id, enabled: false });
    await rejected('remove_core_config', { id: active.id });
    for (const name of [null, '   ', 'invalid\u0000name', 'invalid\u007fname', 'é'.repeat(33), active.name]) {
        await rejected('create_core_config', { name });
    }

    const name = 'Lab preview';
    const broadcast = nextPreviewSnapshot(observer.socket, frame =>
        frame.data.server.core.configs.some(config => config.name === name));
    const created = await requestPreview(socket, 'create_core_config', { name: ` ${name} ` });
    assert.equal(created.ok, true, created.feedback);
    const config = created.data.server.core.configs.at(-1);
    assert.ok(Number.isSafeInteger(config.id) && core.configs.every(item => item.id !== config.id));
    assert.deepEqual(config, { id: config.id, name, enabled: false, active: false });
    assert.deepEqual((await broadcast).data.server.core, created.data.server.core);
    await rejected('create_core_config', { name }, created.data.server.core);
    await rejected('set_core_config_enabled', { id: config.id, enabled: 'true' }, created.data.server.core);

    for (const enabled of [true, false, true]) {
        const reply = await requestPreview(socket, 'set_core_config_enabled', { id: config.id, enabled });
        assert.equal(reply.ok, true, reply.feedback);
        assert.deepEqual(reply.data.server.core, { ...core, configs: [...core.configs, { ...config, enabled }] });
    }
    const removed = await requestPreview(socket, 'remove_core_config', { id: config.id });
    assert.equal(removed.ok, true, removed.feedback);
    assert.deepEqual(removed.data.server.core, core);
    await rejected('remove_core_config', { id: config.id });
    assert.deepEqual(await readdir(cwd), files, 'Preview config actions leave the backend working directory intact.');
});

test('preview actions cannot change or expose the administrator credentials', async t => {
    const { socket, initial } = await connectPreview(t);
    const saved = await readFile(database, 'utf8');
    for (const action of ['setup', 'sign_in', 'sign_out']) {
        const reply = await requestPreview(socket, action, { username: 'legacy-preview', password: 'preview-passphrase' });
        assert.equal(reply.ok, false, action);
        assert.deepEqual(reply.data.settings, initial.settings);
    }
    for (const key of ['admin', 'admin_username', 'password', 'auth']) {
        assert.equal(initial.settings[key], undefined);
        assert.equal(initial[key], undefined);
    }
    assert.equal(await readFile(database, 'utf8'), saved);
});

test('validates TLS fields and partially updates the preview settings', async t => {
    const { socket, initial } = await connectPreview(t);
    for (const settings of [{ port: 4070, multiplex: false }, { domain: '  ' },
        { domain: 'valid.example.invalid', private_key_path: 5 }, { certificate_path: 'a'.repeat(161) }]) {
        const rejected = await requestPreview(socket, 'apply_settings', { settings });
        assert.equal(rejected.ok, false, rejected.feedback);
        assert.deepEqual(rejected.data.settings, initial.settings);
    }
    const private_key_path = 'preview/renewed.key';
    const settings = await requestPreview(socket, 'apply_settings', { settings: { private_key_path } });
    assert.equal(settings.ok, true, settings.feedback);
    assert.deepEqual(settings.data.settings, { ...initial.settings, private_key_path });
    assert.equal((await fetch(new URL('/', url))).status, 200);
});

test('restores preview backups atomically and preserves the active config', async t => {
    const { socket, initial } = await connectPreview(t);
    const observer = await connectPreview(t);
    const files = await readdir(cwd);
    const original = initial.server.core.configs;
    const active = original.find(config => config.active);
    const standby = original.find(config => !config.active);
    const tls = {
        domain: 'restored.example.invalid', certificate_path: 'preview/restore.crt', private_key_path: '',
    };
    const configs = [{ name: standby.name, enabled: false }, { name: 'Restored config', enabled: true }];
    const backup = { format: 'wave-preview-backup', version: 1, settings: tls, configs };
    for (const invalid of [
        { ...backup, format: 'wave-db' }, { ...backup, version: 2 }, { ...backup, settings: { ...tls, domain: '' } },
        { ...backup, settings: { domain: tls.domain } }, { ...backup, configs: null },
        { ...backup, configs: [...configs, { name: active.name, enabled: false }] },
        { ...backup, configs: [...configs, { name: '  Restored config ', enabled: false }] },
        { ...backup, configs: [{ name: 'Invalid flag', enabled: 1 }] },
        { ...backup, configs: Array.from({ length: 64 }, (_, i) => ({ name: `Too many ${i}`, enabled: false })) },
    ]) {
        const reply = await requestPreview(socket, 'restore_preview_backup', { backup: invalid });
        assert.equal(reply.ok, false, reply.feedback);
        assert.deepEqual(reply.data.settings, initial.settings);
        assert.deepEqual(reply.data.server.core.configs, original);
    }

    const broadcast = nextPreviewSnapshot(observer.socket, frame => frame.data.settings.domain === tls.domain);
    const restored = await requestPreview(socket, 'restore_preview_backup', { backup });
    assert.equal(restored.ok, true, restored.feedback);
    const expected = [active, { ...standby, enabled: false }, {
        id: restored.data.server.core.configs.at(-1).id, ...configs[1], active: false,
    }];
    assert.deepEqual(restored.data.server.core.configs, expected);
    assert.deepEqual((await broadcast).data.server.core.configs, expected);
    assert.deepEqual(restored.data.settings, { ...initial.settings, ...tls });
    assert.equal(restored.data.server.version, initial.server.version);

    const full = {
        ...backup,
        configs: Array.from({ length: 63 }, (_, i) => ({
            name: String(i).padStart(2, '0') + '"'.repeat(62), enabled: false,
        })),
    };
    assert.ok(JSON.stringify(full).length > 8192, 'A full backup can exceed the ordinary request limit.');
    const filled = await requestPreview(socket, 'restore_preview_backup', { backup: full });
    assert.equal(filled.ok, true, filled.feedback);
    assert.equal(filled.data.server.core.configs.length, 64);
    assert.deepEqual(filled.data.server.core.configs[0], active);
    const cleared = await requestPreview(socket, 'restore_preview_backup', { backup: { ...backup, configs: [] } });
    assert.equal(cleared.ok, true, cleared.feedback);
    assert.deepEqual(cleared.data.server.core.configs, [active]);
    assert.deepEqual(await readdir(cwd), files);
});

test('simulates preview bot actions and console entirely in memory', async t => {
    const { socket } = await connectPreview(t);
    const connected = await requestPreview(socket, 'bot_connect', { token: 'demo-placeholder' });
    assert.equal(connected.ok, true, connected.feedback);
    assert.equal(connected.data.bot.connected, true);
    assert.equal(connected.data.bot.token, undefined);
    const bot = await requestPreview(socket, 'bot_action', { kind: 'broadcast', message: 'Sample notice' });
    assert.equal(bot.ok, true, bot.feedback);
    assert.equal(bot.data.bot.last_message, 'Sample notice');

    const input = 'echo executed > preview-execution-probe.txt';
    const console = await requestPreview(socket, 'console_command', { input });
    assert.equal(console.ok, true, console.feedback);
    assert.equal(console.data.console.history.at(-1), input);
    assert.equal(console.data.console.output.at(-2), `$ ${input}`);
    assert.match(console.data.console.output.at(-1), /preview input/);
    await assert.rejects(stat(join(cwd, 'preview-execution-probe.txt')), { code: 'ENOENT' });

    const stopped = await requestPreview(socket, 'stop_console');
    assert.equal(stopped.data.console.running, false);
    assert.equal((await requestPreview(socket, 'console_command', { input: 'help' })).ok, false);
    assert.equal((await requestPreview(socket, 'start_console')).data.console.running, true);
    assert.deepEqual((await requestPreview(socket, 'clear_logs')).data.logs, []);
});

test('merges independent bot settings and overrides updates from stale clients in either order', async t => {
    for (const order of [['settings', 'overrides'], ['overrides', 'settings']]) {
        await t.test(`${order[0]} first`, async clients => {
            const settingsClient = await connectPreview(clients);
            const overridesClient = await connectPreview(clients);
            const { bot } = settingsClient.initial;
            assert.deepEqual(overridesClient.initial.bot, bot);
            const notifications = !bot.notifications;
            const port = bot.overrides.port === 443 ? 8443 : 443;
            const edits = {
                settings: {
                    socket: settingsClient.socket,
                    fields: {
                        notifications, allow_status: bot.allow_status, allow_config: bot.allow_config,
                        notification_rules: bot.notification_rules, permissions: bot.permissions,
                    },
                },
                overrides: {
                    socket: overridesClient.socket,
                    fields: { overrides: { ...overridesClient.initial.bot.overrides, port } },
                },
            };
            const [first, second] = order.map(name => edits[name]);
            const save = edit => requestPreview(edit.socket, 'apply_bot_settings', { settings: edit.fields });
            const firstReply = await save(first);
            assert.equal(firstReply.ok, true, firstReply.feedback);
            const observed = nextPreviewSnapshot(first.socket, frame => frame.request_id === undefined &&
                frame.data.bot.notifications === notifications && frame.data.bot.overrides.port === port);
            const secondReply = await save(second);
            assert.equal(secondReply.ok, true, secondReply.feedback);
            const expected = { ...bot, notifications, overrides: { ...bot.overrides, port } };
            assert.deepEqual(secondReply.data.bot, expected);
            assert.deepEqual((await observed).data.bot, expected);
        });
    }
});

async function expectStartupFailure(args, diagnostic, executable = backend) {
    await assert.rejects(run(executable, args, { cwd, timeout }), error => {
        assert.ok(Number.isInteger(error.code) && error.code !== 0, error);
        assert.match(error.stderr, diagnostic);
        assert.equal(error.stdout, '');
        return true;
    });
}

test('rejects invalid command-line options', async () => {
    await expectStartupFailure(['--unknown'], /Unknown option.*--unknown/);
    await expectStartupFailure(['--site-dir', '--check-tls'], /Missing value.*--site-dir/);
    await expectStartupFailure(['--host', '127.0.0.1'], /Unknown option.*--host/);
    await expectStartupFailure(['--port', '2045'], /Unknown option.*--port/);
});

async function isolatedBackend(name, settings = {}) {
    const directory = join(cwd, name);
    const executable = join(directory, basename(backend));
    const db = join(directory, 'wave-db.json');
    await mkdir(directory);
    await copyFile(backend, executable);
    await writeFile(db, JSON.stringify(settings), { mode: 0o600 });
    return { executable, db };
}

test('rejects invalid database listener values before binding', async () => {
    const { executable, db } = await isolatedBackend('invalid-listener');
    for (const port of ['2045', -1, 65536, 2 ** 40, 2045.5]) {
        await writeFile(db, JSON.stringify({ listen: { port } }));
        await expectStartupFailure(['--site-dir', siteDir], /listen\.port.*integer from 0 to 65535/, executable);
    }
    for (const host of ['', 'host name', '127.0.0.1\0', null]) {
        await writeFile(db, JSON.stringify({ listen: { host } }));
        await expectStartupFailure(['--site-dir', siteDir], /listen\.host.*nonempty address/, executable);
    }
    await writeFile(db, JSON.stringify({ listen: '127.0.0.1:2045' }));
    await expectStartupFailure(['--site-dir', siteDir], /listen in wave-db.json must be a JSON object/, executable);
});

test('rejects malformed manually configured Argon2id hashes before binding', async t => {
    const fixtureHash = `$argon2id$v=19$m=65536,t=3,p=4$${'A'.repeat(22)}$${'A'.repeat(43)}`;
    const { executable, db } = await isolatedBackend('invalid-admin');
    for (const [label, password] of [
        ['leading zero in the memory cost', fixtureHash.replace('$m=', '$m=0')],
        ['noncanonical base64 padding bits', fixtureHash.slice(0, -1) + 'B'],
        ['plaintext password', 'fixture plaintext password'],
    ]) {
        await t.test(label, async () => {
            await writeFile(db, JSON.stringify({
                listen: { host: '127.0.0.1', port: 0 }, admin: [{ username: 'fixture-operator', password }],
            }));
            await expectStartupFailure(['--site-dir', siteDir],
                /administrator in wave-db.json requires a username and an Argon2id password hash/, executable);
        });
    }
});

test('fails before binding when the site or an asset is missing', async () => {
    const { executable } = await isolatedBackend('missing-assets');
    await expectStartupFailure([], /Cannot access site directory.*web-release/, executable);
    const incomplete = join(cwd, 'incomplete');
    await mkdir(incomplete);
    await writeFile(join(incomplete, 'index.html'), assets['index.html']);
    await writeFile(join(incomplete, 'index.js'), assets['index.js']);
    await expectStartupFailure(
        ['--site-dir', incomplete],
        /Missing required site asset.*index\.wasm/,
        executable,
    );
});

test('rejects concurrent access to the database or a port already in use', async () => {
    await expectStartupFailure(['--site-dir', siteDir], /Another Wave process is using wave-db.json/);
    const { executable } = await isolatedBackend('port-in-use', {
        listen: { host: url.hostname, port: Number(url.port) },
    });
    await expectStartupFailure(
        ['--site-dir', siteDir],
        new RegExp(`Could not bind to 127\\.0\\.0\\.1:${url.port}`),
        executable,
    );
});
