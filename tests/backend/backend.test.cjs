const assert = require('node:assert/strict');
const { execFile, spawn } = require('node:child_process');
const { createHash } = require('node:crypto');
const { once } = require('node:events');
const { appendFile, chmod, chown, mkdir, mkdtemp, readFile, readdir, rm, stat, symlink, unlink, writeFile } =
    require('node:fs/promises');
const { request: httpRequest } = require('node:http');
const { createServer } = require('node:net');
const { hostname, tmpdir } = require('node:os');
const { basename, delimiter, dirname, join } = require('node:path');
const { createInterface } = require('node:readline');
const { after, before, test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { isDeepStrictEqual, promisify } = require('node:util');
const { authFixture, emptyCatalogPath, fixtureSession, nextPreviewSnapshot, openPreview, panelBasePath,
    requestPreview, rotatingLogPath, seedCore } = require('./auth_fixture.cjs');
const { bundledSchematicFile, copyBackend, coreReleaseDir, schematicFile } = require('./tls_fixture.cjs');

const sourceBackend = process.env.WAVE_BACKEND;
const run = promisify(execFile);
const timeout = 5000;
const panelVersion = '0.0.2 beta';
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

function parseSchematic(source) {
    return JSON.parse(source, (_key, value, context) =>
        typeof value === 'number' && Number.isInteger(value) && !Number.isSafeInteger(value)
            ? JSON.rawJSON(context.source) : value);
}

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
    backend = await copyBackend(sourceBackend, cwd, 'bin with spaces');
    coreFixture = await seedCore(backend, { executable: process.platform === 'linux' });
    database = join(dirname(backend), 'wave-db.json');
    await writeFile(database, JSON.stringify({
        listen: { host: '127.0.0.1', port: 0 }, 'panel-base-path': panelBasePath, waterwall_install_attempted: true,
    }));
    await mkdir(join(cwd, siteDir));
    for (const [name, contents] of Object.entries(assets)) {
        await writeFile(join(cwd, siteDir, name), contents);
    }

    if (process.platform === 'linux') countersAtStartup = await readNetworkCounters();
    server = spawn(backend, ['--site-dir', siteDir], {
        cwd,
        env: { ...process.env, PATH: await emptyCatalogPath(cwd) },
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
    const fresh = await copyBackend(backend, cwd, 'fresh');
    const db = join(dirname(fresh), 'wave-db.json');
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

function websocketStatus(base, path, headers = {}) {
    const endpoint = new URL(path, base);
    return new Promise((resolve, reject) => {
        const probe = httpRequest(endpoint, {
            headers: {
                Origin: endpoint.origin, Connection: 'Upgrade', Upgrade: 'websocket',
                'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': Buffer.alloc(16).toString('base64'),
                ...headers,
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
}

function panelClient(origin) {
    const request = (path, options = {}) =>
        fetch(new URL(path, origin), { redirect: 'manual', signal: AbortSignal.timeout(timeout), ...options });
    const post = (path, data, headers = {}) => request(path, {
        method: 'POST', body: JSON.stringify(data),
        headers: { 'Content-Type': 'application/json', 'X-Wave-CSRF': '1', Origin: origin, ...headers },
    });
    return { request, post };
}

async function assertNotFound(response) {
    assert.equal(response.status, 404);
    assert.equal(response.headers.get('location'), null);
    assert.equal(response.headers.get('content-type'), 'text/html');
    assert.equal(response.headers.get('server'), 'nginx');
    const date = response.headers.get('date');
    assert.ok(date, '404 response is missing its Date header');
    const timestamp = new Date(date);
    assert.equal(date, timestamp.toUTCString());
    assert.ok(Math.abs(Date.now() - timestamp.getTime()) < 60_000, '404 response Date should be current');
    const expected = Buffer.from([
        '<html>',
        '<head><title>404 Not Found</title></head>',
        '<body>',
        '<center><h1>404 Not Found</h1></center>',
        '<hr><center>nginx</center>',
        '</body>',
        '</html>',
        '',
    ].join('\r\n'));
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), expected);
}

const bootstrapPath = '/_wave-setup';
const basePathEndpoint = `${bootstrapPath}/api/auth/base-path`;

test('selects a base path on first run and scopes the configured panel routes', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false, basePath: null, managed: true });
    const loopback = JSON.parse(await readFile(fixture.database, 'utf8'));
    await unlink(fixture.database);
    // Create the database without ever binding its fresh public listener defaults.
    await expectStartupFailure(['--site-dir', join(fixture.cwd, 'missing site')], /Cannot access site directory/,
        fixture.backend);
    const fresh = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.deepEqual(fresh.admin, []);
    assert.equal(fresh['panel-base-path'], undefined);
    assert.equal(fresh.waterwall_version, undefined);
    await writeFile(fixture.database, JSON.stringify({ ...fresh, ...loopback }));
    await fixture.start();
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).waterwall_version, '1.6.0');
    assert.equal(await readFile(fixture.ready, 'utf8'), `${fixture.pid}\n${fixture.origin}/\n`);

    const { request, post } = panelClient(fixture.origin);
    const chosen = '/chosen';
    const selection = { 'panel-base-path': chosen };
    for (const path of ['/', '/panel', `${bootstrapPath}/`]) {
        const response = await request(path);
        assert.equal(response.status, 303);
        assert.equal(response.headers.get('location'), `${bootstrapPath}/base-path`);
    }
    assert.equal(await (await request(`${bootstrapPath}/base-path`)).text(), fixture.index);
    assert.equal((await request(`${bootstrapPath}/index.wasm`)).status, 200);
    assert.equal((await request(`${bootstrapPath}/index.js`)).status, 200);
    await assertNotFound(await request(`${bootstrapPath}/api/session`));
    assert.equal(await websocketStatus(fixture.origin, `${bootstrapPath}/ws/workspace`), 404);
    assert.equal((await post(basePathEndpoint, selection, { Origin: 'https://other.example.test' })).status, 403);
    assert.equal((await post(basePathEndpoint, { 'panel-base-path': '/invalid/' })).status, 400);
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8'))['panel-base-path'], undefined);

    const saved = await post(basePathEndpoint, selection);
    assert.equal(saved.status, 201);
    assert.equal(saved.headers.get('location'), `${chosen}/`);
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8'))['panel-base-path'], chosen);
    assert.equal(await readFile(fixture.ready, 'utf8'), `${fixture.pid}\n${fixture.origin}${chosen}/\n`);
    for (const path of ['/', '/panel', '/panel/', `${bootstrapPath}/`, '/index.wasm', `${chosen}-other/`,
        `${chosen}/unknown`]) {
        await assertNotFound(await request(path));
    }
    await assertNotFound(await post(basePathEndpoint, { 'panel-base-path': '/other' }));
    const canonical = await request(chosen);
    assert.equal(canonical.status, 303);
    assert.equal(canonical.headers.get('location'), `${chosen}/`);
    assert.equal((await request(`${chosen}/`)).headers.get('location'), `${chosen}/setup`);
    assert.equal((await request(`${chosen}/index.wasm`)).status, 200);
    assert.equal((await request(`${chosen}/api/session`)).status, 401);

    const credentials = { username: 'bootstrap-admin', password: 'fixture bootstrap passphrase 7Dm3J' };
    assert.equal((await post(`${chosen}/api/auth/setup`, credentials)).status, 201);
    assert.equal((await request(`${chosen}/`)).headers.get('location'), `${chosen}/sign-in`);
    const login = await post(`${chosen}/api/auth/sign-in`, credentials);
    assert.equal(login.status, 204);
    const headers = { Cookie: login.headers.get('set-cookie').split(';', 1)[0] };
    assert.equal(await (await request(`${chosen}/`, { headers })).text(), fixture.index);
    assert.equal((await (await request(`${chosen}/api/session`, { headers })).json()).username, credentials.username);
    const { stdout: info } = await run(fixture.backend, ['--local-settings']);
    assert.match(info, /Administrator: "bootstrap-admin"/);
    assert.match(info, /Base path: \/chosen/);
    assert.doesNotMatch(info, /argon2|wave_session|signing_key/);
    for (const path of ['/ws/workspace', '/panel/ws/workspace', `${chosen}-other/ws/workspace`]) {
        assert.equal(await websocketStatus(fixture.origin, path, headers), 404);
    }
    assert.equal(await websocketStatus(fixture.origin, `${chosen}/ws/workspace`), 401);
    const { initial } = await connectPreview(t, fixture.origin, headers.Cookie, chosen);
    assert.ok(initial.server);
    const session = connectSocket('/ws/panel', fixture.origin, headers.Cookie, chosen);
    t.after(() => session.close());
    const [frame] = await once(session, 'message', { signal: AbortSignal.timeout(timeout) });
    assert.equal(JSON.parse(frame.data).username, credentials.username);
});

test('requires the existing administrator to sign in before recovering the base path', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const { request, post } = panelClient(fixture.origin);
    const credentials = { username: 'recovery-admin', password: 'fixture recovery passphrase 8Tz4L' };
    await fixture.start();
    assert.equal((await post(`${panelBasePath}/api/auth/setup`, credentials)).status, 201);
    await fixture.stop();
    const database = JSON.parse(await readFile(fixture.database, 'utf8'));
    delete database['panel-base-path'];
    await writeFile(fixture.database, JSON.stringify(database));
    await fixture.start();

    const chosen = '/chosen';
    const selection = { 'panel-base-path': chosen };
    const entry = await request('/');
    assert.equal(entry.status, 303);
    assert.equal(entry.headers.get('location'), `${bootstrapPath}/sign-in`);
    assert.equal(await (await request(`${bootstrapPath}/sign-in`)).text(), fixture.index);
    assert.equal((await post(basePathEndpoint, selection)).status, 401);
    assert.equal((await post(basePathEndpoint, selection, { Cookie: 'wave_session=invalid' })).status, 401);
    await assertNotFound(await post(`${bootstrapPath}/api/auth/setup`, credentials));
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8'))['panel-base-path'], undefined);

    const login = await post(`${bootstrapPath}/api/auth/sign-in`, credentials);
    assert.equal(login.status, 204);
    const headers = { Cookie: login.headers.get('set-cookie').split(';', 1)[0] };
    assert.equal((await request('/', { headers })).headers.get('location'), `${bootstrapPath}/base-path`);
    const saved = await post(basePathEndpoint, selection, headers);
    assert.equal(saved.status, 201);
    assert.equal(saved.headers.get('location'), `${chosen}/`);
    const persisted = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.equal(persisted['panel-base-path'], chosen);
    assert.deepEqual(persisted.admin, database.admin);
    assert.equal((await request(`${chosen}/`, { headers })).status, 200);
    assert.equal((await (await request(`${chosen}/api/session`, { headers })).json()).username, credentials.username);
    await assertNotFound(await post(basePathEndpoint, selection, headers));
    await assertNotFound(await request(`${panelBasePath}/`, { headers }));
});

test('serves uncached entry assets and protects HTTP data with an administrator session', async () => {
    const credentials = { username: 'fixture-user', password: 'fake fixture test passphrase 9Jw4c' };
    const web = panelClient(url.origin);
    const request = (path, options) => web.request(panelBasePath + path, options);
    const post = (action, origin = url.origin) =>
        web.post(`${panelBasePath}/api/auth/${action}`, credentials, { Origin: origin });
    const catalogStaging = join(dirname(backend), 'waterwall_bin', '.waterwall-catalog-');
    async function persistentFiles(directory) {
        const files = [];
        for (const entry of await readdir(directory, { withFileTypes: true })) {
            const path = join(directory, entry.name);
            // The background catalog check creates and removes private staging directories.
            if (entry.isDirectory() && path.startsWith(catalogStaging)) continue;
            files.push(path);
            if (entry.isDirectory()) files.push(...await persistentFiles(path));
        }
        return files.sort();
    }
    const files = await persistentFiles(cwd);
    assert.equal((await request('/')).headers.get('location'), `${panelBasePath}/setup`);
    for (const [path, asset] of [['/setup', 'index.html'], ['/index.js', 'index.js'], ['/index.wasm', 'index.wasm']]) {
        const response = await request(path);
        assert.equal(response.status, 200);
        assert.equal(response.headers.get('cache-control'), 'no-store');
        assert.deepEqual(Buffer.from(await response.arrayBuffer()), assets[asset]);
        if (asset === 'index.wasm') assert.equal(response.headers.get('content-type'), 'application/wasm');
    }
    assert.equal((await request('/api/session')).status, 401);
    assert.equal(await websocketStatus(url, `${panelBasePath}/ws/workspace`), 401);

    assert.equal((await post('setup', 'https://other.example.test')).status, 403);
    assert.equal((await post('setup')).status, 201);
    assert.equal((await post('setup')).status, 409);
    assert.equal((await request('/')).headers.get('location'), `${panelBasePath}/sign-in`);
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
    assert.deepEqual(await persistentFiles(cwd), files);
});

function connectSocket(path, base = url, sessionCookie = cookie, basePath = panelBasePath) {
    const endpoint = new URL(basePath + path, base);
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

function connectPreview(t, base = url, sessionCookie = cookie, basePath = panelBasePath) {
    return openPreview(t, base, sessionCookie, basePath);
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
        assert.equal(initial.server.version, panelVersion);
        assert.deepEqual(initial.server.core.available_versions, ['1.6.0']);
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
        const { stdout: coreLogs } = await run(backend, ['--local-core-logs']);
        assert.match(coreLogs, /\[core\] \[warning\] Earlier file content/);
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

test('lets WaterWall complete its shutdown when Wave receives SIGTERM',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, managed: true });
        const core = await seedCore(fixture.backend, { executable: true, observeShutdown: true });
        await fixture.start();
        const session = await fixtureSession(fixture);
        const { socket } = await openPreview(t, fixture.origin, session, fixture.basePath);
        await createCoreConfig(socket, 'Shutdown fixture', true);
        const started = await requestPreview(socket, 'start_core');
        assert.equal(started.ok, true, started.feedback);
        const child = JSON.parse(await waitForFile(core.processFile));

        assert.deepEqual(await fixture.stop(), [0, null], fixture.stderr);
        assert.equal(await readFile(core.shutdownFile, 'utf8'), String(child.pid));
    });

const supportedInstaller = process.platform === 'linux' && ['x64', 'arm64'].includes(process.arch);

test('checks Wave and WaterWall together and retains the latest known versions on failure',
    { skip: !supportedInstaller }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false });
        const tools = join(fixture.cwd, 'panel update tools');
        const temporary = join(fixture.cwd, 'temporary');
        const tags = join(fixture.cwd, 'tags response');
        const calls = join(fixture.cwd, 'tag requests');
        const catalogCalls = join(fixture.cwd, 'schematic requests');
        const release = join(fixture.cwd, 'release tag response');
        const catalogRelease = join(fixture.cwd, 'release schematic response');
        const endpoint = 'https://api.github.com/repos/radkesvat/wave/tags?per_page=100&page=';
        const catalogEndpoint = 'https://api.github.com/repos/radkesvat/wave/contents/backend/schematics?ref=main';
        await Promise.all([mkdir(tools), mkdir(temporary)]);
        await writeFile(join(tools, 'curl'), [
            '#!/bin/sh',
            'for url do :; done',
            'case "$url" in',
            `  "${catalogEndpoint}")`,
            '    printf "%s\\n" "$url" >> "$WAVE_FIXTURE_CATALOG_CALLS"',
            '    while [ ! -f "$WAVE_FIXTURE_CATALOG_RELEASE" ]; do sleep 0.02; done',
            '    printf "[]" ;;',
            `  "${endpoint}"*)`,
            '    printf "%s\\n" "$url" >> "$WAVE_FIXTURE_TAG_CALLS"',
            '    while [ ! -f "$WAVE_FIXTURE_TAG_RELEASE" ]; do sleep 0.02; done',
            '    page=${url##*page=}',
            '    [ -f "$WAVE_FIXTURE_TAGS.$page" ] || exit 22',
            '    cat "$WAVE_FIXTURE_TAGS.$page" ;;',
            '  *) exit 22 ;;',
            'esac',
            '',
        ].join('\n'), { mode: 0o700 });
        await fixture.start({
            PATH: tools + delimiter + process.env.PATH,
            TMPDIR: temporary,
            WAVE_FIXTURE_TAGS: tags,
            WAVE_FIXTURE_TAG_CALLS: calls,
            WAVE_FIXTURE_CATALOG_CALLS: catalogCalls,
            WAVE_FIXTURE_CATALOG_RELEASE: catalogRelease,
            WAVE_FIXTURE_TAG_RELEASE: release,
        });
        assert.equal(await waitForFile(catalogCalls), `${catalogEndpoint}\n`);
        const session = await fixtureSession(fixture);
        const { socket, initial, schematicRevision } = await connectPreview(t, fixture.origin, session);
        const { version, core } = initial.server;
        assert.equal(version, panelVersion);
        const baseVersion = version.replace(/ beta$/, '');
        const [major, minor, patch] = baseVersion.split('.').map(Number);
        const newer = `${major}.${minor + 10}.${patch}`;
        const fullPage = first => [first, ...Array.from({ length: 99 }, (_, index) => `legacy-${index}`)];
        assert.deepEqual(initial.server.panel, {
            checking_updates: false, update_check_generation: 0, update_available: false,
        });
        assert.equal(initial.server.checking_updates, false);
        assert.equal(core.latest_version, core.version);
        assert.equal(core.update_available, false);
        await assert.rejects(stat(calls), { code: 'ENOENT' });

        const check = async (generation, pages, previous, beforeResult) => {
            for (const page of [1, 2]) {
                if (pages[page - 1] === undefined) await rm(`${tags}.${page}`, { force: true });
                else await writeFile(`${tags}.${page}`, JSON.stringify(pages[page - 1].map(name => ({ name }))));
            }
            const completed = nextPreviewSnapshot(socket, frame =>
                frame.data.server.panel.update_check_generation === generation &&
                !frame.data.server.panel.checking_updates && !frame.data.server.core.checking_updates);
            const ack = await requestPreview(socket, 'check_updates');
            assert.equal(ack.ok, true, ack.feedback);
            assert.equal(ack.data.server.checking_updates, true);
            assert.equal(ack.data.server.panel.checking_updates, true);
            assert.equal(ack.data.server.core.checking_updates, true);
            assert.equal(ack.data.server.panel.latest_version, previous);
            await beforeResult?.();
            const result = await completed;
            assert.equal(result.data.server.checking_updates, false);
            assert.equal(result.data.server.version, version);
            assert.equal(result.data.server.core.version, core.version);
            assert.equal(result.data.server.core.latest_version, core.version);
            assert.equal(result.schematic_revision, schematicRevision);
            assert.deepEqual(await readdir(temporary), [], 'The panel check removes its staging directory.');
            return result;
        };

        const available = await check(1, [fullPage(`v${major}.${minor + 9}.${patch}`),
            [`v${newer}-beta`, `v${newer}`]], undefined, async () => {
            assert.equal(await waitForFile(calls), `${endpoint}1\n`);
            const [staging] = await readdir(temporary);
            assert.match(staging, /^wave-panel-check-/);
            assert.equal((await stat(join(temporary, staging))).mode & 0o777, 0o700);
            const duplicate = await requestPreview(socket, 'check_updates');
            assert.equal(duplicate.ok, true);
            const panelFinished = nextPreviewSnapshot(socket, frame =>
                !frame.data.server.panel.checking_updates && frame.data.server.panel.update_check_generation === 1);
            await writeFile(release, '');
            const partial = await panelFinished;
            assert.equal(partial.data.server.panel.latest_version, newer);
            assert.equal(partial.data.server.core.checking_updates, true);
            assert.equal(partial.data.server.checking_updates, true);
            const joined = await requestPreview(socket, 'check_updates');
            assert.equal(joined.ok, true, joined.feedback);
            assert.equal(joined.data.server.panel.checking_updates, false);
            await writeFile(catalogRelease, '');
        });
        assert.equal(available.data.server.panel.latest_version, newer);
        assert.equal(available.data.server.panel.update_available, true);
        assert.equal(panelLogs(available, 'info', `Wave ${newer} is available.`).length, 1);
        assert.equal(panelLogs(available, 'success', 'WaterWall update check completed.').length, 1);
        assert.equal(await readFile(calls, 'utf8'), `${endpoint}1\n${endpoint}2\n`);
        assert.equal(await readFile(catalogCalls, 'utf8'), `${catalogEndpoint}\n`);

        const failed = await check(2, [fullPage(`v${major + 1}.0.0`)], newer);
        assert.equal(failed.data.server.panel.latest_version, newer);
        assert.equal(failed.data.server.panel.update_available, true);
        assert.match(failed.data.server.panel.update_check_error, /GitHub/);
        assert.equal(panelLogs(failed, 'error', failed.data.server.panel.update_check_error).length, 1);
        assert.equal(await readFile(calls, 'utf8'), `${endpoint}1\n${endpoint}2\n`.repeat(2));

        const invalid = await check(3, [['unversioned']], newer);
        assert.equal(invalid.data.server.panel.latest_version, newer);
        assert.match(invalid.data.server.panel.update_check_error, /tag/i);
        assert.equal(panelLogs(invalid, 'error', invalid.data.server.panel.update_check_error).length, 1);

        const current = await check(4, [[`v${baseVersion}-beta`]], newer);
        assert.equal(current.data.server.panel.latest_version, version);
        assert.equal(current.data.server.panel.update_available, false);
        assert.equal(panelLogs(current, 'info', 'No newer Wave panel version is available.').length, 1);

        const stable = await check(5, [[`v${baseVersion}`]], version);
        assert.equal(stable.data.server.panel.latest_version, baseVersion);
        assert.equal(stable.data.server.panel.update_available, true);
        assert.equal(panelLogs(stable, 'info', `Wave ${baseVersion} is available.`).length, 1);

        const older = await check(6, [['v0.0.0']], baseVersion);
        assert.equal(older.data.server.panel.latest_version, '0.0.0');
        assert.equal(older.data.server.panel.update_available, false);

        const empty = await check(7, [[]], '0.0.0');
        assert.equal(empty.data.server.panel.latest_version, undefined);
        assert.equal(empty.data.server.panel.update_available, false);
        assert.equal(empty.data.server.panel.update_check_error, undefined);
        assert.equal(panelLogs(empty, 'info', 'Wave has no version tags on GitHub yet.').length, 1);
        assert.equal(await readFile(catalogCalls, 'utf8'), `${catalogEndpoint}\n`.repeat(7));
    });

test('sends the selected schematic only in the first authenticated workspace snapshot', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    assert.equal(await websocketStatus(fixture.origin, `${fixture.basePath}/ws/workspace`), 401);
    const session = await fixtureSession(fixture);
    const { socket, initial, schematic } = await connectPreview(t, fixture.origin, session);
    const bundled = JSON.parse(await readFile(bundledSchematicFile, 'utf8'));
    assert.deepEqual(schematic, bundled);
    assert.equal(initial.server.core.version, bundled.version);
    assert.equal(Object.keys(schematic.nodes).length, 95);
    assert.equal((await requestPreview(socket, 'refresh')).schematic, undefined);
    assert.equal((await nextPreviewSnapshot(socket)).schematic, undefined);
    assert.deepEqual((await connectPreview(t, fixture.origin, session)).schematic, bundled);
});

test('serves authenticated workspace snapshots and actions over HTTP with session and workspace checks', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false, basePath: '/test/wave' });
    await fixture.start();
    const cookie = await fixtureSession(fixture);
    const { request, post } = panelClient(fixture.origin);
    const route = fixture.basePath + '/api/workspace';
    const identity = await request(fixture.basePath + '/api/session', { headers: { Cookie: cookie } });
    assert.equal(identity.status, 200);
    const sessionId = (await identity.json()).session_id;
    const headers = { Cookie: cookie, 'X-Wave-Session': sessionId };

    assert.equal((await request(route, { headers: { 'X-Wave-Session': sessionId } })).status, 401);
    assert.equal((await request(route, { headers: { Cookie: cookie } })).status, 401);
    assert.equal((await request(route, { headers: { ...headers, 'X-Wave-Session': 'different-sign-in' } })).status, 401);
    await assertNotFound(await request('/api/workspace', { headers }));

    const snapshot = async (query = '') => {
        const response = await request(route + query, { headers });
        assert.equal(response.status, 200);
        assert.equal(response.headers.get('cache-control'), 'no-store');
        const frame = await response.json();
        assert.equal(frame.type, 'snapshot');
        assert.equal(frame.users_roster, false);
        return frame;
    };
    const first = await snapshot();
    assert.equal(first.data.settings.theme, 'default');
    assert.ok(first.workspace_id);
    assert.equal(first.schematic.version, first.data.server.core.version);
    const known = `?workspace_id=${encodeURIComponent(first.workspace_id)}&schematic_revision=${first.schematic_revision}`;
    assert.equal((await snapshot(known)).schematic, undefined);
    assert.equal((await snapshot(`?schematic_revision=${first.schematic_revision}`)).schematic.version, first.schematic.version);
    assert.equal((await request(`${route}?schematic_revision=-1`, { headers })).status, 400);

    const actionHeaders = { ...headers, 'X-Wave-Workspace': first.workspace_id };
    const action = { action: 'apply_theme', theme: 'dark', request_id: 42, schematic_revision: first.schematic_revision };
    assert.equal((await post(route, action, headers)).status, 409);
    assert.equal((await post(route, action, { ...actionHeaders, 'X-Wave-Session': 'different-sign-in' })).status, 401);
    assert.equal((await post(route, action, { ...actionHeaders, Origin: 'https://other.example.test' })).status, 403);
    assert.equal((await post(route, action, { ...actionHeaders, 'X-Wave-CSRF': '0' })).status, 403);
    assert.equal((await snapshot(known)).data.settings.theme, 'default');

    const refused = await post(route, { ...action, action: 'users_roster_subscribe' }, actionHeaders);
    assert.equal(refused.status, 200);
    const roster = await refused.json();
    assert.equal(roster.ok, false);
    assert.equal(roster.request_id, action.request_id);
    assert.match(roster.feedback, /requires a WebSocket/);

    const tooLarge = await post(route, { ...action, unused: 'x'.repeat(256 * 1024 + 1024) }, actionHeaders);
    assert.equal(tooLarge.status, 413);
    const rawPost = (body, options) => request(route, {
        method: 'POST', body, ...options,
        headers: { ...actionHeaders, 'Content-Type': 'application/json', 'X-Wave-CSRF': '1', Origin: fixture.origin },
    });
    const tooLargeChunked = await rawPost((async function* () { yield 'x'.repeat(256 * 1024 + 1025); })(),
        { duplex: 'half' });
    assert.equal(tooLargeChunked.status, 413);
    const deepAction = JSON.stringify(action).slice(0, -1) + ',"unused":' + '['.repeat(70) + '0' + ']'.repeat(70) + '}';
    assert.equal((await (await rawPost(deepAction)).json()).ok, false);
    assert.equal((await snapshot(known)).data.settings.theme, 'default');
    const saved = await post(route, action, actionHeaders);
    assert.equal(saved.status, 200);
    const reply = await saved.json();
    assert.equal(reply.ok, true, reply.feedback);
    assert.equal(reply.request_id, action.request_id);
    assert.equal(reply.workspace_id, first.workspace_id);
    assert.equal(reply.users_roster, false);
    assert.equal(reply.schematic, undefined);
    assert.equal((await snapshot(known)).data.settings.theme, 'dark');

    const replacementCookie = await fixtureSession(fixture, { setup: false });
    assert.equal((await post(route, action, { ...actionHeaders, Cookie: replacementCookie })).status, 401);
    await fixture.stop();
    await fixture.start();
    const restarted = await snapshot(known);
    assert.notEqual(restarted.workspace_id, first.workspace_id);
    assert.equal(restarted.schematic.version, first.schematic.version);
    assert.equal((await post(route, { ...action, theme: 'light' }, actionHeaders)).status, 409);
    assert.equal((await snapshot()).data.settings.theme, 'dark');

    assert.equal((await post(fixture.basePath + '/api/auth/sign-out', {}, headers)).status, 204);
    assert.equal((await request(route, { headers })).status, 401);
});

test('seeds the embedded schematic on first startup under umask 0002 and uses a corrected cache on restart', async t => {
    const previous = process.umask(0o002);
    try {
        const fixture = await authFixture(t, undefined, { encrypted: false, prepareCore: false });
        const settings = {
            ...JSON.parse(await readFile(fixture.database, 'utf8')),
            admin: [],
            waterwall_install_attempted: true,
            panel_name: 'Fixture',
        };
        await writeFile(fixture.database, JSON.stringify(settings));
        const cached = schematicFile(fixture.backend);
        assert.deepEqual((await readdir(dirname(fixture.backend))).sort(),
            [basename(fixture.backend), basename(fixture.database)].sort());

        await fixture.start();
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')), {
            ...settings, waterwall_version: '1.6.0',
        });
        const original = await readFile(bundledSchematicFile, 'utf8');
        assert.equal(await readFile(cached, 'utf8'), original);
        assert.deepEqual(await readdir(dirname(cached)), ['waterwall-1.6.0.json']);
        await fixture.stop();

        const corrected = original.replace('"label": "Core"', '"label": "Corrected core label"');
        assert.notEqual(corrected, original);
        await writeFile(cached, corrected);
        await fixture.start();
        assert.equal(await readFile(cached, 'utf8'), corrected);
        const { schematic } = await connectPreview(t, fixture.origin, await fixtureSession(fixture));
        assert.deepEqual(schematic, JSON.parse(corrected));
    } finally {
        process.umask(previous);
    }
});

const unprivilegedUser = process.getuid?.() === 0 ? { uid: 65534, gid: 65534 } : undefined;

async function writeWithMode(path, contents, mode = 0o644) {
    await writeFile(path, contents, { mode });
    await chmod(path, mode);
}

test('starts as an unprivileged user when an optional legacy directory is unreadable',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, runAs: unprivilegedUser });
        await fixture.start();
        await fixture.stop();
        const release = coreReleaseDir(fixture.backend);
        const bin = dirname(release);
        const previousCache = join(bin, 'schematics');
        await mkdir(previousCache, { mode: 0o000 });
        await chmod(previousCache, 0o000);
        const cached = await readFile(schematicFile(fixture.backend));

        try {
            await fixture.start();
            assert.deepEqual(await readFile(schematicFile(fixture.backend)), cached);
        } finally {
            await chmod(previousCache, 0o700);
        }
    });

test('starts the selected release and warns when the legacy binary cannot be read or its destination inspected',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, runAs: unprivilegedUser });
        const version = '1.6.1';
        const release = coreReleaseDir(fixture.backend, version);
        const legacyRelease = coreReleaseDir(fixture.backend);
        const legacyProgram = join(dirname(legacyRelease), 'Waterwall');
        const selectedProgram = join(release, 'Waterwall');
        const selectedSchematic = schematicFile(fixture.backend, version);
        const legacyBinary = await readFile('/bin/true');
        const original = await readFile(bundledSchematicFile, 'utf8');
        await mkdir(release, { mode: 0o700 });
        await chmod(release, 0o700);
        await writeWithMode(selectedProgram, '#!/bin/sh\nexit 0\n', 0o700);
        await writeWithMode(selectedSchematic, original.replace('"version": "1.6.0"', `"version": "${version}"`));
        await writeWithMode(legacyProgram, legacyBinary, 0o100);
        if (unprivilegedUser) {
            const { uid, gid } = unprivilegedUser;
            for (const path of [release, selectedProgram, selectedSchematic, legacyProgram]) {
                await chown(path, uid, gid);
            }
        }
        const originalLegacy = await stat(legacyProgram);
        await run(legacyProgram, [], { cwd: fixture.cwd, timeout, ...unprivilegedUser });
        const settings = JSON.parse(await readFile(fixture.database, 'utf8'));
        await writeFile(fixture.database, JSON.stringify({ ...settings, waterwall_version: version }));

        await fixture.start();
        const session = await fixtureSession(fixture);
        const { initial, schematic } = await connectPreview(t, fixture.origin, session);
        assert.equal(initial.server.core.version, version);
        assert.equal(initial.server.core.state, 'stopped');
        assert.equal(schematic.version, version);
        await fixture.stop();

        assert.match(fixture.stderr, /Warning: Could not migrate legacy WaterWall executable/);
        assert.ok(fixture.stderr.includes(legacyProgram));
        assert.match(fixture.stderr, /Cannot copy the legacy executable into staging\. Permission denied/);
        const remainingLegacy = await stat(legacyProgram);
        assert.equal(remainingLegacy.ino, originalLegacy.ino);
        assert.equal(remainingLegacy.mode & 0o777, 0o100);
        assert.deepEqual(await readdir(legacyRelease), ['waterwall-1.6.0.json']);
        await chmod(legacyProgram, 0o700);
        assert.deepEqual(await readFile(legacyProgram), legacyBinary);

        await chmod(legacyRelease, 0o600);
        try {
            await fixture.start();
            await fixture.stop();
            assert.ok(fixture.stderr.includes(join(legacyRelease, 'Waterwall')));
            assert.match(fixture.stderr, /Cannot inspect the migration destination\. Permission denied/);
        } finally {
            await chmod(legacyRelease, 0o700);
        }
        assert.deepEqual(await readdir(legacyRelease), ['waterwall-1.6.0.json']);
    });

test('keeps a root-owned legacy WaterWall binary executable after migration changes its owner',
    { skip: process.platform !== 'linux' || !unprivilegedUser }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, runAs: unprivilegedUser });
        const release = coreReleaseDir(fixture.backend);
        const legacy = join(dirname(release), 'Waterwall');
        const migrated = join(release, 'Waterwall');
        const runAsService = { cwd: fixture.cwd, timeout, ...unprivilegedUser };
        await writeWithMode(legacy, await readFile('/bin/true'), 0o655);
        await run(legacy, [], runAsService);
        await fixture.start();
        await fixture.stop();
        assert.equal((await stat(migrated)).uid, unprivilegedUser.uid);
        assert.equal((await stat(legacy)).mode & 0o777, 0o655);
        await run(migrated, [], runAsService);
    });

test('migrates legacy WaterWall binaries and valid schematics into version folders without overwriting them', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const cached = schematicFile(fixture.backend);
    const release = coreReleaseDir(fixture.backend);
    const bin = dirname(release);
    const previousCache = join(bin, 'schematics');
    const olderCache = join(dirname(fixture.backend), 'schematics');
    for (const directory of [previousCache, olderCache]) {
        await mkdir(directory, { mode: 0o755 });
        await chmod(directory, 0o755);
    }
    const previousPath = join(previousCache, 'waterwall-1.6.0.json');
    const olderPath = join(olderCache, 'waterwall-1.6.0.json');
    const original = await readFile(bundledSchematicFile, 'utf8');
    await writeWithMode(previousPath, '{}');
    await writeWithMode(olderPath, `${original}\n`);
    const schematicVersion = version => original.replace('"version": "1.6.0"', `"version": "${version}"`);
    const previous = schematicVersion('1.6.1');
    await writeWithMode(join(previousCache, 'waterwall-1.6.1.json'), previous);
    await writeWithMode(join(olderCache, 'waterwall-1.6.1.json'), `${previous}\n`);
    const linkedSchematic = join(fixture.cwd, 'waterwall-1.6.2.json');
    await writeWithMode(linkedSchematic, schematicVersion('1.6.2'));
    await symlink(linkedSchematic, join(previousCache, 'waterwall-1.6.2.json'));
    const writableSchematic = join(olderCache, 'waterwall-1.6.3.json');
    await writeWithMode(writableSchematic, schematicVersion('1.6.3'), 0o666);
    const redirectedRelease = join(fixture.cwd, 'redirected release');
    await mkdir(redirectedRelease, { mode: 0o755 });
    await chmod(redirectedRelease, 0o755);
    await writeWithMode(join(redirectedRelease, 'waterwall-1.6.4.json'), schematicVersion('1.6.4'));
    await symlink(redirectedRelease, coreReleaseDir(fixture.backend, '1.6.4'));
    const legacyProgram = join(bin, 'Waterwall');
    const program = join(release, 'Waterwall');
    const previousBinary = '#!/bin/sh\nexit 0\n';
    await writeWithMode(legacyProgram, previousBinary, 0o700);
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { initial } = await connectPreview(t, fixture.origin, session);
    assert.equal(initial.server.core.state, 'stopped');
    assert.deepEqual(initial.server.core.available_versions, ['1.6.1', '1.6.0']);
    assert.equal(await readFile(cached, 'utf8'), `${original}\n`);
    assert.equal(await readFile(previousPath, 'utf8'), '{}');
    assert.equal(await readFile(olderPath, 'utf8'), `${original}\n`);
    assert.equal(await readFile(schematicFile(fixture.backend, '1.6.1'), 'utf8'), previous);
    assert.equal(await readFile(program, 'utf8'), previousBinary);
    assert.equal(await readFile(legacyProgram, 'utf8'), previousBinary);
    assert.equal((await stat(program)).mode & 0o777, 0o700);
    for (const version of ['1.6.2', '1.6.3']) {
        await assert.rejects(stat(schematicFile(fixture.backend, version)), { code: 'ENOENT' });
    }
    assert.deepEqual((await readdir(release)).sort(), ['Waterwall', 'waterwall-1.6.0.json']);
    await fixture.stop();

    await writeFile(previousPath, original);
    await writeFile(legacyProgram, '#!/bin/sh\nexit 1\n');
    await fixture.start();
    assert.equal(await readFile(cached, 'utf8'), `${original}\n`);
    assert.equal(await readFile(program, 'utf8'), previousBinary);
});

test('finds core updates, keeps the active release on failure, and reloads the selected version',
    { skip: !supportedInstaller }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false });
        const currentCore = await seedCore(fixture.backend, { executable: true });
        const currentExecutable = await readFile(currentCore.program, 'utf8');
        const original = await readFile(bundledSchematicFile, 'utf8');
        const current = parseSchematic(original);
        const selected = { ...current, version: '1.6.1', config: { ...current.config }, nodes: { ...current.nodes } };
        delete selected.config.users_starter;
        selected.nodes.FixtureNextRelease = {
            ...selected.nodes.Template,
            graph_rules: [{
                assert: ['eq', ['get', '/ready'], true],
                message: 'Enable the fixture node.',
            }],
        };

        const payload = 'fixture archive bytes for the selected release\n';
        const archive = 'Fixture-Waterwall-linux.zip';
        const platform = process.arch === 'arm64' ? 'linux-aarch64' : 'linux-x86_64';
        selected.packages = {
            ...current.packages,
            [platform]: { archive, sha256: createHash('sha256').update(payload).digest('hex') },
        };
        const schematicBytes = Buffer.from(JSON.stringify(selected));
        const catalogEntry = (version, bytes) => ({
            name: `waterwall-${version}.json`, type: 'file', size: bytes.length,
            sha: createHash('sha1').update(`blob ${bytes.length}\0`).update(bytes).digest('hex'),
        });
        const candidate = catalogEntry(selected.version, schematicBytes);

        const tools = join(fixture.cwd, 'fixture tools');
        const listing = join(fixture.cwd, 'catalog listing');
        const blobs = join(fixture.cwd, 'catalog blobs');
        const catalogCalls = join(fixture.cwd, 'catalog calls');
        const archiveCalls = join(fixture.cwd, 'archive calls');
        const unzipCalls = join(fixture.cwd, 'unzip calls');
        const archiveFile = join(fixture.cwd, 'fixture archive');
        const archiveHold = join(fixture.cwd, 'archive hold');
        const installedExecutable = '#!/bin/sh\nexit 0\n';
        await mkdir(tools);
        await mkdir(blobs);
        await writeFile(join(blobs, candidate.sha), schematicBytes);
        await writeFile(join(tools, 'curl'), [
            '#!/bin/sh',
            'for url do :; done',
            'case "$url" in',
            '  "https://api.github.com/repos/radkesvat/wave/contents/backend/schematics?ref=main")',
            '    printf "%s\\n" "$url" >> "$WAVE_FIXTURE_CATALOG_CALLS"',
            '    cat "$WAVE_FIXTURE_CATALOG_LISTING" ;;',
            '  "https://api.github.com/repos/radkesvat/wave/tags?per_page=100&page=1") printf "[]" ;;',
            '  "https://api.github.com/repos/radkesvat/wave/git/blobs/"*)',
            '    printf "%s\\n" "$url" >> "$WAVE_FIXTURE_CATALOG_CALLS"',
            '    cat "$WAVE_FIXTURE_CATALOG_BLOBS/${url##*/}" ;;',
            '  "https://github.com/radkesvat/WaterWall/releases/download/"*)',
            '    printf "%s\\n" "$url" >> "$WAVE_FIXTURE_ARCHIVE_CALLS"',
            '    while [ -f "$WAVE_FIXTURE_ARCHIVE_HOLD" ]; do sleep 0.02; done',
            '    cat "$WAVE_FIXTURE_ARCHIVE" ;;',
            '  *) exit 22 ;;',
            'esac',
            '',
        ].join('\n'), { mode: 0o700 });
        await writeFile(join(tools, 'unzip'), [
            '#!/bin/sh',
            '[ "$1" = "-p" ] && [ "$3" = "Waterwall" ] || exit 2',
            'cmp "$2" "$WAVE_FIXTURE_ARCHIVE" || exit 3',
            'printf "%s\\n" "$3" >> "$WAVE_FIXTURE_UNZIP_CALLS"',
            `printf '%s' '${installedExecutable}'`,
            '',
        ].join('\n'), { mode: 0o700 });
        const env = {
            PATH: tools + delimiter + process.env.PATH,
            WAVE_FIXTURE_CATALOG_LISTING: listing,
            WAVE_FIXTURE_CATALOG_BLOBS: blobs,
            WAVE_FIXTURE_CATALOG_CALLS: catalogCalls,
            WAVE_FIXTURE_ARCHIVE_CALLS: archiveCalls,
            WAVE_FIXTURE_UNZIP_CALLS: unzipCalls,
            WAVE_FIXTURE_ARCHIVE: archiveFile,
            WAVE_FIXTURE_ARCHIVE_HOLD: archiveHold,
        };

        await fixture.start(env);
        assert.match(await waitForFile(catalogCalls), /backend\/schematics\?ref=main/);
        const session = await fixtureSession(fixture);
        const { socket, initial, schematic, schematicRevision } = await connectPreview(t, fixture.origin, session);
        assert.equal(initial.server.version, panelVersion);
        assert.equal(initial.server.core.version, '1.6.0');
        assert.equal(initial.server.core.state, 'stopped');
        assert.deepEqual(initial.server.core.available_versions, ['1.6.0']);
        assert.equal(schematic.version, '1.6.0');
        assert.ok(schematicRevision > 0);
        const compatible = await requestPreview(socket, 'apply_core_settings', {
            schematic_revision: undefined, settings: { misc: { workers: 2 } },
        });
        assert.equal(compatible.ok, true, compatible.feedback);
        const startupError = 'Could not fetch the Wave schematic list from GitHub.';
        const startupFailure = await nextPreviewSnapshot(socket, frame =>
            !frame.data.server.core.checking_updates && frame.data.server.core.update_check_error === startupError);
        assert.equal(panelLogs(startupFailure, 'error', startupError).length, 1);

        const changedActive = Buffer.from(JSON.stringify({
            ...current,
            packages: {
                ...current.packages,
                [platform]: { ...current.packages[platform], sha256: 'a'.repeat(64) },
            },
        }));
        const activeEntry = catalogEntry(current.version, changedActive);
        await writeFile(join(blobs, activeEntry.sha), changedActive);
        await writeFile(listing, JSON.stringify([activeEntry]));
        await unlink(currentCore.program);
        const activeProtected = nextPreviewSnapshot(socket, frame =>
            !frame.data.server.core.checking_updates &&
            frame.data.server.core.update_check_error?.includes('published archive metadata changed'));
        const checkActive = await requestPreview(socket, 'check_core_updates');
        assert.equal(checkActive.ok, true, checkActive.feedback);
        const unchanged = await activeProtected;
        assert.equal(panelLogs(unchanged, 'error', unchanged.data.server.core.update_check_error).length, 1);
        assert.equal(unchanged.data.server.core.state, 'missing');
        assert.equal(unchanged.data.server.core.version, current.version);
        assert.equal(unchanged.schematic_revision, schematicRevision);
        assert.equal(await readFile(schematicFile(fixture.backend), 'utf8'), original);
        assert.match(await readFile(catalogCalls, 'utf8'), new RegExp(`/git/blobs/${activeEntry.sha}`));
        await assert.rejects(stat(archiveCalls), { code: 'ENOENT' });
        await writeFile(currentCore.program, currentExecutable, { mode: 0o700 });

        await writeFile(listing, JSON.stringify([candidate]));
        const found = nextPreviewSnapshot(socket, frame =>
            !frame.data.server.core.checking_updates && frame.data.server.core.available_versions.includes(selected.version));
        const check = await requestPreview(socket, 'check_core_updates');
        assert.equal(check.ok, true, check.feedback);
        const checked = await found;
        assert.equal(panelLogs(checked, 'success', 'WaterWall update check completed.').length, 1);
        assert.deepEqual(checked.data.server.core.available_versions, [selected.version, '1.6.0']);
        assert.equal(checked.data.server.core.latest_version, selected.version);
        assert.equal(checked.data.server.core.update_available, true);
        assert.equal(checked.schematic, undefined);
        assert.equal(checked.schematic_revision, schematicRevision);
        const targetFile = schematicFile(fixture.backend, selected.version);
        assert.deepEqual(await readFile(targetFile), schematicBytes);
        assert.match(await readFile(catalogCalls, 'utf8'), new RegExp(`/git/blobs/${candidate.sha}`));
        await assert.rejects(stat(archiveCalls), { code: 'ENOENT' });

        const badBlob = 'f'.repeat(40);
        await writeFile(join(blobs, badBlob), schematicBytes.toString().replace('Enable the fixture node.', 'Ignore the fixture node.'));
        await writeFile(listing, JSON.stringify([{ ...candidate, sha: badBlob }]));
        const badCheck = nextPreviewSnapshot(socket, frame =>
            !frame.data.server.core.checking_updates && frame.data.server.core.update_check_error);
        assert.equal((await requestPreview(socket, 'check_core_updates')).ok, true);
        const badResult = await badCheck;
        const badResultError = badResult.data.server.core.update_check_error;
        assert.equal(panelLogs(badResult, 'error', badResultError).length, 1);
        assert.deepEqual(badResult.data.server.core.available_versions, [selected.version, '1.6.0']);
        assert.equal(badResult.data.server.core.latest_version, selected.version);
        assert.equal(badResult.data.server.core.update_available, true);
        assert.deepEqual(await readFile(targetFile), schematicBytes);
        await writeFile(listing, '[]');

        await createCoreConfig(socket, 'Already running', true);
        const running = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'running');
        const start = await requestPreview(socket, 'start_core');
        assert.equal(start.ok, true, start.feedback);
        await running;
        const blocked = await requestPreview(socket, 'update_core', { version: selected.version });
        assert.equal(blocked.ok, false);
        assert.match(blocked.feedback, /Stop WaterWall/);
        await assert.rejects(stat(archiveCalls), { code: 'ENOENT' });
        const stopped = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'stopped');
        assert.equal((await requestPreview(socket, 'stop_core')).ok, true);
        await stopped;

        const stored = JSON.parse(await readFile(fixture.database, 'utf8'));
        await writeFile(archiveFile, 'bad fixture archive bytes\n');
        const failed = nextPreviewSnapshot(socket, frame =>
            frame.data.server.core.update_target === undefined && frame.data.server.core.update_error?.includes('SHA-256'));
        const rejected = await requestPreview(socket, 'update_core', { version: selected.version });
        assert.equal(rejected.ok, true, rejected.feedback);
        assert.equal(rejected.data.server.core.update_target, selected.version);
        const rolledBack = await failed;
        const installError = rolledBack.data.server.core.update_error;
        assert.equal(panelLogs(rolledBack, 'error', installError).length, 1);
        assert.equal(rolledBack.data.server.core.version, '1.6.0');
        assert.equal(rolledBack.schematic_revision, schematicRevision);
        assert.equal(rolledBack.schematic, undefined);
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')), stored);
        await assert.rejects(stat(unzipCalls), { code: 'ENOENT' });
        const selectedProgram = join(coreReleaseDir(fixture.backend, selected.version), 'Waterwall');
        await assert.rejects(stat(selectedProgram), { code: 'ENOENT' });

        const peer = await connectPreview(t, fixture.origin, session);
        await writeFile(archiveFile, payload);
        await writeFile(archiveHold, '');
        const catalogBeforeInstall = await readFile(catalogCalls, 'utf8');
        const switched = nextPreviewSnapshot(socket, frame => frame.data.server.core.version === selected.version);
        const peerSwitched = nextPreviewSnapshot(peer.socket, frame => frame.data.server.core.version === selected.version);
        const requested = await requestPreview(socket, 'update_core', { version: selected.version });
        assert.equal(requested.ok, true, requested.feedback);
        const queued = await requestPreview(socket, 'check_updates');
        assert.equal(queued.ok, true, queued.feedback);
        assert.equal(queued.data.server.core.update_target, selected.version);
        assert.equal(queued.data.server.core.checking_updates, true);
        assert.equal(queued.data.server.panel.checking_updates, true);
        assert.equal(await readFile(catalogCalls, 'utf8'), catalogBeforeInstall);
        const afterQueuedCheck = nextPreviewSnapshot(socket, frame =>
            frame.data.server.core.version === selected.version && !frame.data.server.core.checking_updates &&
            frame.data.server.panel.update_check_generation === 1);
        await unlink(archiveHold);
        const applied = await switched;
        assert.equal(applied.data.server.version, panelVersion);
        assert.equal(applied.data.server.core.state, 'stopped');
        assert.equal(applied.data.server.core.latest_version, selected.version);
        assert.equal(applied.data.server.core.update_available, false);
        assert.equal(applied.data.server.core.update_target, undefined);
        assert.equal(applied.schematic.version, selected.version);
        assert.deepEqual(applied.schematic.nodes.FixtureNextRelease, selected.nodes.FixtureNextRelease);
        assert.ok(applied.schematic_revision > schematicRevision);
        const peerApplied = await peerSwitched;
        assert.equal(peerApplied.schematic.version, selected.version);
        assert.equal(peerApplied.schematic_revision, applied.schematic_revision);
        for (const error of [startupError, badResultError, installError]) {
            assert.equal(panelLogs(peerApplied, 'error', error).length, 1);
        }
        assert.equal(panelLogs(await afterQueuedCheck, 'success', 'WaterWall update check completed.').length, 2);

        const settings = { misc: { workers: 3 } };
        for (const revision of [undefined, schematicRevision]) {
            const stale = await requestPreview(peer.socket, 'apply_core_settings', {
                schematic_revision: revision, settings,
            });
            assert.equal(stale.ok, false);
            assert.match(stale.feedback, /WaterWall changed version/);
            assert.equal(stale.data.server.core.version, selected.version);
            assert.equal(stale.data.core_settings.misc.workers, 2);
        }
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')), { ...stored, waterwall_version: selected.version });
        const savedSettings = await requestPreview(peer.socket, 'apply_core_settings', { settings });
        assert.equal(savedSettings.ok, true, savedSettings.feedback);
        assert.equal(savedSettings.data.core_settings.misc.workers, 3);
        assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).core_settings.misc.workers, 3);
        const theme = await requestPreview(peer.socket, 'apply_theme', { schematic_revision: undefined, theme: 'light' });
        assert.equal(theme.ok, true, theme.feedback);
        assert.equal(theme.data.settings.theme, 'light');
        const expectedUrl = `https://github.com/radkesvat/WaterWall/releases/download/v${selected.version}/${archive}`;
        const calls = (await readFile(archiveCalls, 'utf8')).trimEnd().split('\n');
        assert.deepEqual(calls, [expectedUrl, expectedUrl]);
        assert.equal(await readFile(unzipCalls, 'utf8'), 'Waterwall\n');
        assert.equal(await readFile(selectedProgram, 'utf8'), installedExecutable);
        assert.deepEqual((await readdir(dirname(targetFile))).sort(), ['Waterwall', `waterwall-${selected.version}.json`]);
        assert.equal(await readFile(currentCore.program, 'utf8'), currentExecutable);

        const profile = await createCoreConfig(socket, 'New-release node');
        const { config, revision } = (await requestPreview(socket, 'get_core_config', { id: profile.id })).result;
        assert.deepEqual(config.nodes, []);
        const node = { name: 'New node', type: 'FixtureNextRelease' };
        const save = settings => requestPreview(socket, 'save_core_config', {
            id: profile.id, revision, config: { ...config, nodes: [{ ...node, settings }] },
        });
        const invalid = await save({});
        assert.equal(invalid.ok, false);
        assert.match(invalid.feedback, /Enable the fixture node/);
        const valid = await save({ ready: true });
        assert.equal(valid.ok, true, valid.feedback);
        assert.deepEqual(valid.result.config.nodes, [{ ...node, settings: { ready: true } }]);

        await fixture.stop();
        await fixture.start(env);
        const reopened = await connectPreview(t, fixture.origin, session);
        assert.equal(reopened.initial.server.core.version, selected.version);
        assert.equal(reopened.initial.server.core.state, 'stopped');
        assert.equal(reopened.schematic.version, selected.version);
        assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).waterwall_version, selected.version);
        const omitted = await requestPreview(reopened.socket, 'apply_core_settings', {
            schematic_revision: undefined, settings: { misc: { workers: 4 } },
        });
        assert.equal(omitted.ok, false);
        assert.match(omitted.feedback, /WaterWall changed version/);
        assert.equal(omitted.data.core_settings.misc.workers, 3);
        const currentRevision = await requestPreview(reopened.socket, 'apply_core_settings', {
            settings: { misc: { workers: 4 } },
        });
        assert.equal(currentRevision.ok, true, currentRevision.feedback);
    });

test('attempts first-run installation once and lets an authenticated administrator retry a missing core',
    { skip: !supportedInstaller }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, prepareCore: false });
        const releaseDir = coreReleaseDir(fixture.backend);
        const configDir = join(dirname(fixture.backend), 'configs');
        const tools = join(fixture.cwd, 'fixture tools');
        const calls = join(fixture.cwd, 'curl calls');
        const released = join(fixture.cwd, 'release curl');
        await mkdir(tools);
        await writeFile(join(tools, 'curl'), [
            '#!/bin/sh',
            'for url do :; done',
            'case "$url" in',
            '  "https://api.github.com/repos/radkesvat/wave/contents/backend/schematics?ref=main") printf "[]"; exit 0 ;;',
            'esac',
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
        if (firstFailed.data.server.core.checking_updates) {
            await nextPreviewSnapshot(first.socket, ({ data }) => data.server.core.checking_updates === false);
        }
        assert.equal(panelLogs(firstFailed, 'error', firstFailed.data.server.core.error).length, 1);
        assert.equal((await invoked()).length, 1);
        assert.deepEqual(await readdir(dirname(releaseDir)), ['1.6.0']);
        assert.deepEqual(await readdir(releaseDir), ['waterwall-1.6.0.json']);

        await fixture.stop();
        await unlink(released);
        await fixture.start(env);
        const restarted = await connectPreview(t, fixture.origin, session);
        assert.equal(restarted.initial.server.core.state, 'missing');
        assert.deepEqual(await readdir(releaseDir), ['waterwall-1.6.0.json']);
        await delay(250);
        assert.equal((await invoked()).length, 1, 'Restarting does not reset the first-run attempt.');

        const retry = await requestPreview(restarted.socket, 'install_core');
        assert.equal(retry.ok, true, retry.feedback);
        assert.equal(retry.data.server.core.state, 'installing');
        const secondFailure = failedInstall(restarted.socket);
        await writeFile(released, '');
        const secondFailed = await secondFailure;
        assert.equal(panelLogs(secondFailed, 'error', secondFailed.data.server.core.error).length, 1);
        assert.equal(panelLogs(secondFailed, 'success', 'WaterWall installed.').length, 0);
        assert.equal((await invoked()).length, 2);
        assert.deepEqual(await readdir(releaseDir), ['waterwall-1.6.0.json']);

        const installed = nextPreviewSnapshot(restarted.socket, frame => frame.data.server.core.state === 'stopped');
        const staged = await seedCore(fixture.backend, { executable: true });
        assert.equal(panelLogs(await installed, 'success', 'WaterWall installed.').length, 0);
        const deleted = nextPreviewSnapshot(restarted.socket, frame => frame.data.server.core.state === 'missing');
        await unlink(staged.program);
        await deleted;
        assert.equal((await invoked()).length, 2, 'Deleting the executable leaves installation up to the administrator.');
    });

test('persists the panel theme and leaves saved settings intact on invalid choices', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).theme, undefined);
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket, initial } = await connectPreview(t, fixture.origin, session);
    assert.equal(initial.settings.theme, 'default');
    const original = JSON.parse(await readFile(fixture.database, 'utf8'));

    for (const theme of ['light', 'classic', 'dark', 'default', 'darker']) {
        const applied = await requestPreview(socket, 'apply_theme', { theme });
        assert.equal(applied.ok, true, applied.feedback);
        assert.equal(applied.data.settings.theme, theme);
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')), { ...original, theme });
    }

    const saved = await readFile(fixture.database, 'utf8');
    const savedFile = await stat(fixture.database, { bigint: true });
    for (const input of [{}, { theme: null }, { theme: true }, { theme: 'Light' }, { theme: 'unknown' }]) {
        const rejected = await requestPreview(socket, 'apply_theme', input);
        assert.equal(rejected.ok, false);
        assert.equal(rejected.data.settings.theme, 'darker');
        assert.equal(await readFile(fixture.database, 'utf8'), saved);
        const current = await stat(fixture.database, { bigint: true });
        assert.equal(current.ino, savedFile.ino);
        assert.equal(current.mtimeNs, savedFile.mtimeNs);
    }

    const reopened = await connectPreview(t, fixture.origin, session);
    assert.equal(reopened.initial.settings.theme, 'darker');
    await fixture.stop();
    await fixture.start();
    const restarted = await connectPreview(t, fixture.origin, session);
    assert.equal(restarted.initial.settings.theme, 'darker');
});

test('persists donation tiers and reminds donors 30 days after their latest response', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const start = () => fixture.start({ TZ: 'UTC+12' });
    const hour = 60 * 60 * 1000;
    const reminderInterval = 30 * 24 * hour;
    const utcTimestamp = milliseconds => new Date(milliseconds).toISOString().replace(/\.\d{3}Z$/, 'Z');
    const expectedDonation = (record, due = false) => ({
        ...record,
        reminder_due: due,
        ...(record.answer === 'no' || due ? {} : {
            next_reminder_at: utcTimestamp(Date.parse(record.answered_at) + reminderInterval),
        }),
    });
    await start();
    const session = await fixtureSession(fixture);
    const { socket, initial } = await connectPreview(t, fixture.origin, session);
    assert.deepEqual(initial.settings.donation, { reminder_due: true });
    const original = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.equal(original.donation, undefined);

    const submitDonation = async (socket, answer) => {
        const before = Date.now();
        const saved = await requestPreview(socket, 'apply_donation', { answer });
        const after = Date.now();
        assert.equal(saved.ok, true, saved.feedback);
        const record = { answer, answered_at: saved.data.settings.donation.answered_at };
        assert.match(record.answered_at, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/);
        const recorded = Date.parse(record.answered_at);
        assert.ok(recorded >= Math.floor(before / 1000) * 1000 && recorded <= after,
            'The timestamp uses the current UTC time even when the server is in another time zone.');
        assert.deepEqual(saved.data.settings.donation, expectedDonation(record));
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')), { ...original, donation: record });
        return record;
    };

    let response;
    for (const answer of ['below_5', 'more_than_5', 'no', '1', '3', '5', '10', '15', '30', '50_plus']) {
        response = await submitDonation(socket, answer);
    }

    const contents = await readFile(fixture.database, 'utf8');
    for (const input of [{}, { answer: 1 }, { answer: 'yes' }, { answer: 'NO' }]) {
        const rejected = await requestPreview(socket, 'apply_donation', input);
        assert.equal(rejected.ok, false);
        assert.deepEqual(rejected.data.settings.donation, expectedDonation(response));
        assert.equal(await readFile(fixture.database, 'utf8'), contents);
    }

    await fixture.stop();
    await start();
    const reopened = await connectPreview(t, fixture.origin, session);
    assert.deepEqual(reopened.initial.settings.donation, expectedDonation(response));

    const restartWithDonation = async record => {
        await fixture.stop();
        await writeFile(fixture.database, JSON.stringify({ ...original, donation: record }));
        await start();
        return connectPreview(t, fixture.origin, session);
    };
    const now = Date.now();
    const pending = { ...response, answered_at: utcTimestamp(now - reminderInterval + hour) };
    assert.deepEqual((await restartWithDonation(pending)).initial.settings.donation, expectedDonation(pending));

    const expired = { ...response, answered_at: utcTimestamp(now - reminderInterval) };
    const due = await restartWithDonation(expired);
    assert.deepEqual(due.initial.settings.donation, expectedDonation(expired, true));
    assert.deepEqual((await nextPreviewSnapshot(due.socket)).data.settings.donation,
        expectedDonation(expired, true));
    const updated = await submitDonation(due.socket, '5');
    assert.ok(updated.answered_at > expired.answered_at, 'Changing the donation tier starts a fresh 30 days.');
    await submitDonation(due.socket, 'no');

    for (const [record, reminderDue] of [
        [{ ...expired, answer: 'no' }, false],
        [{ ...pending, answer: 'below_5' }, false],
        [{ ...expired, answer: 'more_than_5' }, true],
    ]) {
        const saved = await restartWithDonation(record);
        assert.deepEqual(saved.initial.settings.donation, expectedDonation(record, reminderDue));
    }

    await fixture.stop();
    const invalid = JSON.stringify({ ...original, donation: { ...response, answered_at: '2025-02-30T12:00:00Z' } });
    await writeFile(fixture.database, invalid);
    await start();
    const malformed = await connectPreview(t, fixture.origin, session);
    assert.deepEqual(malformed.initial.settings.donation, { reminder_due: true });
    assert.equal(await readFile(fixture.database, 'utf8'), invalid);
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

const userApiOperations = ['status', 'list-users', 'get-user', 'create-user', 'update-user', 'change-user-password'];

test('creates disabled configs with protected authentication and API starter nodes', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await connectPreview(t, fixture.origin, session);
    const ports = new Set();
    const credentials = new Set();
    for (const invalid of [
        { add_auth_nodes: 'true' },
        { add_auth_nodes: true, allow_live_user_management: 'true' },
        { allow_live_user_management: true },
    ]) {
        const rejected = await requestPreview(socket, 'create_core_config', { name: 'Invalid starter', ...invalid });
        assert.equal(rejected.ok, false);
        assert.deepEqual(rejected.data.server.core.configs, []);
    }
    const configDir = join(dirname(fixture.backend), 'configs');
    assert.deepEqual(await readdir(configDir), []);

    const monitorFile = join(configDir, 'Legacy monitor', 'config.json');
    let monitorBefore;
    for (const [name, management] of [['Legacy monitor', false], ['Default starter'], ['Explicit management', true]]) {
        const created = await requestPreview(socket, 'create_core_config', {
            name: ` ${name} `, add_auth_nodes: true, allow_live_user_management: management,
        });
        assert.equal(created.ok, true, created.feedback);
        const row = created.data.server.core.configs.find(config => config.name === name);
        assert.ok(row, 'The new config is registered under its trimmed name.');
        assert.equal(row.enabled, false);
        assert.equal(row.active, false);
        assert.deepEqual(row.auth_nodes, ['AuthenticationServer']);
        assert.equal(row.live_users, true);

        const opened = await requestPreview(socket, 'get_core_config', { id: row.id });
        assert.equal(opened.ok, true, opened.feedback);
        assert.equal(opened.result.editable, true);
        const { config } = opened.result;
        const directory = join(configDir, name);
        const file = join(directory, 'config.json');
        const contents = await readFile(file, 'utf8');
        assert.deepEqual(JSON.parse(contents), config);
        if (file === monitorFile) monitorBefore = contents;
        assert.equal((await stat(file)).mode & 0o777, 0o600);
        assert.equal(config.name, name);
        assert.equal(config['config-version'], 1);
        assert.equal(config['core-minimum-version'], 0);
        assert.equal(config.nodes.length, 4);
        assert.deepEqual(Object.fromEntries(config.nodes.map(node => [node.name, node.type])), {
            AuthenticationClient: 'AuthenticationClient', AuthenticationServer: 'AuthenticationServer',
            ApiListener: 'TcpListener', UsersApi: 'APIServer',
        });
        const [authClient, authServer, listener, api] =
            ['AuthenticationClient', 'AuthenticationServer', 'ApiListener', 'UsersApi'].map(nodeName =>
                config.nodes.find(node => node.name === nodeName));
        assert.equal(authClient.next, authServer.name);
        assert.equal(listener.next, api.name);
        assert.equal(authClient.settings.name, 'wave-auth');
        assert.equal(authClient.settings['push-interval-ms'], 500);
        assert.deepEqual(authServer.settings['auth-clients'], [{
            name: authClient.settings.name, secret: authClient.settings.secret,
            'allow-user-pull': true, 'allow-stats-push': true,
        }]);
        assert.equal(api.settings.clients.length, 1);
        const [client] = api.settings.clients;
        assert.equal(client.name, 'wave-users');
        const operations = management === false ? ['list-users'] : userApiOperations;
        assert.deepEqual(client.allow, [{ config: name, node: authServer.name, operations }]);
        for (const credential of [authClient.settings.secret, client.token]) {
            assert.ok(typeof credential === 'string' && /^[0-9a-f]{64}$/.test(credential));
            assert.ok(!credentials.has(credential), 'Each generated credential is independent.');
            assert.ok(!JSON.stringify(created).includes(credential), 'The creation acknowledgement keeps credentials private.');
            credentials.add(credential);
        }

        const databaseName = authServer['panel-settings']['auth-db'];
        assert.match(databaseName, /^auth-[0-9a-f]{32}\.json$/);
        assert.equal(authServer.settings['db-path'], `configs/${name}/${databaseName}`);
        const usersFile = join(directory, databaseName);
        assert.deepEqual(JSON.parse(await readFile(usersFile, 'utf8')), { users: [] });
        assert.equal((await stat(usersFile)).mode & 0o777, 0o600);
        const users = await requestPreview(socket, 'get_config_users', { config_id: row.id, node: authServer.name });
        assert.equal(users.ok, true, users.feedback);
        assert.deepEqual(users.result.users, []);

        const { address, port } = listener.settings;
        assert.equal(address, '127.0.0.1');
        assert.ok(Number.isInteger(port) && port >= 49152 && port <= 65535);
        assert.ok(!ports.has(port), 'Each starter config receives a distinct API port.');
        ports.add(port);
        // Keep each verified port occupied while generating the next config.
        const occupied = createServer();
        t.after(() => occupied.listening && new Promise(resolve => occupied.close(resolve)));
        occupied.listen(port, address);
        await once(occupied, 'listening');
    }
    assert.equal(await readFile(monitorFile, 'utf8'), monitorBefore);
});

async function cachedStarter(t, edit) {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const schematic = parseSchematic(await readFile(bundledSchematicFile, 'utf8'));
    schematic.version = '1.6.1';
    const starter = schematic.config.users_starter;
    const client = starter.template.nodes.find(node => node.type === 'APIServer').settings.clients[0];
    edit(starter, client);
    const selected = schematicFile(fixture.backend, schematic.version);
    await mkdir(dirname(selected), { mode: 0o700 });
    await writeFile(selected, JSON.stringify(schematic), { mode: 0o600 });
    const settings = JSON.parse(await readFile(fixture.database, 'utf8'));
    await writeFile(fixture.database, JSON.stringify({ ...settings, waterwall_version: schematic.version }));

    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket, initial } = await connectPreview(t, fixture.origin, session);
    assert.equal(initial.server.core.version, schematic.version);
    return { socket, configDir: join(dirname(fixture.backend), 'configs') };
}

for (const [kind, hasSlot, operations, pushInterval] of [
    ['list-only slot', true, ['list-users']],
    ['legacy list-only grant', false, ['list-users']],
    ['custom read grant', false, ['list-users', 'get-user'], 10000],
    ['complete management slot', true, userApiOperations, 0],
]) {
    test(`grants scoped management for a cached starter with a ${kind}`, async t => {
        const retained = { config: 'Another config', node: 'AuthenticationServer', operations: ['list-users'] };
        const { socket, configDir } = await cachedStarter(t, (starter, client) => {
            if (!hasSlot) delete starter.management_operations_slot;
            starter.template['config-version'] = 2;
            starter.template['fixture-metadata'] = { members: ['kept', 12] };
            const authClient = starter.template.nodes.find(node => node.type === 'AuthenticationClient');
            if (pushInterval === undefined) delete authClient.settings['push-interval-ms'];
            else authClient.settings['push-interval-ms'] = pushInterval;
            client.allow[0].operations = operations;
            client.allow.push(retained);
        });
        const name = 'Starter template';
        const created = await requestPreview(socket, 'create_core_config', { name, add_auth_nodes: true });
        assert.equal(created.ok, true, created.feedback);
        const [row] = created.data.server.core.configs;
        const opened = await requestPreview(socket, 'get_core_config', { id: row.id });
        assert.equal(opened.ok, true, opened.feedback);
        const saved = JSON.parse(await readFile(join(configDir, name, 'config.json'), 'utf8'));
        assert.equal(saved['config-version'], 2);
        assert.equal(saved['core-minimum-version'], 0);
        assert.deepEqual(saved['fixture-metadata'], { members: ['kept', 12] });
        const authClient = saved.nodes.find(node => node.type === 'AuthenticationClient');
        assert.equal(authClient.settings['push-interval-ms'], pushInterval ?? 500);
        const [client] = saved.nodes.find(node => node.type === 'APIServer').settings.clients;
        assert.deepEqual(client.allow, [
            { config: name, node: 'AuthenticationServer', operations: userApiOperations }, retained,
        ]);
        assert.deepEqual(opened.result.config, saved);
    });
}

test('refuses a full management starter when its cached template only grants access to another config', async t => {
    const foreignGrant = { config: 'Another config', node: 'AuthenticationServer', operations: ['list-users'] };
    const { socket, configDir } = await cachedStarter(t, (starter, client) => {
        delete starter.management_operations_slot;
        starter.slots.name = ['/name'];
        client.allow = [foreignGrant];
    });
    const name = 'Custom template';
    const create = fields => requestPreview(socket, 'create_core_config', { name, add_auth_nodes: true, ...fields });
    const denied = await create({});
    assert.equal(denied.ok, false);
    assert.deepEqual(denied.data.server.core.configs, []);
    assert.deepEqual(await readdir(configDir), []);
    const monitor = await create({ allow_live_user_management: false });
    assert.equal(monitor.ok, true, monitor.feedback);
    const config = JSON.parse(await readFile(join(configDir, name, 'config.json'), 'utf8'));
    assert.deepEqual(config.nodes.find(node => node.type === 'APIServer').settings.clients[0].allow, [foreignGrant]);
});

test('rejects starter nodes when every eligible API port is occupied', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await connectPreview(t, fixture.origin, session);

    const occupied = createServer();
    t.after(() => occupied.listening && new Promise(resolve => occupied.close(resolve)));
    for (let candidate = 65534; candidate > 49152; candidate--) {
        occupied.listen(candidate, '127.0.0.1');
        try {
            await once(occupied, 'listening');
            break;
        } catch (error) {
            if (error.code !== 'EADDRINUSE') throw error;
        }
    }
    assert.ok(occupied.listening, 'Expected a loopback listener within the dynamic port range.');
    const { port } = occupied.address();

    const reservations = await createCoreConfig(socket, 'Port reservations');
    const { config, revision } = (await requestPreview(socket, 'get_core_config', { id: reservations.id })).result;
    const ranges = [[49152, port - 1], [port + 1, 65535]];
    const nodes = ranges.map((range, index) => ({
        name: `Reserved ${index}`, type: 'TcpListener', next: 'Sink', settings: {
            address: '127.0.0.1', 'port-range': range, 'multiport-backend': 'socket',
        },
    }));
    nodes.push({ name: 'Sink', type: 'BlackHole' });
    const saved = await requestPreview(socket, 'save_core_config', {
        id: reservations.id, config: { ...config, nodes }, revision,
    });
    assert.equal(saved.ok, true, saved.feedback);
    const registered = await readFile(fixture.database, 'utf8');
    const configDirectory = join(dirname(fixture.backend), 'configs');

    const name = 'Wait for port';
    const create = () => requestPreview(socket, 'create_core_config', { name, add_auth_nodes: true });
    const rejected = await create();
    assert.equal(rejected.ok, false);
    assert.match(rejected.feedback, /available loopback TCP port/);
    assert.deepEqual(rejected.data.server.core.configs.map(row => row.name), [reservations.name]);
    assert.equal(await readFile(fixture.database, 'utf8'), registered);
    assert.deepEqual(await readdir(configDirectory), [reservations.name]);

    await new Promise(resolve => occupied.close(resolve));
    const created = await create();
    assert.equal(created.ok, true, created.feedback);
    const row = created.data.server.core.configs.find(row => row.name === name);
    assert.ok(row);
    const { result } = await requestPreview(socket, 'get_core_config', { id: row.id });
    const listener = result.config.nodes.find(node => node.type === 'TcpListener');
    assert.deepEqual(listener.settings, { address: '127.0.0.1', port });
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

test('assigns sequential saved user IDs from the current database within the schematic range', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await openPreview(t, fixture.origin, session);
    const { socket: peer } = await openPreview(t, fixture.origin, session);
    const created = await requestPreview(socket, 'create_core_config', { name: 'Saved ID fixture', add_auth_nodes: true });
    assert.equal(created.ok, true, created.feedback);
    const [config] = created.data.server.core.configs;
    const node = config.auth_nodes[0];
    const create = (client, password, id) => requestPreview(client, 'create_config_user', {
        config_id: config.id, node, user: { id, password },
    });
    const added = async (client, password, id) => {
        const reply = await create(client, password, id);
        assert.equal(reply.ok, true, reply.feedback);
        return reply.result.users.at(-1).id;
    };
    const first = await Promise.all([added(socket, 'first'), added(peer, 'second')]);
    assert.deepEqual(first.sort(), ['1', '2']);
    await added(socket, 'large imported ID', '9007199254740993');
    await added(socket, 'smaller ID', '7');
    assert.equal(await added(socket, 'after the largest ID'), '9007199254740994');
    await added(socket, 'maximum ID', '18446744073709551615');
    const exhausted = await create(socket, 'above the maximum');
    assert.equal(exhausted.ok, false);
    assert.match(exhausted.feedback, /no higher account id/i);
    assert.equal(await added(socket, 'explicit unused ID', '3'), '3');

    const opened = await requestPreview(socket, 'get_core_config', { id: config.id });
    const file = opened.result.config.nodes.find(entry => entry.name === node).settings['db-path'];
    await fixture.stop();
    // A custom ID control accepts only whole megabytes within these byte bounds.
    const cached = schematicFile(fixture.backend);
    const schematic = parseSchematic(await readFile(cached, 'utf8'));
    Object.assign(schematic.ui.users.fields.id, { minimum: 1500000, maximum: 3500000, gigabytes: true });
    await writeFile(cached, JSON.stringify(schematic));
    const database = join(dirname(fixture.backend), file);
    await writeFile(database, JSON.stringify({ users: [
        { id: '9007199254740993', password: 'above the custom range' },
        { 'user-id': '1750000', pass: 'legacy fixture account' },
    ] }));
    await fixture.start();
    const restarted = await openPreview(t, fixture.origin, session);
    assert.equal(await added(restarted.socket, 'first whole megabyte above the legacy ID'), '2000000');
    assert.equal(await added(restarted.socket, 'last whole megabyte in range'), '3000000');
    const before = await readFile(database, 'utf8');
    const limited = await create(restarted.socket, 'beyond the custom range');
    assert.equal(limited.ok, false);
    assert.match(limited.feedback, /no higher account id/i);
    assert.equal(await readFile(database, 'utf8'), before);
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

    const readPassword = (sock, node, user_id = record.id) =>
        users(sock, node, 'get_config_user_password', { user_id });
    const selected = await readPassword(socket, 'staff');
    assert.equal(selected.ok, true, selected.feedback);
    assert.deepEqual(selected.result, { password });
    assert.equal(selected.feedback, '');
    assert.equal(selected.feedback_in_logs_only, undefined);
    assert.doesNotMatch(JSON.stringify(selected.data), new RegExp(password));
    const { initial: peer } = await connectPreview(t, fixture.origin, session);
    assert.doesNotMatch(JSON.stringify(peer), new RegExp(password));
    const listed = await users(socket, 'staff');
    assert.deepEqual(listed.result.users, [record]);
    assert.doesNotMatch(JSON.stringify(listed), new RegExp(password));
    const detailScope = {
        source: 'offline', run_id: listed.data.server.core.configs[0].users_access.staff.run_id,
    };
    const detail = user_id => users(socket, 'staff', 'get_config_user', { ...detailScope, user_id });
    const openedDetail = await detail(record.id);
    assert.equal(openedDetail.ok, true, openedDetail.feedback);
    assert.deepEqual(openedDetail.result, { user: record });
    assert.doesNotMatch(JSON.stringify(openedDetail), new RegExp(password));
    assert.equal((await detail(`0${record.id}`)).ok, false);

    const mapped = { id: '18446744073709551615', pass: 'mapped-credential', name: '' };
    for (const [contents, expectedName] of [
        [[mapped], ''], [{ users: [mapped] }, ''], [mapped, ''], [{ 'Name from key': mapped }, 'Name from key'],
    ]) {
        await writeFile(databases[0], JSON.stringify(contents));
        const opened = await detail(mapped.id);
        assert.equal(opened.ok, true, opened.feedback);
        assert.deepEqual(opened.result, { user: { id: mapped.id, name: expectedName, enabled: true } });
    }
    const additional = Array.from({ length: 51 }, (_, index) => ({
        id: String(index + 1), password: `pagination-credential-${index}`,
    }));
    const farAccount = { id: '18446744073709551615', name: 'Élodie Directory', password: 'search-fixture' };
    const greek = { id: '60', name: 'ΝΙΚΟΣ', password: 'search-greek' };
    const german = { id: '61', name: 'Straße', password: 'search-german' };
    const accented = { id: '62', name: 'café', password: 'search-accented' };
    const many = { users: [...stored.users, ...additional, farAccount, greek, german, accented] };
    await writeFile(databases[0], JSON.stringify(many));
    for (const [search, account] of [
        ['éLODIE', farAccount], [farAccount.id, farAccount], ['νικος', greek], ['νικοσ', greek],
        ['STRASSE', german], ['CAFÉ', accented],
    ]) {
        const found = (await users(socket, 'staff', 'get_config_users', { search, page: 99 })).result;
        assert.deepEqual(found, {
            users: [{ id: account.id, name: account.name, enabled: true }],
            editable: true, page: 0, page_count: 1, matched: 1, total: many.users.length,
        });
    }
    for (const search of ['999999', 'cafe']) {
        const missing = (await users(socket, 'staff', 'get_config_users', { search })).result;
        assert.equal(missing.matched, 0);
        assert.equal(missing.total, many.users.length);
    }
    await writeFile(databases[0], JSON.stringify({ users: [...many.users, farAccount] }));
    const duplicate = await users(socket, 'staff', 'get_config_users', { search: 'Alex' });
    assert.match(duplicate.feedback, /IDs must be unique/);
    assert.match((await detail(record.id)).feedback, /IDs must be unique/);
    await writeFile(databases[0], JSON.stringify(stored));
    for (const userId of [null, `0${record.id}`, '18446744073709551616', '2']) {
        const rejected = await readPassword(socket, 'staff', userId);
        assert.equal(rejected.ok, false);
        assert.equal(rejected.result, undefined);
        assert.equal(rejected.feedback_in_logs_only, undefined);
    }
    assert.equal((await readPassword(socket, 'guests')).ok, false);

    const renamed = structuredClone(savedGraph);
    renamed.nodes[0].next = 'directory';
    renamed.nodes[1].name = 'directory';
    const rename = await save(renamed, saved.result.revision);
    assert.equal(rename.ok, true, rename.feedback);
    assert.equal(rename.result.config.nodes[1].settings['db-path'], savedGraph.nodes[1].settings['db-path']);
    assert.equal((await users(socket, 'staff')).ok, false);
    // Model WaterWall-managed history and legacy fields before applying a partial user edit.
    const onDisk = stored.users[0];
    onDisk.pass = onDisk.password;
    delete onDisk.password;
    onDisk.limit.traffic.u = onDisk.limit.traffic.up;
    onDisk.time['first-usage-at-ms'] = '1920000000000';
    onDisk['expires-at-ms'] = '1924992001000';
    onDisk.stats = { traffic: { u: '18446744073709551615', d: '956' }, devices: '2' };
    onDisk.extension = { preserved: true };
    const existing = JSON.stringify(stored);
    await writeFile(databases[0], existing);
    assert.deepEqual((await readPassword(socket, 'directory')).result, { password });
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
        'record-stat-interval-ms': '2147483647',
        limit: { traffic: { up: null, down: '0' } }, time: { 'expire-at-ms': null },
    });
    assert.equal(edited.ok, true, edited.feedback);
    assert.equal(edited.feedback_in_logs_only, true);
    const updated = {
        ...record, name: 'Jordan', notes, enabled: false, 'wireguard-allowed-ips': '',
        'record-stat-interval-ms': '2147483647',
        limit: { ...record.limit, traffic: { total: record.limit.traffic.total, down: '0' } },
        time: {
            'created-at-ms': createdAt, 'first-usage-at-ms': onDisk.time['first-usage-at-ms'],
            'expire-after-first-usage-ms': record.time['expire-after-first-usage-ms'],
        },
        stats: { traffic: { up: onDisk.stats.traffic.u, down: onDisk.stats.traffic.d }, devices: '2' },
    };
    assert.deepEqual(edited.result.users, [updated]);
    const [merged] = JSON.parse(await readFile(databases[0], 'utf8')).users;
    assert.equal(merged.pass, password);
    assert.equal(merged.password, undefined);
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
    const blockedPassword = await readPassword(socket, 'directory');
    assert.equal(blockedPassword.ok, false);
    assert.match(blockedPassword.feedback, /Stop WaterWall/);
    assert.equal(blockedPassword.result, undefined);
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
    assert.doesNotMatch(JSON.stringify(rotated), new RegExp(otherPassword));
    const [storedRotation] = JSON.parse(await readFile(databases[0], 'utf8')).users;
    assert.equal(storedRotation.password, otherPassword);
    assert.equal(storedRotation.pass, undefined);
    assert.deepEqual((await readPassword(restarted.socket, 'directory')).result, { password: otherPassword });
    const currentScope = {
        source: 'offline', run_id: restarted.initial.server.core.configs[0].users_access.directory.run_id,
    };
    const compact = await users(restarted.socket, 'directory', 'edit_config_user', {
        ...currentScope, roster: true, user_id: record.id, user: { name: updated.name },
    });
    assert.equal(compact.ok, true, compact.feedback);
    assert.deepEqual(compact.result, { user_id: record.id });
    const deleted = await users(restarted.socket, 'directory', 'delete_config_user', { user_id: record.id });
    assert.equal(deleted.ok, true, deleted.feedback);
    assert.equal(deleted.feedback_in_logs_only, true);
    assert.deepEqual(deleted.result.users, []);
    assert.deepEqual(JSON.parse(await readFile(databases[0], 'utf8')), { users: [] });

    const neighbor = { id: '42', name: 'Neighbor', password: 'neighbor-fixture-password' };
    await writeFile(databases[0], JSON.stringify({ users: [
        { id: record.id, notes: 'x'.repeat(17 * 1024), password }, neighbor,
    ] }));
    const { run_id } = restarted.initial.server.core.configs[0].users_access.directory;
    const scope = { source: 'offline', run_id };
    const oversize = await users(restarted.socket, 'directory', 'get_config_users', scope);
    assert.deepEqual(oversize.result.users[0], { id: record.id, enabled: true, oversized: true });
    const oversizedDetail = await users(restarted.socket, 'directory', 'get_config_user', {
        ...scope, user_id: record.id,
    });
    assert.equal(oversizedDetail.ok, false);
    assert.match(oversizedDetail.feedback, /too large to edit/);
    const oversizedEdit = await users(restarted.socket, 'directory', 'edit_config_user', {
        ...scope, roster: true, user_id: record.id, user: { enabled: false },
    });
    assert.equal(oversizedEdit.ok, false);
    assert.match(oversizedEdit.feedback, /too large to edit/);
    const removed = await users(restarted.socket, 'directory', 'delete_config_user', { ...scope, user_id: record.id });
    assert.equal(removed.ok, true, removed.feedback);
    const reloaded = await users(restarted.socket, 'directory', 'get_config_users', scope);
    assert.deepEqual(reloaded.result.users, [{ id: neighbor.id, name: neighbor.name, enabled: true }]);
    assert.deepEqual(JSON.parse(await readFile(databases[0], 'utf8')), { users: [neighbor] });
});

test('rejects stale offline user requests after the database binding changes',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false });
        await seedCore(fixture.backend);
        await fixture.start();
        const session = await fixtureSession(fixture);
        const { socket: first } = await connectPreview(t, fixture.origin, session);
        const { socket: second } = await connectPreview(t, fixture.origin, session);
        const profile = await createCoreConfig(first, 'Database scope fixture');
        const configDir = join(dirname(fixture.backend), 'configs', profile.name);
        const file = join(configDir, 'config.json');
        const openGraph = () => requestPreview(second, 'get_core_config', { id: profile.id });
        const saveGraph = (config, revision) => requestPreview(second, 'save_core_config', {
            id: profile.id, config, revision,
        });
        const staffAccess = reply => reply.data.server.core.configs.find(row => row.id === profile.id).users_access?.staff;
        const users = (action, run_id, fields = {}) => requestPreview(first, action, {
            config_id: profile.id, node: 'staff', source: 'offline', run_id, ...fields,
        });
        const { result: original } = await openGraph();
        const configured = await saveGraph({ ...original.config, nodes: ['staff', 'guests'].map(name => ({
            name, type: 'AuthenticationServer',
        })) }, original.revision);
        assert.equal(configured.ok, true, configured.feedback);
        const { config, revision } = configured.result;
        const [staff, guests] = config.nodes.map(node => join(dirname(fixture.backend), node.settings['db-path']));
        const id = '41';
        await writeFile(staff, JSON.stringify({ users: [{ id, name: 'Staff account', password: 'staff-fixture-secret' }] }));
        await writeFile(guests, JSON.stringify({ users: [{ id, name: 'Guest account', password: 'guest-fixture-secret' }] }));
        const old = staffAccess(configured);
        assert.ok(old.database_generation);
        assert.doesNotMatch(JSON.stringify(old), /auth-[a-f0-9]{32}\.json/);
        const opened = await users('get_config_users', old.run_id);
        assert.equal(opened.result.users[0].name, 'Staff account');

        const unrelated = await saveGraph({ ...config, author: 'Graph fixture' }, revision);
        assert.equal(unrelated.ok, true, unrelated.feedback);
        assert.deepEqual(staffAccess(unrelated), old);
        const ordinary = await users('edit_config_user', old.run_id, { user_id: id, user: { notes: 'Same database' } });
        assert.equal(ordinary.ok, true, ordinary.feedback);
        assert.deepEqual(staffAccess(ordinary), old);
        const staffBytes = await readFile(staff);
        const guestBytes = await readFile(guests);

        const reassigned = await saveGraph({ ...unrelated.result.config, nodes: [{ ...config.nodes[1], name: 'staff' }] },
            unrelated.result.revision);
        assert.equal(reassigned.ok, true, reassigned.feedback);
        const replacement = staffAccess(reassigned);
        assert.notEqual(replacement.run_id, old.run_id);
        assert.notEqual(replacement.database_generation, old.database_generation);
        for (const [action, fields] of [
            ['edit_config_user', { user_id: id, user: { name: 'Wrong account' } }],
            ['delete_config_user', { user_id: id }],
            ['create_config_user', { user: { id: '42', password: 'unused-create-secret' } }],
            ['get_config_user_password', { user_id: id }],
            ['get_config_user', { user_id: id }],
        ]) {
            const result = await users(action, old.run_id, fields);
            assert.equal(result.ok, false, action);
            assert.match(result.feedback, /reopen the account/i);
            assert.equal(result.result, undefined);
            assert.deepEqual(staffAccess(result), replacement);
            assert.deepEqual(await readFile(staff), staffBytes);
            assert.deepEqual(await readFile(guests), guestBytes);
        }
        const reopened = await users('get_config_users', replacement.run_id);
        assert.equal(reopened.result.users[0].name, 'Guest account');
        const credential = await users('get_config_user_password', replacement.run_id, { user_id: id });
        assert.equal(credential.result.password, 'guest-fixture-secret');
        const detail = await users('get_config_user', replacement.run_id, { user_id: id });
        assert.deepEqual(detail.result, { user: { id, enabled: true, name: 'Guest account' } });

        const external = { ...reassigned.result.config, nodes: [config.nodes[0]] };
        await writeFile(file, JSON.stringify(external));
        const stale = await users('edit_config_user', replacement.run_id, { user_id: id, user: { enabled: false } });
        assert.equal(stale.ok, false);
        assert.match(stale.feedback, /reopen the account/i);
        assert.notEqual(staffAccess(stale).database_generation, replacement.database_generation);
        assert.notEqual(staffAccess(stale).database_generation, old.database_generation);
        assert.deepEqual(await readFile(staff), staffBytes);
        assert.deepEqual(await readFile(guests), guestBytes);
        const fresh = await users('get_config_users', staffAccess(stale).run_id);
        assert.equal(fresh.result.users[0].name, 'Staff account');

        await writeFile(file, JSON.stringify({ ...external, nodes: [] }));
        assert.equal(staffAccess(await openGraph()), undefined);
        await writeFile(file, JSON.stringify(external));
        const recreated = staffAccess(await openGraph());
        assert.notEqual(recreated.database_generation, staffAccess(fresh).database_generation);
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

function managementWaterWallFixture(configName) {
    const { appendFileSync, readFileSync, writeFileSync } = require('node:fs');
    const { createServer } = require('node:net');
    const settings = JSON.parse(readFileSync(0, 'utf8'));
    const config = settings.configs.map(path => JSON.parse(readFileSync(path, 'utf8')))
        .find(entry => entry.name === configName);
    const listener = config.nodes.find(node => node.name === 'listener');
    const id = '18446744073709551615';
    const token = '12'.repeat(24);
    const cursor = '34'.repeat(32);
    const revisions = { config_revision: '3', stats_revision: '4', edit_token: token, accounting_time_ms: id };
    const times = { expire_at_ms: '0', expire_after_first_usage_ms: id, first_usage_at_ms: '0' };
    const traffic = { u: id, d: '0', total: '0' };
    const base = { id, gid: '0', enabled: true, usage: { u: id, d: '0' } };
    const summary = { ...base, name_preview: 'Élodie', name_preview_state: 'truncated', traffic_limit: traffic,
        time: times, password: 'secret-from-list' };
    const user = { ...base, name: 'Élodie Directory', email: '', notes: '', wireguard_allowed_ips: '',
        record_stat_interval_ms: '120000', time: { ...times, created_at_ms: '1' },
        limit: { traffic, bandwidth: { u: '0', d: '1' }, ips: '2', devices: '3', cons_in: '4', cons_out: '5' },
        password: 'secret-from-detail' };
    const server = createServer(connection => {
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
            appendFileSync('management-requests.jsonl', JSON.stringify({ ...request, body }) + '\n', { mode: 0o600 });
            const envelope = { id: request.id, error: null, result: null };
            let result;
            switch (request.operation) {
            case 'status':
                result = { ...revisions, database_loaded: true, database_dirty: false, primary_checkpoint_valid: true,
                    user_count: id, configured_producer_count: '1', session_record_count: '2',
                    primary_save: { last_result: 'not_attempted', last_attempt_age_ms: null, last_success_age_ms: null },
                    audit: { enabled: false, healthy: true, queued_records: '0', written_records: '0',
                        dropped_records: '0', suppressed_rejections: '0', write_failures: '0', last_error: null } };
                break;
            case 'list-users':
                result = body.cursor === cursor ? { error: { code: 'stale_cursor' } }
                    : { ...revisions, total_users: '2', users: [summary], next_cursor: cursor };
                break;
            case 'get-user':
                result = body.id === '1' ? { error: { code: 'not_found' } } : { ...revisions, user };
                break;
            default:
                result = { id: body.id, config_revision: '5', edit_token: token };
                if (body.password === 'lost-create-ack') return connection.destroy();
                if (body.password === 'gateway-denied') {
                    result = null;
                    envelope.error = { code: 'forbidden', message: 'secret-from-gateway' };
                }
                if (body.password === 'wrong-ack') result.id = '1';
                if (body.patch?.name === 'conflict') result = { error: { code: 'conflict' } };
                if (body.patch?.name === 'stop-during-write') {
                    writeFileSync('management-write-pending', 'pending');
                    return;
                }
                if (body.patch?.name === 'oversized-response') {
                    const header = Buffer.alloc(4);
                    header.writeUInt32BE(1024 * 1024);
                    return connection.end(header);
                }
                if (body.patch?.name === 'wrong-correlation') envelope.id += '-wrong';
                break;
            }
            if (result) envelope.result = { code: result.error ? 1 : 0, content_type: 'application/json',
                body_encoding: 'utf8', body: JSON.stringify(result) };
            const payload = Buffer.from(JSON.stringify(envelope));
            const header = Buffer.alloc(4);
            header.writeUInt32BE(payload.length);
            connection.write(header.subarray(0, 2));
            setImmediate(() => connection.write(Buffer.concat([header.subarray(2), payload.subarray(0, 15)]),
                () => connection.end(payload.subarray(15))));
        });
    });
    server.listen(listener.settings['port-range'][0], '127.0.0.1',
        () => writeFileSync('management-ready', String(process.pid)));
}

test('scopes and validates each WaterWall management operation through the workspace',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false });
        const core = await seedCore(fixture.backend);
        const configName = 'Management logical profile';
        await writeFile(core.program, `#!/usr/bin/env node\n(${managementWaterWallFixture})(${JSON.stringify(configName)});\n`,
            { mode: 0o700 });
        const reserved = createServer();
        reserved.listen(0, '127.0.0.1');
        await once(reserved, 'listening');
        const { port } = reserved.address();
        await new Promise(resolve => reserved.close(resolve));
        const fixtureDir = dirname(fixture.backend);
        const requestsFile = join(fixtureDir, 'management-requests.jsonl');
        const requests = async () => (await readFile(requestsFile, 'utf8')).trim().split('\n').map(JSON.parse);
        await fixture.start();
        const session = await fixtureSession(fixture);
        const { socket } = await connectPreview(t, fixture.origin, session);
        const { socket: observer } = await connectPreview(t, fixture.origin, session);
        const profile = await createCoreConfig(socket, 'Managed files', true);
        const standby = await createCoreConfig(socket, 'Standby');
        const saveGraph = async (id, config) => {
            const { revision } = (await requestPreview(socket, 'get_core_config', { id })).result;
            const result = await requestPreview(socket, 'save_core_config', { id, config, revision });
            assert.equal(result.ok, true, result.feedback);
            return result.result.config;
        };
        const listener = (name, next) => ({ name, type: 'TcpListener', ...(next ? { next } : {}),
            settings: { address: '127.0.0.1', 'port-range': [port, port] } });
        const grant = (name, key, node, operations, config = configName) => ({ name, token: key.repeat(32),
            allow: [{ config, node, operations }] });
        const clients = [
            grant('wrong logical selector', 'a1', 'staff', ['status', 'update-user'], profile.name),
            grant('status client', 'a2', 'health', ['status']),
            grant('read-only client', 'a3', 'readers', ['list-users']),
            grant('staff reader', 'a4', 'staff', ['list-users', 'get-user']),
            grant('staff editor', 'a5', 'staff', ['update-user']),
            grant('staff creator', 'a6', 'staff', ['create-user']),
            grant('staff credentials', 'a7', 'staff', ['change-user-password']),
        ];
        const configured = await saveGraph(profile.id, { name: configName, nodes: [
            ...['staff', 'health', 'readers', 'ungranted'].map(name => ({ name, type: 'AuthenticationServer' })),
            listener('listener', 'api'), { name: 'api', type: 'APIServer', settings: { clients } },
        ] });
        await saveGraph(standby.id, { name: 'Standby', nodes: [{ name: 'staff', type: 'AuthenticationServer' },
            listener('overlapping listener')] });
        const db = join(fixtureDir, configured.nodes[0].settings['db-path']);
        const initialDb = await readFile(db);
        const access = (frame, id = profile.id) => frame.data.server.core.configs.find(row => row.id === id).users_access;
        const withState = (sock, state) => nextPreviewSnapshot(sock, frame => frame.data.server.core.state === state);
        const start = async () => {
            const running = withState(observer, 'running');
            assert.equal((await requestPreview(socket, 'start_core')).ok, true);
            return running;
        };
        const running = await start();
        await waitForFile(join(fixtureDir, 'management-ready'));
        const nodeAccess = access(running);
        const scope = (node, id = profile.id) => ({ config_id: id, node, source: 'live', run_id: access(running, id)[node].run_id });
        const live = (action, fields = {}, node = 'staff', scoped = scope(node)) =>
            requestPreview(socket, action, { ...scoped, ...fields });
        assert.deepEqual(nodeAccess.health.operations, ['status']);
        assert.deepEqual(nodeAccess.readers.operations, ['list-users']);
        assert.deepEqual(nodeAccess.staff.operations,
            ['list-users', 'get-user', 'create-user', 'update-user', 'change-user-password']);
        assert.equal(nodeAccess.ungranted.source, 'blocked');
        assert.match(nodeAccess.ungranted.reason, /grant/i);
        assert.match(access(running, standby.id).staff.reason, /inactive/i);
        const health = await live('get_live_config_user_status', {}, 'health');
        assert.equal(health.ok, true, health.feedback);
        assert.equal(health.result.status.user_count, '18446744073709551615');
        assert.equal(health.result.status.session_record_count, '2');
        assert.equal(health.result.status.audit.healthy, true);
        assert.equal(health.result.status.edit_token, undefined);
        assert.equal((await live('get_live_config_users', {}, 'health')).ok, false);
        assert.equal((await live('get_live_config_user_status')).ok, false);
        const readOnly = await live('get_live_config_users', {}, 'readers');
        assert.equal(readOnly.ok, true, readOnly.feedback);
        assert.equal(readOnly.result.users[0].name_preview_state, 'truncated');
        const id = readOnly.result.users[0].id;
        assert.equal(id, '18446744073709551615');
        assert.deepEqual(readOnly.result.users[0].stats.traffic, { up: id, down: '0' });
        assert.equal(readOnly.result.total, '2');
        const stale = await live('get_live_config_users', { cursor: readOnly.result.next_cursor }, 'readers');
        assert.equal(stale.result.reset_cursor, true);
        const denied = await live('edit_live_config_user', { user_id: id, user: { enabled: false } }, 'readers');
        assert.equal(denied.result.outcome, 'not_sent');
        const sibling = await live('create_live_config_user', { node: 'readers', user: { password: 'no-sibling-write' } });
        assert.equal(sibling.result.outcome, 'not_sent');
        assert.equal((await live('get_live_config_user', { user_id: '1' })).result.code, 'not_found');
        const wrongId = await live('get_live_config_user', { user_id: '2' });
        assert.equal(wrongId.ok, false);
        const detail = await live('get_live_config_user', { user_id: id });
        assert.equal(detail.ok, true, detail.feedback);
        const if_match = detail.result.if_match;
        assert.match(if_match, /^[0-9a-f]{48}$/);
        assert.equal(detail.result.user.limit.traffic.up, id);
        assert.equal(detail.result.user.limit['connections-in'], '4');
        assert.equal(detail.result.user.time['expire-after-first-usage-ms'], id);
        const create = (user, fields = {}) => live('create_live_config_user', { user, ...fields });
        const edit = user => live('edit_live_config_user', { user_id: id, if_match, user });
        const change = password => live('change_live_config_user_password', { user_id: id, if_match, password });
        const created = await create({ id: '43', password: 'initial-password-fixture', gid: id, enabled: false });
        assert.equal(created.result.outcome, 'applied');
        assert.equal(created.result.user_id, '43');
        assert.match(created.feedback, /running WaterWall core/);
        assert.equal((await create({ password: 'top-level-id-fixture' }, { user_id: '44' })).result.user_id, '44');
        assert.equal((await create({ id: '45', password: 'matching-id-fixture' }, { user_id: '45' })).result.user_id, '45');
        const createCount = async () => (await requests()).filter(request => request.operation === 'create-user').length;
        assert.equal(await createCount(), 3);
        for (const [fields, nested] of [
            [{}, {}], [{ user_id: '' }, { id: '46' }], [{ user_id: '46' }, { id: '' }],
            [{ user_id: '46' }, { id: '47' }], [{ user_id: 46 }, { id: '46' }],
            [{}, { id: '0' }], [{}, { id: '046' }], [{}, { id: '18446744073709551616' }],
        ]) {
            const invalid = await create({ ...nested, password: 'unused-id-fixture' }, fields);
            assert.equal(invalid.ok, false);
            assert.equal(invalid.result.outcome, 'not_sent');
        }
        assert.equal(await createCount(), 3, 'Invalid IDs never reach the WaterWall fixture.');
        assert.equal((await edit({ enabled: false })).result.outcome, 'applied');
        const multi = await edit({ limit: { traffic: { up: id, down: null }, 'connections-out': '2' },
            'wireguard-allowed-ips': '192.0.2.1/32', 'record-stat-interval-ms': null });
        assert.equal(multi.result.outcome, 'applied');
        const acceptedLarge = '\\"'.repeat(8000);
        assert.equal((await edit({ notes: acceptedLarge })).result.outcome, 'applied');
        const sentBeforeLarge = (await requests()).length;
        assert.equal((await edit({ notes: '\\"'.repeat(8250) })).result.outcome, 'not_sent');
        assert.equal((await edit({ stats: { traffic: { up: '0' } } })).result.outcome, 'not_sent');
        assert.equal((await requests()).length, sentBeforeLarge);
        assert.equal((await edit({ name: 'conflict' })).result.outcome, 'conflict');
        for (const name of ['oversized-response', 'wrong-correlation']) {
            assert.equal((await edit({ name })).result.outcome, 'unknown');
        }
        const lost = await create({ password: 'lost-create-ack' }, { user_id: '46' });
        assert.equal(lost.result.outcome, 'unknown');
        assert.equal(lost.result.user_id, '46');
        assert.equal((await change('gateway-denied')).result.outcome, 'not_sent');
        assert.equal((await change('wrong-ack')).result.outcome, 'unknown');
        const changed = await change('rotated-password-fixture');
        assert.equal(changed.result.outcome, 'applied');
        const captured = await requests();
        assert.deepEqual(captured[0].body, {});
        assert.deepEqual(captured[0].selector, { config: configName, node: 'health' });
        assert.equal(captured[0].auth.client, 'status client');
        assert.equal(captured[1].auth.client, 'read-only client');
        assert.equal(captured.find(request => request.operation === 'get-user').auth.client, 'staff reader');
        const createdWire = captured.find(request => request.operation === 'create-user');
        assert.equal(createdWire.auth.client, 'staff creator');
        assert.deepEqual(createdWire.body, { id: '43', password: 'initial-password-fixture', config: { gid: id, enabled: false } });
        const edits = captured.filter(request => request.operation === 'update-user');
        assert.equal(edits[0].auth.client, 'staff editor');
        assert.deepEqual(edits[0].body, { id, if_match, patch: { enabled: false } });
        assert.deepEqual(edits[1].body, { id, if_match, patch: {
            limit: { traffic: { u: id, d: '0' }, cons_out: '2' }, wireguard_allowed_ips: '192.0.2.1/32',
            record_stat_interval_ms: '120000',
        } });
        assert.equal(edits[2].body.patch.notes, acceptedLarge);
        assert.equal(captured.filter(request => request.body.password === 'lost-create-ack').length, 1);
        assert.equal(captured.find(request => request.body.password === 'lost-create-ack').body.id, lost.result.user_id);
        assert.equal(captured.at(-1).auth.client, 'staff credentials');
        assert.deepEqual(captured.at(-1).body, { id, if_match, password: 'rotated-password-fixture' });
        for (const reply of [running, health, readOnly, detail, created, multi, lost, changed]) {
            assert.doesNotMatch(JSON.stringify(reply), /secret-from|password-fixture|lost-create-ack/);
            for (const client of clients) assert.doesNotMatch(JSON.stringify(reply), new RegExp(client.token));
        }
        assert.deepEqual(await readFile(db), initialDb);

        const pending = edit({ name: 'stop-during-write' });
        await waitForFile(join(fixtureDir, 'management-write-pending'));
        const stopped = withState(observer, 'stopped');
        assert.equal((await requestPreview(observer, 'stop_core')).ok, true);
        assert.equal((await pending).result.outcome, 'unknown');
        await stopped;
        assert.equal((await requestPreview(socket, 'set_core_config_enabled', { id: standby.id, enabled: true })).ok, true);
        const second = await start();
        assert.notEqual(access(second).staff.run_id, nodeAccess.staff.run_id);
        assert.match(access(second).staff.reason, /dedicated listener port/);
        const beforeRetry = (await requests()).length;
        const oldRun = await create({ password: 'do-not-retry' });
        assert.equal(oldRun.result.outcome, 'not_sent');
        assert.match(oldRun.feedback, /source changed/i);
        const ambiguous = await live('get_live_config_users', { run_id: access(second).staff.run_id });
        assert.equal(ambiguous.ok, false);
        assert.match(ambiguous.feedback, /dedicated listener port/);
        assert.equal((await requests()).length, beforeRetry);
        assert.deepEqual(await readFile(db), initialDb);
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
        assert.deepEqual(JSON.parse(await readFile(configPath(restoredName), 'utf8')), {
            name: restoredName, 'config-version': 1, 'core-minimum-version': 0, nodes: [],
        });
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

test('validates choice controls and offline user mappings in release schematics', async () => {
    const source = await readFile(bundledSchematicFile, 'utf8');
    const schematic = parseSchematic(source);
    const check = path => run(backend, ['--check-schematic', path], { cwd, timeout });
    assert.equal((await check(bundledSchematicFile)).stdout, 'WaterWall schematic is valid.\n');
    for (const type of ['ConnectionToPackets', 'PacketsToConnection', 'PacketsToStream',
        'StreamToPackets', 'WireGuardClient', 'WireGuardServer']) {
        assert.equal(schematic.nodes[type].ports.layer, 'kNodeLayer3 | kNodeLayer4', type);
    }
    for (const type of ['UdpListener', 'UdpStatelessSocket']) {
        assert.equal(schematic.nodes[type].ports.next_layer, 'kNodeLayer3 | kNodeLayer4Datagram', type);
    }

    const directory = await mkdtemp(join(cwd, 'check-schematic-'));
    const candidate = join(directory, 'waterwall-1.6.0.json');
    async function writeCandidate(edit) {
        const document = parseSchematic(source);
        edit(document);
        await writeFile(candidate, JSON.stringify(document));
    }
    async function rejects(edit, diagnostic) {
        await writeCandidate(edit);
        await assert.rejects(check(candidate), { stderr: diagnostic, stdout: '' });
    }
    const choice = { key: 'fixture-choice', label: 'Fixture choice', kind: 'choice' };
    await rejects(document => document.nodes.Template.fields.push(choice), /choice field.*string option/);
    const nestedChoice = choices => document => document.nodes.Template.fields.push({
        key: 'fixture-object', label: 'Fixture object', kind: 'object', children: [{ ...choice, choices }],
    });
    await rejects(nestedChoice([]), /choice field.*string option/);
    await rejects(nestedChoice([1]), /Expected a string/);
    await rejects(document => delete document.ui.users.fields.password, /requires ID and password controls/);
    await rejects(document => { document.ui.users.fields.id.key = 'user-id'; }, /mapping for role id/);
    await rejects(document => { document.ui.users.fields.id.path = '/gid'; }, /mapping for role id/);
    await rejects(document => { document.ui.users.fields.enabled.key = 'name'; }, /mapping for role enabled/);
    await rejects(document => { document.ui.users.access_fields[0].path = '/enabled'; }, /editable.*\/enabled/);
    await rejects(document => { document.ui.users.limit_groups[0].fields[0].path = '/stats/traffic/up'; },
        /editable.*\/stats\/traffic\/up/);
    const starter = document => document.config.users_starter;
    const managementSlot = /starter management operations slot/;
    await rejects(document => { starter(document).management_operations_slot = null; }, managementSlot);
    await rejects(document => { starter(document).management_operations_slot = '/nodes/3/settings/clients/0/token'; },
        managementSlot);
    await rejects(document => {
        starter(document).template.nodes[3].settings.clients[0].allow[0].node = 'AuthenticationClient';
    }, managementSlot);
    await rejects(document => {
        starter(document).template.nodes[3].settings.clients[0].allow[0].operations = ['list-users', 'unsupported'];
    }, managementSlot);
    await rejects(document => {
        const { allow } = starter(document).template.nodes[3].settings.clients[0];
        allow.push({ ...allow[0], config: 'Another config' });
        starter(document).management_operations_slot = '/nodes/3/settings/clients/0/allow/1/operations';
    }, managementSlot);

    const interval = document => document.ui.users.access_fields.find(
        field => field.path === '/record-stat-interval-ms');
    await rejects(document => Object.assign(interval(document), { minimum: 2147483648, maximum: 2147483650 }),
        /bounds.*\/record-stat-interval-ms/);
    await rejects(document => {
        interval(document).minimum = 2147483648;
        delete interval(document).maximum;
    }, /bounds.*\/record-stat-interval-ms/);
    await rejects(document => { document.ui.users.fields.id.minimum = 0; }, /bounds.*\/id/);
    await rejects(document => {
        delete document.ui.users.fields.id.minimum;
        document.ui.users.fields.id.maximum = 0;
    }, /bounds.*\/id/);
    await rejects(document => Object.assign(interval(document), { minimum: 50, maximum: 49 }),
        /bounds.*\/record-stat-interval-ms/);
    await rejects(document => { document.ui.users.fields.group.minimum = -1; }, /bounds.*\/gid/);
    for (const bounds of [{}, { minimum: 100 }, { maximum: 200 }, { minimum: 100, maximum: 200 }]) {
        await writeCandidate(document => {
            delete interval(document).maximum;
            Object.assign(interval(document), bounds);
            delete document.ui.users.fields.id.minimum;
            document.ui.users.fields.id.maximum = JSON.rawJSON('18446744073709551615');
        });
        assert.equal((await check(candidate)).stdout, 'WaterWall schematic is valid.\n');
    }

    const uploadQuota = document => document.ui.users.limit_groups[0].fields.find(
        field => field.path === '/limit/traffic/up');
    const noMegabyte = /\/limit\/traffic\/up.*no whole megabyte/;
    await rejects(document => Object.assign(uploadQuota(document), { minimum: 1, maximum: 999999 }), noMegabyte);
    await rejects(document => { uploadQuota(document).minimum = JSON.rawJSON('18446744073709000001'); }, noMegabyte);
    for (const bounds of [
        { minimum: 1000000, maximum: 2000000 },
        { minimum: 500000, maximum: 2500000 },
        { minimum: JSON.rawJSON('18446744073709000000') },
        { minimum: 1, maximum: 999999, gigabytes: false },
    ]) {
        await writeCandidate(document => Object.assign(uploadQuota(document), bounds));
        assert.equal((await check(candidate)).stdout, 'WaterWall schematic is valid.\n');
    }

    await writeCandidate(document => {
        const { id, password } = document.ui.users.fields;
        document.ui.users.fields = { id, password };
        delete document.ui.users.access_fields;
        delete document.ui.users.limit_groups;
    });
    assert.equal((await check(candidate)).stdout, 'WaterWall schematic is valid.\n');
    await writeCandidate(document => {
        document.runtime.users_protocol = 'none';
        delete document.config.users_starter;
        delete document.config.live_users;
        delete document.ui.users;
        for (const node of Object.values(document.nodes)) {
            delete node.user_database;
        }
    });
    assert.equal((await check(candidate)).stdout, 'WaterWall schematic is valid.\n');
});

async function isolatedBackend(name, settings = {}) {
    const executable = await copyBackend(backend, cwd, name);
    const db = join(dirname(executable), 'wave-db.json');
    await seedCore(executable);
    await writeFile(db, JSON.stringify({ waterwall_install_attempted: true, ...settings }), { mode: 0o600 });
    return { executable, db };
}

test('rejects startup when the selected schematic is missing or invalid', async () => {
    const { executable, db } = await isolatedBackend('invalid-schematic', {
        listen: { host: '127.0.0.1', port: 0 },
        waterwall_version: '1.6.1',
    });
    const site = ['--site-dir', siteDir];
    const legacy = join(dirname(executable), 'schematics');
    await mkdir(legacy, { mode: 0o755 });
    await chmod(legacy, 0o755);
    await writeWithMode(join(legacy, 'waterwall-1.6.1.json'), '{}');
    await expectStartupFailure(site, /waterwall-1\.6\.1\.json.*Expected a regular schematic file/, executable);
    const settings = await readFile(db, 'utf8');

    const selected = schematicFile(executable, '1.6.1');
    assert.deepEqual(await readdir(dirname(selected)), []);
    const bundled = await readFile(bundledSchematicFile, 'utf8');
    const valid = bundled.replace('"version": "1.6.0"', '"version": "1.6.1"');
    const legacyCandidate = join(legacy, 'waterwall-1.6.1.json');
    await writeFile(legacyCandidate, valid);
    await symlink(legacyCandidate, selected);
    await expectStartupFailure(site, /waterwall-1\.6\.1\.json.*Expected a regular schematic file/, executable);
    await unlink(selected);
    await writeWithMode(selected, valid.replace('"schema_version": 1', '"schema_version": 2'));
    await expectStartupFailure(site, /waterwall-1\.6\.1\.json.*Unsupported schematic format/, executable);
    const check = await run(executable, ['--check-schematic', bundledSchematicFile], { cwd, timeout });
    assert.equal(check.stdout, 'WaterWall schematic is valid.\n');
    assert.equal(await readFile(db, 'utf8'), settings);
    assert.deepEqual(await readdir(dirname(selected)), ['waterwall-1.6.1.json']);

    const unpinned = JSON.parse(settings);
    delete unpinned.waterwall_version;
    await writeFile(db, JSON.stringify(unpinned));
    const cachedDefault = schematicFile(executable);
    const invalidDefault = bundled.replace('"schema_version": 1', '"schema_version": 2');
    await writeWithMode(cachedDefault, invalidDefault);
    await expectStartupFailure(site, /waterwall-1\.6\.0\.json.*Unsupported schematic format/, executable);
    assert.deepEqual(JSON.parse(await readFile(db, 'utf8')), unpinned);
    assert.equal(await readFile(cachedDefault, 'utf8'), invalidDefault);
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
