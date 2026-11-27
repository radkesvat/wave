const assert = require('node:assert/strict');
const { execFile, spawn } = require('node:child_process');
const { once } = require('node:events');
const { appendFile, chmod, copyFile, mkdir, mkdtemp, readFile, readdir, rm, rmdir, stat, unlink, writeFile } =
    require('node:fs/promises');
const { request: httpRequest } = require('node:http');
const { createServer } = require('node:net');
const { hostname, tmpdir } = require('node:os');
const { basename, delimiter, dirname, join } = require('node:path');
const { createInterface } = require('node:readline');
const { after, before, test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { isDeepStrictEqual, promisify } = require('node:util');
const { authFixture, rotatingLogPath, seedCore } = require('./auth_fixture.cjs');

const sourceBackend = process.env.WAVE_BACKEND;
const run = promisify(execFile);
const timeout = 5000;
const historyLimit = 19;
const metricHistories = [
    ['cpu_percent', 'cpu_history', 100],
    ['memory_used_gib', 'memory_history', Infinity],
    ['upload_rate', 'upload_history', Infinity],
    ['download_rate', 'download_history', Infinity],
];
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
let countersAtStartup;
let coreFixture;

async function readNetworkCounters() {
    const rows = await readFile('/proc/net/dev', 'utf8');
    const metrics = new Map();
    for (const row of rows.trimEnd().split('\n').slice(2)) {
        const separator = row.lastIndexOf(':');
        const name = row.slice(0, separator).trim();
        if (name === 'lo') continue;
        const columns = row.slice(separator + 1).trim().split(/\s+/);
        metrics.set(name, [BigInt(columns[0]), BigInt(columns[8])]);
    }
    return metrics;
}

before(async () => {
    assert.ok(sourceBackend, 'Run this test through a Wave backend CTest preset.');
    cwd = await mkdtemp(join(tmpdir(), 'wave-backend-'));
    backend = join(cwd, 'bin with spaces', basename(sourceBackend));
    await mkdir(dirname(backend));
    await copyFile(sourceBackend, backend);
    coreFixture = await seedCore(backend, { executable: process.platform === 'linux' });
    database = join(dirname(backend), 'wave-db.json');
    await writeFile(database, JSON.stringify({
        listen: { host: '127.0.0.1', port: 0 }, waterwall_install_attempted: true,
    }));
    await mkdir(join(cwd, siteDir));
    for (const [name, contents] of Object.entries(assets)) {
        await writeFile(join(cwd, siteDir, name), contents);
    }

    if (process.platform === 'linux') countersAtStartup = await readNetworkCounters();
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

test('initializes a private database beside the executable and preserves existing listener settings', async () => {
    const fresh = join(cwd, 'fresh', basename(backend));
    const db = join(dirname(fresh), 'wave-db.json');
    await mkdir(dirname(fresh));
    await copyFile(backend, fresh);
    // Validate startup settings without binding the default public port.
    const missingSite = () => expectStartupFailure([], /Cannot access site directory/, fresh);
    await missingSite();
    assert.deepEqual(JSON.parse(await readFile(db, 'utf8')), {
        listen: { host: '0.0.0.0', port: 2045 }, admin: [],
    });
    assert.equal((await stat(db)).mode & 0o777, 0o600);
    const existing = { listen: { host: '127.0.0.1' }, panel_name: 'Fixture' };
    await writeFile(db, JSON.stringify(existing));
    await missingSite();
    assert.deepEqual(JSON.parse(await readFile(db, 'utf8')), {
        ...existing, listen: { ...existing.listen, port: 2045 }, admin: [],
    });
    await writeFile(db, JSON.stringify({ listen: { port: -1 } }));
    await expectStartupFailure(['--site-dir', siteDir], /listen\.port.*integer/, fresh);
    await writeFile(db, JSON.stringify({
        listen: { host: '127.0.0.1', port: 0 },
        admin: [{ username: 'fixture-user', password: 'fixture plaintext password' }],
    }));
    await expectStartupFailure(['--site-dir', siteDir], /requires a username and an Argon2id password hash/, fresh);
    await assert.rejects(stat(join(cwd, 'wave-db.json')), { code: 'ENOENT' });
});

test('serves uncached entry assets and protects HTTP data with an administrator session', async () => {
    const credentials = { username: 'fixture-user', password: 'fake fixture test passphrase 9Jw4c' };
    const request = (path, options = {}) =>
        fetch(new URL(path, url), { redirect: 'manual', signal: AbortSignal.timeout(timeout), ...options });
    const post = (action, origin = url.origin) => request(`/api/auth/${action}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Wave-CSRF': '1', Origin: origin },
        body: JSON.stringify(credentials),
    });
    const files = (await readdir(cwd, { recursive: true })).sort();
    assert.equal((await request('/panel')).headers.get('location'), '/setup');
    for (const [path, asset] of [['/setup', 'index.html'], ['/index.js', 'index.js'], ['/index.wasm', 'index.wasm']]) {
        const response = await request(path);
        assert.equal(response.status, 200);
        assert.equal(response.headers.get('cache-control'), 'no-store');
        assert.deepEqual(Buffer.from(await response.arrayBuffer()), assets[asset]);
        if (asset === 'index.wasm') assert.equal(response.headers.get('content-type'), 'application/wasm');
    }
    assert.equal((await request('/api/session')).status, 401);
    const upgrade = await new Promise((resolve, reject) => {
        const probe = httpRequest(new URL('/ws/preview', url), {
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
    assert.equal(upgrade, 401);

    assert.equal((await post('setup', 'https://other.example.test')).status, 403);
    assert.equal((await post('setup')).status, 201);
    assert.equal((await post('setup')).status, 409);
    assert.equal((await request('/panel')).headers.get('location'), '/sign-in');
    const login = await post('sign-in');
    assert.equal(login.status, 204);
    const [value, ...flags] = login.headers.get('set-cookie').split('; ');
    assert.match(value, /^wave_session=[\w-]+\.[\w-]+\.[\w-]+$/);
    assert.ok(flags.includes('HttpOnly') && flags.includes('SameSite=Strict') && !flags.includes('Secure'));
    cookie = value;
    const headers = { Cookie: cookie };
    assert.equal((await (await request('/api/session', { headers })).json()).username, credentials.username);
    assert.equal((await request('/wave-db.json', { headers })).status, 404);
    const contents = await readFile(database, 'utf8');
    const { admin, auth } = JSON.parse(contents);
    assert.equal(admin[0].username, credentials.username);
    assert.match(admin[0].password, /^\$argon2id\$/);
    assert.ok(!contents.includes(credentials.password));
    assert.equal(auth.sessions.length, 1);
    assert.deepEqual((await readdir(cwd, { recursive: true })).sort(), files);
});

function connectSocket(path, base = url, sessionCookie = cookie) {
    const endpoint = new URL(path, base);
    const origin = endpoint.origin;
    endpoint.protocol = endpoint.protocol === 'https:' ? 'wss:' : 'ws:';
    return new WebSocket(endpoint, { headers: { Cookie: sessionCookie, Origin: origin } });
}

function assertMetric(value, maximum = Infinity) {
    assert.ok(value === null || (Number.isFinite(value) && value >= 0 && value <= maximum),
        'Expected a nonnegative metric or null, got ' + value);
}

async function assertLinuxMemoryAndUptime(snapshot) {
    const [memory, uptime] = await Promise.all([
        readFile('/proc/meminfo', 'utf8'),
        readFile('/proc/uptime', 'utf8'),
    ]);
    const totalKib = Number(/^MemTotal:\s+(\d+) kB$/m.exec(memory)[1]);
    assert.equal(snapshot.memory_total_gib, totalKib / 2 ** 20);
    assert.ok(Number.isFinite(snapshot.memory_used_gib));
    assertMetric(snapshot.memory_used_gib, snapshot.memory_total_gib);
    const minutes = Math.floor(parseFloat(uptime) / 60);
    assert.ok(Number.isInteger(snapshot.uptime_minutes) &&
        snapshot.uptime_minutes >= minutes - 1 && snapshot.uptime_minutes <= minutes);
}

async function readOverview(t) {
    const socket = connectSocket('/ws/overview');
    t.after(() => socket.close());
    const frames = [];
    socket.addEventListener('message', event => frames.push(event.data));
    const [event] = await once(socket, 'close', { signal: AbortSignal.timeout(timeout) });

    assert.equal(event.code, 1000);
    assert.equal(event.wasClean, true);
    assert.equal(frames.length, 1);
    assert.equal(typeof frames[0], 'string');
    return JSON.parse(frames[0]);
}

async function nextPreviewSnapshot(socket, matches = () => true, wait = timeout) {
    const signal = AbortSignal.timeout(wait);
    for (;;) {
        const [message] = await once(socket, 'message', { signal });
        assert.equal(typeof message.data, 'string');
        const frame = JSON.parse(message.data);
        assert.equal(frame.type, 'snapshot');
        if (matches(frame)) return frame;
    }
}

async function connectPreview(t, base, sessionCookie) {
    const socket = connectSocket('/ws/preview', base, sessionCookie);
    t.after(() => socket.close());
    return { socket, initial: (await nextPreviewSnapshot(socket)).data };
}

async function fixtureSession(fixture) {
    const credentials = { username: 'fixture-admin', password: 'fixture core test passphrase 4Kq7v' };
    const post = action => fetch(new URL('/api/auth/' + action, fixture.origin), {
        method: 'POST',
        headers: { Origin: fixture.origin, 'X-Wave-CSRF': '1', 'Content-Type': 'application/json' },
        body: JSON.stringify(credentials),
        signal: AbortSignal.timeout(timeout),
    });
    assert.equal((await post('setup')).status, 201);
    const login = await post('sign-in');
    assert.equal(login.status, 204);
    return login.headers.get('set-cookie').split(';', 1)[0];
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

function assertHostHistory(server) {
    for (const [key, historyKey, limit] of metricHistories) {
        const history = server[historyKey];
        assert.ok(Array.isArray(history) && history.length <= historyLimit, historyKey);
        for (const reading of history) assertMetric(reading, limit);
        assertMetric(server[key], limit);
        assert.equal(server[key], history.at(-1) ?? null, key);
    }
}

async function assertLinuxNetworkTotals(server) {
    const after = await readNetworkCounters();
    assert.ok(Number.isFinite(server.download_total));
    assert.ok(Number.isFinite(server.upload_total));
    assertMetric(server.download_total);
    assertMetric(server.upload_total);
    if (after.size !== countersAtStartup.size) return;
    for (const [name, start] of countersAtStartup) {
        const end = after.get(name);
        // A replaced, new, or reset interface prevents comparing cumulative counters across the interval.
        if (!end || end.some((value, i) => value < start[i])) return;
    }
    const totalGib = (map, index) =>
        Number([...map.values()].reduce((sum, counters) => sum + counters[index], 0n)) / 2 ** 30;
    for (const [index, total] of [server.download_total, server.upload_total].entries()) {
        const minimum = totalGib(countersAtStartup, index);
        const maximum = totalGib(after, index);
        const rounding = maximum * Number.EPSILON * after.size;
        assert.ok(total >= minimum - rounding && total <= maximum + rounding,
            'Host traffic totals fall between independent Linux reads of the same interface counters.');
    }
}

test('streams measured Linux metrics twice per second and shares history with Overview',
    { skip: process.platform !== 'linux' }, async t => {
        // Measure history accumulation without an open telemetry socket.
        await delay(1500);
        const overview = await readOverview(t);
        assert.ok(overview.download_mib_per_second.length >= 2 && overview.download_mib_per_second.length <= 13);
        await assertLinuxMemoryAndUptime(overview);

        const { socket, initial } = await connectPreview(t);
        const { server: original } = initial;
        assert.equal(original.memory_total_gib, overview.memory_total_gib);
        assert.equal(original.name, hostname());
        assertHostHistory(original);
        await assertLinuxNetworkTotals(original);
        assert.ok(original.download_history.some((_, index) =>
            isDeepStrictEqual(original.download_history.slice(index, index + overview.download_mib_per_second.length),
                overview.download_mib_per_second)));
        assert.equal(initial.admin, undefined);
        assert.equal(initial.auth, undefined);

        const first = (await nextPreviewSnapshot(socket, undefined, 2000)).data;
        const start = performance.now();
        let latest;
        for (let sample = 0; sample < 4; sample++) {
            latest = (await nextPreviewSnapshot(socket, undefined, 2000)).data;
        }
        const elapsed = performance.now() - start;
        assert.ok(elapsed >= 1000 && elapsed < 3500, 'Four intervals took ' + elapsed + ' ms.');
        const { server } = latest;
        assertHostHistory(server);
        assert.ok(server.memory_history.length >= Math.min(historyLimit, first.server.memory_history.length + 4));
        assert.ok(Number.isFinite(server.cpu_percent) || Number.isFinite(first.server.cpu_percent));
        assert.ok(Number.isFinite(server.download_rate) && Number.isFinite(server.upload_rate));
        await assertLinuxNetworkTotals(server);
    });

async function waitForFile(path) {
    const deadline = performance.now() + timeout;
    while (performance.now() < deadline) {
        try {
            const contents = await readFile(path, 'utf8');
            if (contents.length) return contents;
        } catch (error) {
            if (error.code !== 'ENOENT') throw error;
        }
        await delay(25);
    }
    throw new Error('The fixture did not write its expected file.');
}

async function createCoreConfig(socket, name, enabled = false) {
    const created = await requestPreview(socket, 'create_core_config', { name });
    assert.equal(created.ok, true, created.feedback);
    const config = created.data.server.core.configs.find(config => config.name === name.trim());
    assert.ok(config);
    assert.equal(config.enabled, false);
    if (!enabled) return config;
    const changed = await requestPreview(socket, 'set_core_config_enabled', { id: config.id, enabled: true });
    assert.equal(changed.ok, true, changed.feedback);
    return changed.data.server.core.configs.find(entry => entry.id === config.id);
}

function configPath(name) {
    return join(coreFixture.configDir, name, 'config.json');
}

function panelLogs(frame, level, message = frame.feedback) {
    return frame.data.logs.filter(row => row.source === 'panel' && row.level === level && row.message === message);
}

test('starts, restarts, and reaps WaterWall with the selected configs supplied through stdin',
    { skip: process.platform !== 'linux' }, async t => {
        const { socket, initial } = await connectPreview(t);
        const observer = await connectPreview(t);
        assert.equal(initial.server.version, '1.6.0');
        assert.deepEqual(initial.server.available_panel_versions, ['1.6.0']);
        const rejectedStart = nextPreviewSnapshot(observer.socket, frame =>
            panelLogs(frame, 'error', 'Enable at least one config before starting WaterWall.').length > 0);
        const empty = await requestPreview(socket, 'start_core');
        assert.equal(empty.ok, false);
        assert.match(empty.feedback, /enable.*config/i);
        assert.equal(empty.feedback_in_logs_only, true);
        assert.equal(panelLogs(empty, 'error').length, 1);
        assert.equal(panelLogs(await rejectedStart, 'error', empty.feedback).length, 1);
        const stoppedRestart = await requestPreview(socket, 'restart_core');
        assert.equal(stoppedRestart.ok, false);
        assert.match(stoppedRestart.feedback, /must be running/i);
        assert.equal(stoppedRestart.feedback_in_logs_only, true);
        assert.equal(panelLogs(stoppedRestart, 'error').length, 1);
        const refreshed = await requestPreview(socket, 'refresh');
        assert.equal(refreshed.ok, true);
        assert.equal(refreshed.feedback_in_logs_only, true);
        assert.equal(panelLogs(refreshed, 'info').length, 1);

        const active = await createCoreConfig(socket, 'Main config', true);
        const inactive = await createCoreConfig(socket, 'Skip disabled');
        for (const [path, error] of [[coreFixture.program, /installed/], [configPath(active.name), /unsafe/i]]) {
            const mode = (await stat(path)).mode & 0o777;
            await chmod(path, mode | 0o022);
            try {
                const unsafe = await requestPreview(socket, 'start_core');
                assert.equal(unsafe.ok, false);
                assert.match(unsafe.feedback, error);
                await assert.rejects(stat(coreFixture.processFile), { code: 'ENOENT' });
            } finally {
                await chmod(path, mode);
            }
        }

        const settings = {
            log: { path: 'test-logs/', core: { file: 'core-fixture.log', loglevel: 'WARN', console: false } },
            misc: { workers: 2, 'ram-profile': 'server', 'tcp-stack-receive-budget': 0 },
            dns: { rotate: false, flags: [], 'server-failover': {} },
        };
        const saved = await requestPreview(socket, 'apply_core_settings', { settings });
        assert.equal(saved.ok, true, saved.feedback);
        assert.deepEqual(saved.data.core_settings, settings);
        assert.equal(saved.feedback_in_logs_only, true);
        assert.equal(saved.data.logs.filter(row => row.source === 'panel' && row.message === saved.feedback).length, 1);

        const taggedLog = (level, message) =>
            `2026-01-02 12:34:56.789 ${level.padEnd(5)} ${message}\n`;
        const coreLog = join(dirname(backend), rotatingLogPath(settings, 'core'));
        await mkdir(dirname(coreLog), { recursive: true });
        await writeFile(coreLog, taggedLog('WARN', 'Earlier file content'));
        const expectedLogs = [
            { source: 'core', ts: '12:34:56', level: 'warning', message: 'Fixture core log' },
            { source: 'dns', ts: '12:34:56', level: 'error', message: 'Fixture DNS log' },
            { source: 'internal', ts: '12:34:56', level: 'debug', message: 'Fixture internal log' },
            { source: 'network', ts: '12:34:56', level: 'info', message: 'Fixture network log' },
        ];
        await seedCore(backend, { executable: true, observeShutdown: true,
            logs: expectedLogs.map(({ source, message }, i) => ({
                source, text: taggedLog(['WARN', 'ERROR', 'DEBUG', 'INFO'][i], message),
            })),
        });
        t.after(() => seedCore(backend, { executable: true }));
        const captured = nextPreviewSnapshot(observer.socket, frame => expectedLogs.every(expected =>
            frame.data.logs.some(row => row.message === expected.message)));
        const running = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'running');
        assert.equal((await requestPreview(socket, 'start_core')).ok, true);
        const started = await running;
        assert.deepEqual(started.data.server.core.configs, [{ ...active, active: true }, inactive]);
        assert.equal(panelLogs(started, 'success', 'WaterWall started.').length, 1);
        const diagnostics = await requestPreview(socket, 'diagnostics');
        assert.equal(diagnostics.ok, true, diagnostics.feedback);
        assert.match(diagnostics.feedback, /WaterWall is running/);
        assert.equal(diagnostics.data.logs.at(-1).message, diagnostics.feedback);
        assert.equal(panelLogs(diagnostics, 'success', 'WaterWall started.').length, 1);
        const child = JSON.parse(await waitForFile(coreFixture.processFile));
        assert.deepEqual(child.args, ['--config:stdin']);
        assert.equal(child.cwd, dirname(backend));
        assert.deepEqual(child.settings, { ...settings, configs: ['configs/Main config/config.json'] });
        await assert.rejects(stat(join(dirname(backend), 'core.json')), { code: 'ENOENT' });
        await assert.rejects(stat(join(dirname(backend), 'waterwall_config')), { code: 'ENOENT' });
        assert.doesNotThrow(() => process.kill(child.pid, 0));
        assert.equal((await requestPreview(socket, 'set_core_config_enabled', {
            id: active.id, enabled: false,
        })).ok, false);
        assert.equal((await requestPreview(socket, 'remove_core_config', { id: active.id })).ok, false);

        const logs = (await captured).data.logs.filter(row => row.source !== 'panel');
        assert.deepEqual(logs.sort((a, b) => a.source.localeCompare(b.source)), expectedLogs);
        const contents = await readFile(coreLog, 'utf8');
        const cleared = await requestPreview(socket, 'clear_logs');
        assert.equal(cleared.ok, true, cleared.feedback);
        assert.equal(cleared.feedback_in_logs_only, undefined);
        assert.deepEqual(cleared.data.logs, []);
        assert.equal(await readFile(coreLog, 'utf8'), contents);
        const message = 'Fixture log after clearing';
        const appended = nextPreviewSnapshot(observer.socket, frame => frame.data.logs.some(row => row.message === message));
        await appendFile(coreLog, taggedLog('WARN', message));
        assert.deepEqual((await appended).data.logs, [{ ...expectedLogs[0], message }]);

        const mode = (await stat(coreFixture.program)).mode & 0o777;
        try {
            await chmod(coreFixture.program, mode & ~0o111);
            const unavailable = await requestPreview(socket, 'restart_core');
            assert.equal(unavailable.ok, false);
            assert.match(unavailable.feedback, /must be installed/i);
            assert.equal(unavailable.data.server.core.state, 'running');
            await assert.rejects(stat(coreFixture.terminatedFile), { code: 'ENOENT' });
        } finally {
            await chmod(coreFixture.program, mode);
        }
        const nextSettings = { ...settings, misc: { ...settings.misc, workers: 3 } };
        assert.equal((await requestPreview(socket, 'apply_core_settings', { settings: nextSettings })).ok, true);
        assert.equal((await requestPreview(socket, 'set_core_config_enabled', { id: inactive.id, enabled: true })).ok, true);
        const enabled = { ...inactive, enabled: true };
        await unlink(coreFixture.processFile);
        const starting = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'starting');
        const resumed = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'running' &&
            frame.data.server.core.configs.some(config => config.id === enabled.id && config.active));
        const restart = await requestPreview(socket, 'restart_core');
        assert.equal(restart.ok, true, restart.feedback);
        assert.equal(restart.data.server.core.state, 'stopping');
        assert.deepEqual(restart.data.server.core.configs, [{ ...active, active: true }, enabled]);
        const duplicate = await requestPreview(socket, 'restart_core');
        assert.equal(duplicate.ok, false);
        assert.match(duplicate.feedback, /must be running/i);
        assert.equal(await waitForFile(coreFixture.terminatedFile), String(child.pid));
        await starting;
        const restarted = await resumed;
        assert.deepEqual(restarted.data.server.core.configs,
            [{ ...active, active: true }, { ...enabled, active: true }]);
        assert.equal(panelLogs(restarted, 'success', 'WaterWall started.').length, 1);
        assert.equal(panelLogs(restarted, 'success', 'WaterWall stopped.').length, 0);
        const replacement = JSON.parse(await waitForFile(coreFixture.processFile));
        assert.notEqual(replacement.pid, child.pid);
        assert.throws(() => process.kill(child.pid, 0), { code: 'ESRCH' });
        assert.deepEqual(replacement.settings,
            { ...nextSettings, configs: ['configs/Main config/config.json', 'configs/Skip disabled/config.json'] });

        const stopped = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'stopped');
        assert.equal((await requestPreview(socket, 'stop_core')).ok, true);
        const reaped = await stopped;
        assert.deepEqual(reaped.data.server.core.configs, [active, enabled]);
        assert.equal(panelLogs(reaped, 'success', 'WaterWall stopped.').length, 1);
        assert.throws(() => process.kill(replacement.pid, 0), { code: 'ESRCH' });

        await writeFile(coreFixture.program, '#!/bin/sh\nexit 7\n');
        const exited = nextPreviewSnapshot(observer.socket, frame =>
            frame.data.server.core.error?.includes('WaterWall exited.'));
        assert.equal((await requestPreview(socket, 'start_core')).ok, true);
        const failed = await exited;
        const error = failed.data.server.core.error;
        assert.equal(panelLogs(failed, 'error', error).length, 1);
        assert.equal(panelLogs(failed, 'success', 'WaterWall stopped.').length, 1);
        const afterExit = await nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.error === error);
        assert.equal(panelLogs(afterExit, 'error', error).length, 1);

        for (const { id } of [active, enabled]) {
            assert.equal((await requestPreview(socket, 'remove_core_config', { id })).ok, true);
        }
        await unlink(coreFixture.processFile);
        await unlink(coreFixture.terminatedFile);
    });

const supportedInstaller = process.platform === 'linux' && ['x64', 'arm64'].includes(process.arch);
test('attempts first-run installation once and lets an authenticated administrator retry a missing core',
    { skip: !supportedInstaller }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, prepareCore: false });
        const binDir = join(dirname(fixture.backend), 'waterwall_bin');
        const configDir = join(dirname(fixture.backend), 'configs');
        const tools = join(fixture.cwd, 'fixture tools');
        const calls = join(fixture.cwd, 'curl calls');
        const released = join(fixture.cwd, 'release curl');
        await mkdir(binDir);
        await mkdir(tools);
        await writeFile(join(tools, 'curl'), [
            '#!/bin/sh',
            'printf "%s\n" "$*" >> "$WAVE_CORE_TEST_CALLS"',
            'attempt=0',
            'while [ ! -e "$WAVE_CORE_TEST_RELEASE" ] && [ "$attempt" -lt 200 ]; do',
            '    sleep 0.05',
            '    attempt=$((attempt+1))',
            'done',
            'printf "Fixture is not a verified archive\n"',
            '',
        ].join('\n'), { mode: 0o700 });
        const env = {
            PATH: tools + delimiter + process.env.PATH,
            WAVE_CORE_TEST_CALLS: calls,
            WAVE_CORE_TEST_RELEASE: released,
        };
        const invoked = async () => (await readFile(calls, 'utf8')).trim().split('\n');
        const failedInstall = socket => nextPreviewSnapshot(socket, ({ data: { server: { core } } }) =>
            core.state === 'missing' && core.error?.includes('SHA-256'));
        const initial = JSON.parse(await readFile(fixture.database, 'utf8'));
        await writeFile(fixture.database, JSON.stringify({ ...initial, core_configs: [{ name: 'Incomplete' }] }));
        await assert.rejects(fixture.start(env), /invalid or duplicate config/);
        assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).waterwall_install_attempted, undefined);
        await assert.rejects(stat(calls), { code: 'ENOENT' });
        await fixture.stop();
        await writeFile(fixture.database, JSON.stringify(initial));
        await fixture.start(env);
        const firstCall = await waitForFile(calls);
        assert.match(firstCall, /WaterWall\/releases\/download\/v1\.6\.0\/Waterwall-linux-gcc-/);
        assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).waterwall_install_attempted, true);
        assert.deepEqual(await readdir(configDir), []);

        const session = await fixtureSession(fixture);
        const first = await connectPreview(t, fixture.origin, session);
        assert.equal(first.initial.server.core.state, 'installing');
        assert.equal((await requestPreview(first.socket, 'install_core')).ok, false);
        const firstFailure = failedInstall(first.socket);
        await writeFile(released, '');
        const firstFailed = await firstFailure;
        assert.equal(panelLogs(firstFailed, 'error', firstFailed.data.server.core.error).length, 1);
        assert.equal((await invoked()).length, 1);
        assert.deepEqual(await readdir(binDir), []);

        await fixture.stop();
        await unlink(released);
        await rmdir(binDir);
        await fixture.start(env);
        const restarted = await connectPreview(t, fixture.origin, session);
        assert.equal(restarted.initial.server.core.state, 'missing');
        assert.deepEqual(await readdir(binDir), []);
        await delay(250);
        assert.equal((await invoked()).length, 1, 'Deleting the directory does not reset the first-run attempt.');

        const retry = await requestPreview(restarted.socket, 'install_core');
        assert.equal(retry.ok, true, retry.feedback);
        assert.equal(retry.data.server.core.state, 'installing');
        const secondFailure = failedInstall(restarted.socket);
        await writeFile(released, '');
        const secondFailed = await secondFailure;
        assert.equal(panelLogs(secondFailed, 'error', secondFailed.data.server.core.error).length, 1);
        assert.equal(panelLogs(secondFailed, 'success', 'WaterWall installed.').length, 0);
        assert.equal((await invoked()).length, 2);
        assert.deepEqual(await readdir(binDir), []);

        const installed = nextPreviewSnapshot(restarted.socket, frame => frame.data.server.core.state === 'stopped');
        const staged = await seedCore(fixture.backend, { executable: true });
        assert.equal(panelLogs(await installed, 'success', 'WaterWall installed.').length, 0);
        const deleted = nextPreviewSnapshot(restarted.socket, frame => frame.data.server.core.state === 'missing');
        await unlink(staged.program);
        await deleted;
        assert.equal((await invoked()).length, 2, 'Deleting the executable leaves installation up to the administrator.');
    });

test('preserves config files and keeps outdated core settings until the administrator reviews them', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const canLaunch = process.platform === 'linux';
    const core = await seedCore(fixture.backend, { executable: canLaunch });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket, initial } = await connectPreview(t, fixture.origin, session);
    const original = await readFile(fixture.database, 'utf8');
    assert.deepEqual(initial.server.core.configs, []);
    for (const name of ['..', '../escape', 'nested/profile', 'nested\\profile', 'invalid\u0000name']) {
        assert.equal((await requestPreview(socket, 'create_core_config', { name })).ok, false);
    }
    // This fits on input, but expanding all 11 resolver flag names exceeds 16 KiB.
    const oversized = { log: { path: 'x'.repeat(16 * 1024 - 64) }, dns: { flags: 2047 } };
    assert.ok(Buffer.byteLength(JSON.stringify(oversized)) <= 16 * 1024);
    for (const [settings, field] of [
        [{ configs: ['other.json'] }, /\bconfigs\b/],
        [{ log: { level: 'warning' } }, /\blog\.level\b/],
        [{ log: { core: { console: 'false' } } }, /\blog\.core\.console\b/],
        [{ misc: { mtu: 67 } }, /\bmisc\.mtu\b/],
        [{ dns: { 'server-failover': { 'retry-chance': 65536 } } }, /\bdns\.server-failover\.retry-chance\b/],
        [oversized, /16 KiB/],
    ]) {
        const rejected = await requestPreview(socket, 'apply_core_settings', { settings });
        assert.equal(rejected.ok, false);
        assert.match(rejected.feedback, field);
        assert.deepEqual(rejected.data.core_settings, initial.core_settings);
    }
    assert.equal(await readFile(fixture.database, 'utf8'), original);
    assert.deepEqual(await readdir(core.configDir), []);

    const name = 'Lab profile';
    const created = await createCoreConfig(socket, ` ${name} `, true);
    const directory = join(core.configDir, name);
    const file = join(directory, 'config.json');
    const skeleton = { name, 'config-version': 1, 'core-minimum-version': 0, nodes: [] };
    assert.deepEqual(JSON.parse(await readFile(file, 'utf8')), skeleton);
    assert.equal((await requestPreview(socket, 'create_core_config', { name })).ok, false);

    const stored = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.deepEqual(stored.core_configs, [{ id: created.id, name, enabled: true }]);
    const legacy = JSON.stringify({
        ...stored,
        core_settings: {
            log: { core: { loglevel: 'debug' }, level: 'debug' },
            misc: { 'ram-profile': 6, mtu: 67 },
            dns: {
                'domain-strategy': 2, flags: { tcp: true, edns: true },
                servers: ['192.0.2.53', '2001:db8::53'],
            },
        },
    }, null, 2);
    const settings = {
        log: { core: { loglevel: 'DEBUG' } },
        misc: { 'ram-profile': 'server' },
        dns: { 'domain-strategy': 'prefer-ipv6', flags: ['use-vc', 'edns'], servers: '192.0.2.53,2001:db8::53' },
    };
    await fixture.stop();
    await writeFile(fixture.database, legacy);
    await fixture.start();
    const review = await connectPreview(t, fixture.origin, session);
    assert.deepEqual(review.initial.core_settings, settings);
    assert.match(review.initial.core_settings_notice, /\blog\.level\b/);
    assert.match(review.initial.core_settings_notice, /\bmisc\.mtu\b/);
    assert.equal(await readFile(fixture.database, 'utf8'), legacy);
    if (canLaunch) {
        const blocked = await requestPreview(review.socket, 'start_core');
        assert.equal(blocked.ok, false);
        assert.match(blocked.feedback, /review|save/i);
        await assert.rejects(stat(core.processFile), { code: 'ENOENT' });
        assert.equal(await readFile(fixture.database, 'utf8'), legacy);
    }

    const approved = await requestPreview(review.socket, 'apply_core_settings', {
        settings: review.initial.core_settings,
    });
    assert.equal(approved.ok, true, approved.feedback);
    assert.ok(!approved.data.core_settings_notice);
    const persisted = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.deepEqual(persisted.core_settings, settings);
    assert.deepEqual(persisted.core_configs, stored.core_configs);

    await fixture.stop();
    await fixture.start();
    const restarted = await connectPreview(t, fixture.origin, session);
    assert.deepEqual(restarted.initial.core_settings, settings);
    assert.ok(!restarted.initial.core_settings_notice);
    assert.deepEqual(restarted.initial.server.core.configs, [created]);
    if (canLaunch) {
        const running = nextPreviewSnapshot(restarted.socket, frame => frame.data.server.core.state === 'running');
        assert.equal((await requestPreview(restarted.socket, 'start_core')).ok, true);
        await running;
        const child = JSON.parse(await waitForFile(core.processFile));
        assert.deepEqual(child.settings, { ...settings, configs: [`configs/${name}/config.json`] });
        const stopped = nextPreviewSnapshot(restarted.socket, frame => frame.data.server.core.state === 'stopped');
        assert.equal((await requestPreview(restarted.socket, 'stop_core')).ok, true);
        await stopped;
    }
    const notes = join(directory, 'notes.txt');
    await writeFile(notes, 'Fixture notes survive removal.');
    assert.equal((await requestPreview(restarted.socket, 'remove_core_config', { id: created.id })).ok, true);
    assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')).core_configs, []);
    assert.deepEqual(await readdir(directory), ['notes.txt']);
    await createCoreConfig(restarted.socket, name);
    assert.deepEqual(JSON.parse(await readFile(file, 'utf8')), skeleton);
    assert.equal(await readFile(notes, 'utf8'), 'Fixture notes survive removal.');
});

test('validates configured WaterWall links and preserves supported aliases and peer references', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await connectPreview(t, fixture.origin, session);
    const profile = await createCoreConfig(socket, 'Graph links');
    const { config, revision } = (await requestPreview(socket, 'get_core_config', { id: profile.id })).result;
    const save = (nodes, expectedRevision) =>
        requestPreview(socket, 'save_core_config', { id: profile.id, config: { ...config, nodes }, revision: expectedRevision });
    const sink = { name: 'sink', type: 'BlackHole' };
    const nodes = [
        sink,
        { name: 'router', type: 'SniffRouter', settings: { routes: [{ domain: 'example.test', next: false, target: 'sink' }] } },
        { name: 'primary', type: 'Bridge', settings: { pair: 'peer' } },
        { name: 'peer', type: 'Bridge', settings: { pair: 'primary' } },
        { name: 'duplex', type: 'HalfDuplexClient', settings: { upload: 'sink', download: 'sink' } },
        { name: 'tls', type: 'TlsServer', settings: { 'fallback-node': 'sink', fallback: 'ignored-alias' } },
        { name: 'ip', type: 'IpManipulator', settings: { 'smuggle-fin': false, 'real-fin-upstream-node': 'unused' } },
    ];
    const saved = await save(nodes, revision);
    assert.equal(saved.ok, true, saved.feedback);
    assert.deepEqual(saved.result.config.nodes, nodes);
    for (const bad of [
        { name: 'duplex', type: 'HalfDuplexClient', next: 'sink', settings: { upload: 'sink', download: 'sink' } },
        { name: 'splitter', type: 'PacketSplitStream', settings: { up: 'sink', down: 'sink' } },
        { name: 'router', type: 'SniffRouter', settings: { routes: [{ domain: 'example.test', next: 'missing', target: 'sink' }] } },
        { name: 'primary', type: 'Bridge', settings: { pair: 'primary' } },
        { name: 'tls', type: 'TlsServer', settings: { fallback: 'missing' } },
        { name: 'socks', type: 'Socks5Server', settings: { 'auth-client-node-name': 'sink' } },
        { name: 'ip', type: 'IpManipulator', next: 'sink',
            settings: { 'smuggle-fin': true, 'real-fin-upstream-node': 'missing' } },
    ]) {
        const rejected = await save([...nodes.filter(node => node.name !== bad.name), bad], saved.result.revision);
        assert.equal(rejected.ok, false, JSON.stringify(bad));
    }
    const reopened = await requestPreview(socket, 'get_core_config', { id: profile.id });
    assert.equal(reopened.result.revision, saved.result.revision);
    assert.deepEqual(reopened.result.config.nodes, nodes);
});

test('persists config graphs and isolates AuthenticationServer users by node', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const core = await seedCore(fixture.backend, { executable: true });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await connectPreview(t, fixture.origin, session);
    const profile = await createCoreConfig(socket, 'Office network');
    const dir = join(core.configDir, profile.name);
    const file = join(dir, 'config.json');
    const open = sock => requestPreview(sock, 'get_core_config', { id: profile.id });
    const save = (config, revision) => requestPreview(socket, 'save_core_config', { id: profile.id, config, revision });
    const users = (sock, node, action = 'get_config_users', fields = {}) =>
        requestPreview(sock, action, { config_id: profile.id, node, ...fields });

    const { config: skeleton, revision } = (await open(socket)).result;
    for (const [contents, expected] of [
        ['{"name":"Fixture","nodes":[],"metadata":{"author":"A","author":"B"}}', /duplicate.*key/i],
        ['{"name":"Fixture","nodes":[],"author":"a\\u0000b"}', /NUL/i],
        [JSON.stringify(skeleton) + '\0', /NUL/i],
    ]) {
        await writeFile(file, contents);
        const rejected = await open(socket);
        assert.equal(rejected.ok, false);
        assert.match(rejected.feedback, expected);
        assert.equal(rejected.feedback_in_logs_only, undefined);
        assert.equal(panelLogs(rejected, 'error').length, 0);
        assert.equal(await readFile(file, 'utf8'), contents);
    }
    await writeFile(file, JSON.stringify(skeleton, null, 2) + '\n');
    assert.equal((await open(socket)).result.revision, revision);
    const gateway = { name: 'listener', type: 'TcpListener', next: 'staff', 'panel-settings': { x: 28, y: 57 } };
    for (const nodes of [
        [gateway],
        [{ ...gateway, type: 'NotAWaterWallNode', next: 'listener' }],
        [{ ...gateway, next: 'staff' }, { name: 'staff', type: 'Router', next: 'listener' }],
        [{ name: 'bad path', type: 'AuthenticationServer', settings: { 'db-path': '../../wave-db.json' } }],
    ]) {
        assert.equal((await save({ ...skeleton, nodes }, revision)).ok, false);
        assert.deepEqual(await readdir(dir), ['config.json']);
    }
    const invalidText = await save({ ...skeleton, author: 'a\0b' }, revision);
    assert.equal(invalidText.ok, false);
    assert.match(invalidText.feedback, /NUL/i);

    const graph = { ...skeleton, author: 'Fixture', nodes: [gateway,
        { name: 'staff', type: 'AuthenticationServer' }, { name: 'guests', type: 'AuthenticationServer' }] };
    const saved = await save(graph, revision);
    assert.equal(saved.ok, true, saved.feedback);
    assert.deepEqual(saved.data.server.core.configs[0].auth_nodes, ['staff', 'guests']);
    const savedGraph = saved.result.config;
    assert.deepEqual(savedGraph.nodes[0], gateway);
    assert.equal(savedGraph.author, 'Fixture');
    assert.deepEqual(JSON.parse(await readFile(file, 'utf8')), savedGraph);
    const databases = savedGraph.nodes.slice(1).map(node => {
        const name = node['panel-settings']['auth-db'];
        assert.match(name, /^auth-[0-9a-f]{32}\.json$/);
        assert.equal(node.settings['db-path'], `configs/${profile.name}/${name}`);
        return join(dir, name);
    });
    assert.notEqual(...databases);
    for (const database of databases) {
        assert.deepEqual(JSON.parse(await readFile(database, 'utf8')), { users: [] });
        assert.equal((await stat(database)).mode & 0o777, 0o600);
    }
    assert.match((await save(skeleton, revision)).feedback, /reload/i);

    const password = 'fixture-tunnel-credential';
    const user = {
        id: '9007199254740993', name: 'Alex', email: 'alex@example.test', notes: 'Fixture notes', password,
        enabled: true,
        gid: '18446744073709551615', 'record-stat-interval-ms': '250', 'wireguard-allowed-ips': '192.0.2.17/24',
        limit: {
            traffic: { up: '9007199254740993', down: '2048', total: '0' },
            bandwidth: { up: '1024', down: '512' }, ips: '3', devices: '4',
            'connections-in': '5', 'connections-out': '6',
        },
        time: { 'expire-at-ms': '1924992000000', 'expire-after-first-usage-ms': '3600000' },
    };
    const created = await users(socket, 'staff', 'create_config_user', { user });
    assert.equal(created.ok, true, created.feedback);
    assert.equal(created.feedback_in_logs_only, true);
    assert.equal(panelLogs(created, 'success').length, 1);
    assert.equal(created.result.total, 1);
    const [record] = created.result.users;
    const { password: _, ...expected } = user;
    const createdAt = record.time['created-at-ms'];
    assert.match(createdAt, /^[1-9][0-9]+$/);
    assert.deepEqual(record, {
        ...expected, 'wireguard-allowed-ips': '192.0.2.0/24',
        time: { ...expected.time, 'created-at-ms': createdAt },
    });
    assert.doesNotMatch(JSON.stringify(created), new RegExp(password));
    const stored = JSON.parse(await readFile(databases[0], 'utf8'));
    assert.deepEqual(stored.users, [{ ...record, password }]);
    assert.deepEqual((await users(socket, 'guests')).result.users, []);

    const renamed = structuredClone(savedGraph);
    renamed.nodes[0].next = 'directory';
    renamed.nodes[1].name = 'directory';
    const rename = await save(renamed, saved.result.revision);
    assert.equal(rename.ok, true, rename.feedback);
    assert.equal(rename.result.config.nodes[1].settings['db-path'], savedGraph.nodes[1].settings['db-path']);
    assert.equal((await users(socket, 'staff')).ok, false);
    // Model WaterWall-managed history and a legacy expiration before applying a partial user edit.
    const onDisk = stored.users[0];
    onDisk.limit.traffic.u = onDisk.limit.traffic.up;
    onDisk.time['first-usage-at-ms'] = '1920000000000';
    onDisk['expires-at-ms'] = '1924992001000';
    onDisk.stats = { traffic: { u: '18446744073709551615', d: '956' }, devices: '2' };
    onDisk.extension = { preserved: true };
    const existing = JSON.stringify(stored);
    await writeFile(databases[0], existing);
    const create = user => users(socket, 'directory', 'create_config_user', { user });
    const edit = user => users(socket, 'directory', 'edit_config_user', { user_id: record.id, user });
    for (const [fields, feedback] of [
        [{ gid: '18446744073709551616' }, /gid/i],
        [{ 'record-stat-interval-ms': '2147483648' }, /record-stat-interval-ms/i],
        [{ id: '2' }, /read-only/i],
        [{ time: { 'first-usage-at-ms': '0' } }, /read-only/i],
        [{ stats: { traffic: { up: '0' } } }, /read-only/i],
    ]) {
        const rejected = await edit(fields);
        assert.equal(rejected.ok, false);
        assert.match(rejected.feedback, feedback);
    }
    const otherPassword = 'fixture-distinct-credential';
    for (const id of ['0', '0007', '18446744073709551616', user.id]) {
        const rejected = await create({ id, password: otherPassword });
        assert.equal(rejected.ok, false, id);
        assert.match(rejected.feedback, /id/i);
    }
    const overlap = await create({ password: otherPassword, 'wireguard-allowed-ips': '192.0.2.1/32' });
    assert.equal(overlap.ok, false);
    assert.match(overlap.feedback, /overlap/i);
    assert.equal(overlap.feedback_in_logs_only, true);
    assert.equal(panelLogs(overlap, 'error').length, 1);
    assert.equal(await readFile(databases[0], 'utf8'), existing);

    const notes = 'x'.repeat(9 * 1024);
    const edited = await edit({
        name: 'Jordan', notes, enabled: false, 'wireguard-allowed-ips': '',
        limit: { traffic: { up: null, down: '0' } }, time: { 'expire-at-ms': null },
    });
    assert.equal(edited.ok, true, edited.feedback);
    assert.equal(edited.feedback_in_logs_only, true);
    const updated = {
        ...record, name: 'Jordan', notes, enabled: false, 'wireguard-allowed-ips': '',
        limit: { ...record.limit, traffic: { total: record.limit.traffic.total, down: '0' } },
        time: {
            'created-at-ms': createdAt, 'first-usage-at-ms': onDisk.time['first-usage-at-ms'],
            'expire-after-first-usage-ms': record.time['expire-after-first-usage-ms'],
        },
        stats: { traffic: { up: onDisk.stats.traffic.u, down: onDisk.stats.traffic.d }, devices: '2' },
    };
    assert.deepEqual(edited.result.users, [updated]);
    const [merged] = JSON.parse(await readFile(databases[0], 'utf8')).users;
    assert.equal(merged.password, password);
    assert.equal(merged['expires-at-ms'], undefined);
    assert.deepEqual(merged.limit.traffic, updated.limit.traffic);
    assert.deepEqual(merged.extension, onDisk.extension);
    assert.deepEqual(merged.stats, onDisk.stats);

    assert.equal((await requestPreview(socket, 'set_core_config_enabled', { id: profile.id, enabled: true })).ok, true);
    const running = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'running');
    assert.equal((await requestPreview(socket, 'start_core')).ok, true);
    assert.equal((await open(socket)).result.editable, false);
    assert.equal((await save(renamed, rename.result.revision)).ok, false);
    assert.equal((await users(socket, 'directory')).ok, false);
    assert.equal((await users(socket, 'directory', 'delete_config_user', { user_id: record.id })).ok, false);
    await running;
    const stopped = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'stopped');
    assert.equal((await requestPreview(socket, 'stop_core')).ok, true);
    await stopped;

    await fixture.stop();
    await fixture.start();
    const restarted = await connectPreview(t, fixture.origin, session);
    assert.deepEqual(restarted.initial.server.core.configs[0].auth_nodes, ['directory', 'guests']);
    assert.deepEqual((await open(restarted.socket)).result.config, rename.result.config);
    assert.deepEqual((await users(restarted.socket, 'directory')).result.users, [updated]);
    const rotated = await users(restarted.socket, 'directory', 'edit_config_user', {
        user_id: record.id, user: { password: otherPassword },
    });
    assert.equal(rotated.ok, true, rotated.feedback);
    assert.deepEqual(rotated.result.users, [updated]);
    assert.equal(JSON.parse(await readFile(databases[0], 'utf8')).users[0].password, otherPassword);
    const deleted = await users(restarted.socket, 'directory', 'delete_config_user', { user_id: record.id });
    assert.equal(deleted.ok, true, deleted.feedback);
    assert.equal(deleted.feedback_in_logs_only, true);
    assert.deepEqual(deleted.result.users, []);
    assert.deepEqual(JSON.parse(await readFile(databases[0], 'utf8')), { users: [] });
});

function liveWaterWallFixture({ configName, cursor, user }) {
    const { appendFileSync, readFileSync, writeFileSync } = require('node:fs');
    const { createServer } = require('node:net');
    const parent = process.ppid;
    const settings = JSON.parse(readFileSync(0, 'utf8'));
    const config = settings.configs.map(path => JSON.parse(readFileSync(path, 'utf8')))
        .find(config => config.name === configName);
    const listener = config.nodes.find(node => node.name === 'api listener');
    let requests = 0;
    const upstream = createServer(connection => {
        let input = Buffer.alloc(0);
        let complete = false;
        connection.on('error', () => {});
        connection.on('data', chunk => {
            if (complete) return;
            input = Buffer.concat([input, chunk]);
            if (input.length < 4 || input.length < 4 + input.readUInt32BE(0)) return;
            complete = true;
            const request = JSON.parse(input.subarray(4).toString('utf8'));
            const body = JSON.parse(request.body);
            appendFileSync('live-fixture-requests.jsonl', JSON.stringify({ ...request, body }) + '\n');
            ++requests;
            const stale = body.cursor === cursor;
            const result = stale ? { error: { code: 'stale_cursor' } } : {
                total_users: '2', users: [user], next_cursor: cursor, edit_token: 'fixture-private-edit-token',
            };
            const payload = Buffer.from(JSON.stringify({
                id: request.id, error: null, result: {
                    code: stale ? 1 : 0, content_type: 'application/json', body_encoding: 'utf8',
                    body: JSON.stringify(result),
                },
            }));
            const frame = Buffer.alloc(4 + payload.length);
            frame.writeUInt32BE(payload.length);
            payload.copy(frame, 4);
            connection.write(frame.subarray(0, 2));
            setImmediate(() => {
                connection.end(frame.subarray(2));
                if (requests === 2) upstream.close(() => writeFileSync('live-fixture-closed', 'closed'));
            });
        });
    });
    upstream.listen(listener.settings['port-range'][0], '127.0.0.1',
        () => writeFileSync('live-fixture-ready', 'ready'));
    setInterval(() => {
        try {
            process.kill(parent, 0);
        } catch {
            process.exit(0);
        }
    }, 100);
}

test('lists active AuthenticationServer users through the scoped WaterWall loopback API',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false });
        const core = await seedCore(fixture.backend);
        const cursor = '12'.repeat(32);
        const token = '34'.repeat(32);
        const configName = 'Live logical profile';
        const user = {
            id: '9007199254740993', gid: '7', enabled: true, name_preview: 'Live fixture account',
            name_preview_state: 'complete', usage: { u: '18446744073709551615', d: '2048' },
        };
        const options = JSON.stringify({ configName, cursor, user });
        await writeFile(core.program, `#!/usr/bin/env node\n(${liveWaterWallFixture})(${options});\n`, { mode: 0o700 });
        const reserved = createServer();
        reserved.listen(0, '127.0.0.1');
        await once(reserved, 'listening');
        const { port } = reserved.address();
        await new Promise(resolve => reserved.close(resolve));
        const fixtureDir = dirname(fixture.backend);
        const requestsFile = join(fixtureDir, 'live-fixture-requests.jsonl');
        const requests = async () => (await readFile(requestsFile, 'utf8')).trim().split('\n').map(JSON.parse);
        await fixture.start();
        const session = await fixtureSession(fixture);
        const { socket } = await connectPreview(t, fixture.origin, session);
        const profile = await createCoreConfig(socket, 'Live profile files', true);
        const inactive = await createCoreConfig(socket, 'Inactive profile');
        const node = 'staff';
        const saveGraph = async (id, config) => {
            const { revision } = (await requestPreview(socket, 'get_core_config', { id })).result;
            const saved = await requestPreview(socket, 'save_core_config', { id, config, revision });
            assert.equal(saved.ok, true, saved.feedback);
            return saved;
        };
        const configured = await saveGraph(profile.id, { name: configName, nodes: [
            { name: node, type: 'AuthenticationServer' },
            { name: 'api listener', type: 'TcpListener', next: 'api', settings: {
                address: '127.0.0.1', 'port-range': [port, port],
            } },
            { name: 'api', type: 'APIServer', settings: { clients: [
                { name: 'wrong operation', token: '56'.repeat(32), allow: [{ config: configName, node, operations: ['get-user'] }] },
                { name: 'fixture reader', token, allow: [{ config: configName, node, operations: ['list-users'] }] },
            ] } },
        ] });
        await saveGraph(inactive.id, { name: inactive.name, nodes: [{ name: node, type: 'AuthenticationServer' }] });
        assert.equal(configured.data.server.core.configs.find(entry => entry.id === profile.id).live_users, true);
        const live = fields => requestPreview(socket, 'get_live_config_users', { config_id: profile.id, node, ...fields });
        assert.equal((await live()).result.available, false);
        await assert.rejects(stat(requestsFile), { code: 'ENOENT' });

        const running = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'running');
        assert.equal((await requestPreview(socket, 'start_core')).ok, true);
        await running;
        await waitForFile(join(fixtureDir, 'live-fixture-ready'));
        assert.equal((await requestPreview(socket, 'get_config_users', { config_id: inactive.id, node })).ok, false);
        const first = await live();
        assert.equal(first.ok, true, first.feedback);
        assert.deepEqual(first.result, {
            available: true, total: 2, next_cursor: cursor,
            users: [{ id: user.id, group: user.gid, enabled: user.enabled, name: user.name_preview, usage: user.usage }],
        });
        const [{ id, ...request }] = await requests();
        assert.ok(id.length);
        assert.deepEqual(request, {
            auth: { client: 'fixture reader', token }, selector: { config: configName, node },
            operation: 'list-users', content_type: 'application/json', body_encoding: 'utf8', body: { limit: 50 },
        });

        const stale = await live({ cursor });
        assert.equal(stale.ok, false);
        assert.deepEqual(stale.result, { reset_cursor: true });
        assert.deepEqual((await requests())[1].body, { limit: 50, cursor });
        await waitForFile(join(fixtureDir, 'live-fixture-closed'));

        let foreignBytes = 0;
        const foreign = createServer(connection => {
            connection.on('error', () => {});
            connection.on('data', chunk => {
                foreignBytes += chunk.length;
                connection.end();
            });
        });
        foreign.listen(port, '127.0.0.1');
        await once(foreign, 'listening');
        t.after(() => new Promise(resolve => foreign.close(resolve)));
        const unavailable = await live();
        assert.equal(unavailable.ok, false);
        assert.doesNotMatch(unavailable.feedback, new RegExp(token));
        assert.equal(foreignBytes, 0, 'Wave sends no API bytes to the process that reused the port.');
    });

test('restores backup metadata atomically and preserves running and edited config files',
    { skip: process.platform !== 'linux' }, async t => {
        const { socket, initial } = await connectPreview(t);
        const observer = await connectPreview(t);
        const selected = await createCoreConfig(socket, 'Running profile', true);
        const running = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'running');
        assert.equal((await requestPreview(socket, 'start_core')).ok, true);
        const active = (await running).data.server.core.configs[0];
        assert.deepEqual(active, { ...selected, active: true });
        await waitForFile(coreFixture.processFile);
        const standby = await createCoreConfig(socket, 'Standby profile', true);
        const standbyFile = configPath(standby.name);
        const edited = (await readFile(standbyFile, 'utf8')) + '\n';
        await writeFile(standbyFile, edited);

        const blocked = join(coreFixture.configDir, 'Blocked profile');
        await mkdir(blocked);
        await chmod(blocked, 0o722);
        const stored = await readFile(database, 'utf8');
        const settings = { ...initial.settings, domain: 'restored.example.invalid' };
        const restoredName = 'Restored config';
        const configs = [{ name: standby.name, enabled: false }, { name: restoredName, enabled: true }];
        const backup = { format: 'wave-preview-backup', version: 1, settings, configs };
        const restore = configs => requestPreview(socket, 'restore_preview_backup', { backup: { ...backup, configs } });
        for (const name of [active.name, '../escape', 'Blocked profile']) {
            const rejected = await restore([...configs, { name, enabled: false }]);
            assert.equal(rejected.ok, false);
            assert.equal(rejected.feedback_in_logs_only, true);
            assert.ok(panelLogs(rejected, 'error').length > 0);
            assert.deepEqual(rejected.data.server.core.configs, [active, standby]);
        }
        assert.equal(await readFile(database, 'utf8'), stored);
        await assert.rejects(stat(configPath(restoredName)), { code: 'ENOENT' });

        const restored = await restore(configs);
        assert.equal(restored.ok, true, restored.feedback);
        assert.deepEqual(restored.data.settings, settings);
        assert.deepEqual(restored.data.server.core.configs.slice(0, 2), [active, { ...standby, enabled: false }]);
        assert.equal(JSON.parse(await readFile(configPath(restoredName), 'utf8')).name, restoredName);
        const cleared = await restore([]);
        assert.equal(cleared.ok, true, cleared.feedback);
        assert.deepEqual(cleared.data.server.core.configs, [active]);
        const { id, name, enabled } = active;
        assert.deepEqual(JSON.parse(await readFile(database, 'utf8')).core_configs, [{ id, name, enabled }]);
        assert.equal(await readFile(standbyFile, 'utf8'), edited);
        assert.ok((await stat(configPath(restoredName))).isFile());

        const stopped = nextPreviewSnapshot(observer.socket, frame => frame.data.server.core.state === 'stopped');
        assert.equal((await requestPreview(socket, 'stop_core')).ok, true);
        await stopped;
    });

async function expectStartupFailure(args, diagnostic, executable = backend) {
    await assert.rejects(run(executable, args, { cwd, timeout }), error => {
        assert.ok(Number.isInteger(error.code) && error.code !== 0, error);
        assert.match(error.stderr, diagnostic);
        assert.equal(error.stdout, '');
        return true;
    });
}

async function isolatedBackend(name, settings = {}) {
    const directory = join(cwd, name);
    const executable = join(directory, basename(backend));
    const db = join(directory, 'wave-db.json');
    await mkdir(directory);
    await copyFile(backend, executable);
    await seedCore(executable);
    await writeFile(db, JSON.stringify({ waterwall_install_attempted: true, ...settings }), { mode: 0o600 });
    return { executable, db };
}

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
