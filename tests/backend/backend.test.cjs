const assert = require('node:assert/strict');
const { execFile, spawn } = require('node:child_process');
const { once } = require('node:events');
const { mkdir, mkdtemp, rm, stat, writeFile } = require('node:fs/promises');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const { createInterface } = require('node:readline');
const { after, before, test } = require('node:test');
const { promisify } = require('node:util');

const backend = process.env.WAVE_BACKEND;
const run = promisify(execFile);
const timeout = 5000;
const siteDir = 'fixture site';
const assets = {
    'index.html': Buffer.from('<!doctype html><title>Wave fixture</title>'),
    'index.js': Buffer.from('const waveFixture = true;\n'),
    'index.wasm': Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]),
};
let cwd;
let server;
let closed;
let url;

before(async () => {
    assert.ok(backend, 'Run this test through a Wave backend CTest preset.');
    cwd = await mkdtemp(join(tmpdir(), 'wave-backend-'));
    await mkdir(join(cwd, siteDir));
    for (const [name, contents] of Object.entries(assets)) {
        await writeFile(join(cwd, siteDir, name), contents);
    }

    server = spawn(backend, ['--site-dir', siteDir, '--port', '0'], {
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

test('serves the site assets and returns 404 for a missing file', async () => {
    for (const path of ['/', '/index.html', '/index.js', '/index.wasm']) {
        const response = await fetch(new URL(path, url), { signal: AbortSignal.timeout(timeout) });
        assert.equal(response.status, 200, path);
        assert.deepEqual(Buffer.from(await response.arrayBuffer()), assets[path.slice(1) || 'index.html']);
        if (path === '/index.wasm') {
            assert.equal(response.headers.get('content-type'), 'application/wasm');
        }
    }

    const missing = await fetch(new URL('/missing.js', url), { signal: AbortSignal.timeout(timeout) });
    assert.equal(missing.status, 404);
    await missing.arrayBuffer();
});

test('echoes WebSocket text and closes the connection cleanly', async t => {
    const wsUrl = new URL('/ws', url);
    wsUrl.protocol = 'ws:';
    const socket = new WebSocket(wsUrl);
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
    const wsUrl = new URL('/ws/overview', url);
    wsUrl.protocol = 'ws:';
    const expected = {
        cpu_percent: 28,
        memory_used_gib: Math.fround(3.2),
        memory_total_gib: 8,
        uptime_minutes: 103 * 60 + 23,
        download_mib_per_second: [2.1, 3.0, 2.8, 5.5, 4.6, 7.9, 6.8, 10.2, 8.3,
            8.9, 5.7, 6.2, 7.4].map(Math.fround),
    };

    for (let attempt = 0; attempt < 2; attempt++) {
        const socket = new WebSocket(wsUrl);
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
    const wsUrl = new URL('/ws/preview', url);
    wsUrl.protocol = 'ws:';
    const socket = new WebSocket(wsUrl);
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

test('simulates preview setup, settings, bot actions, and console entirely in memory', async t => {
    const { socket } = await connectPreview(t);
    const setupCredentials = { username: 'preview-admin', password: 'demo-password' };
    const setup = await requestPreview(socket, 'setup', {
        name: 'Wave QA', domain: 'qa.example.invalid', ...setupCredentials,
    });
    assert.equal(setup.ok, true, setup.feedback);
    assert.equal(setup.data.server.name, 'Wave QA');
    assert.equal(setup.data.settings.admin_username, 'preview-admin');
    assert.equal(setup.data.settings.password, undefined);
    const signIn = await requestPreview(socket, 'sign_in', setupCredentials);
    assert.equal(signIn.ok, true, signIn.feedback);
    const invalid = await requestPreview(socket, 'sign_in', { ...setupCredentials, password: 'wrong' });
    assert.equal(invalid.ok, false);

    const settings = await requestPreview(socket, 'apply_settings', { settings: { port: 4070, multiplex: false } });
    assert.equal(settings.ok, true, settings.feedback);
    assert.equal(settings.data.settings.port, 4070);
    assert.equal(settings.data.settings.multiplex, false);
    assert.equal((await fetch(new URL('/', url))).status, 200);

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

async function expectStartupFailure(args, diagnostic) {
    await assert.rejects(run(backend, args, { cwd, timeout }), error => {
        assert.ok(Number.isInteger(error.code) && error.code !== 0, error);
        assert.match(error.stderr, diagnostic);
        assert.equal(error.stdout, '');
        return true;
    });
}

test('rejects invalid command-line options', async () => {
    await expectStartupFailure(['--unknown'], /Unknown option.*--unknown/);
    await expectStartupFailure(['--site-dir', '--port', '0'], /Missing value.*--site-dir/);
    await expectStartupFailure(['--port', '2030extra'], /Invalid port.*2030extra/);
    await expectStartupFailure(['--port', '65536'], /Invalid port.*65536/);
});

test('fails before binding when the site or an asset is missing', async () => {
    await expectStartupFailure(['--port', '0'], /Cannot access site directory.*web-release/);
    const incomplete = join(cwd, 'incomplete');
    await mkdir(incomplete);
    await writeFile(join(incomplete, 'index.html'), assets['index.html']);
    await writeFile(join(incomplete, 'index.js'), assets['index.js']);
    await expectStartupFailure(
        ['--site-dir', incomplete, '--port', '0'],
        /Missing required site asset.*index\.wasm/,
    );
});

test('fails to bind to the port of an already running backend', async () => {
    await expectStartupFailure(
        ['--site-dir', siteDir, '--host', url.hostname, '--port', url.port],
        new RegExp(`Could not bind to 127\\.0\\.0\\.1:${url.port}`),
    );
});
