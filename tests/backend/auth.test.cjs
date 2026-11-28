const assert = require('node:assert/strict');
const { createHmac, randomBytes } = require('node:crypto');
const { once } = require('node:events');
const { readFile, unlink, writeFile } = require('node:fs/promises');
const http = require('node:http');
const https = require('node:https');
const { join } = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const { test } = require('node:test');
const { authFixture, seedCore, timeout } = require('./auth_fixture.cjs');
const { tlsFixture } = require('./tls_fixture.cjs');

const cookiePrefix = '__Host-wave_session=';
const secondsPerDay = 86400;
const expirySeconds = 3 * secondsPerDay;

function client(fixture, basePath = fixture.basePath ?? '') {
    const origin = new URL(fixture.origin);
    const transport = origin.protocol === 'https:' ? https : http;

    async function request(path, { method = 'GET', headers = {}, data, ca = fixture.ca,
        hostname = origin.hostname } = {}) {
        const body = data === undefined ? undefined : JSON.stringify(data);
        const url = new URL(basePath + path, origin);
        url.hostname = hostname;
        return new Promise((resolve, reject) => {
            const req = transport.request(url, {
                method, headers, ca, lookup: fixture.lookup, servername: hostname, agent: false,
                signal: AbortSignal.timeout(timeout),
            }, async response => {
                try {
                    const chunks = [];
                    for await (const chunk of response) chunks.push(chunk);
                    resolve({ status: response.statusCode, headers: response.headers, body: Buffer.concat(chunks).toString() });
                } catch (error) {
                    reject(error);
                }
            });
            req.on('error', reject);
            req.end(body);
        });
    }

    function post(path, data, headers = {}) {
        return request(`/api/auth/${path}`, {
            method: 'POST', data,
            headers: { Origin: fixture.origin, 'Content-Type': 'application/json', 'X-Wave-CSRF': '1', ...headers },
        });
    }

    function upgrade(headers, path = '/ws/panel') {
        return new Promise((resolve, reject) => {
            const req = transport.request(new URL(basePath + path, origin), {
                ca: fixture.ca, lookup: fixture.lookup, servername: origin.hostname, agent: false,
                headers: {
                    Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13',
                    'Sec-WebSocket-Key': randomBytes(16).toString('base64'), ...headers,
                },
            });
            req.setTimeout(timeout, () => req.destroy(new Error('WebSocket upgrade timed out')));
            req.on('error', reject);
            req.on('response', response => {
                response.resume();
                resolve({ status: response.statusCode });
            });
            req.on('upgrade', (response, socket, head) => {
                socket.setTimeout(timeout, () => socket.destroy(new Error('WebSocket response timed out')));
                resolve({ status: response.statusCode, socket, frame: frameReader(socket, head) });
            });
            req.end();
        });
    }

    return { request, post, upgrade };
}

function frameReader(socket, initial) {
    const chunks = socket[Symbol.asyncIterator]();
    let buffer = initial;
    async function take(size) {
        while (buffer.length < size) {
            const next = await chunks.next();
            assert.ok(!next.done, 'The socket closed before a frame was received');
            buffer = Buffer.concat([buffer, next.value]);
        }
        const result = buffer.subarray(0, size);
        buffer = buffer.subarray(size);
        return result;
    }
    return async () => {
        const [flags, length] = await take(2);
        assert.equal(flags & 0xf0, 0x80);
        assert.ok(length <= 127, 'Expected an unmasked server frame');
        const size = length === 126 ? (await take(2)).readUInt16BE()
            : length === 127 ? Number((await take(8)).readBigUInt64BE()) : length;
        return { opcode: flags & 0xf, payload: await take(size) };
    };
}

function clientFrame(opcode, data) {
    const payload = Buffer.from(data);
    assert.ok(payload.length <= 65535);
    const header = Buffer.alloc(payload.length < 126 ? 2 : 4);
    header[0] = 0x80 | opcode;
    if (header.length === 2) {
        header[1] = 0x80 | payload.length;
    } else {
        header[1] = 0x80 | 126;
        header.writeUInt16BE(payload.length, 2);
    }
    const mask = randomBytes(4);
    const maskedPayload = payload.map((byte, index) => byte ^ mask[index % mask.length]);
    return Buffer.concat([header, mask, maskedPayload]);
}

function assertTerminalStatus(frame, type) {
    assert.equal(frame.opcode, 1);
    assert.equal(JSON.parse(frame.payload).type, type);
}

function assertCookie(response, maxAge) {
    assert.equal(response.status, 204);
    assert.equal(response.headers['set-cookie']?.length, 1);
    const [value, ...attributes] = response.headers['set-cookie'][0].split('; ');
    assert.ok(value.startsWith(cookiePrefix));
    assert.ok(attributes.includes('Path=/'));
    assert.ok(attributes.includes('HttpOnly'));
    assert.ok(attributes.includes('Secure'));
    assert.ok(attributes.includes('SameSite=Strict'));
    assert.ok(attributes.includes(`Max-Age=${maxAge}`));
    assert.ok(!attributes.some(attribute => /^Domain=/i.test(attribute)));
    if (maxAge === 0) assert.equal(value, cookiePrefix);
    return value;
}

function sign(body, key) {
    return createHmac('sha256', key).update(body).digest('base64url');
}

async function assertPolicyClose(ws) {
    let frame;
    do {
        frame = await ws.frame();
    } while (frame.opcode === 1 || frame.opcode === 2);
    assert.equal(frame.opcode, 8);
    assert.equal(frame.payload.readUInt16BE(), 1008);
}

test('HTTPS verifies trust and enforces durable JWT sessions on HTTP and WebSocket routes', async t => {
    const fixture = await authFixture(t);
    const { request, post, upgrade } = client(fixture);
    const credentials = { username: 'fixture-admin', password: 'fake fixture passphrase Z7v9Q6' };

    await fixture.start();
    await assert.rejects(request('/setup', { ca: [] }), { code: 'UNABLE_TO_GET_ISSUER_CERT_LOCALLY' });
    await assert.rejects(request('/setup', { hostname: 'nested.panel.example.test' }),
        { code: 'ERR_TLS_CERT_ALTNAME_INVALID' });
    const entry = await request('/');
    assert.equal(entry.status, 303);
    assert.equal(entry.headers.location, `${fixture.basePath}/setup`);
    assert.equal((await request('/', { headers: { Host: 'other.example.invalid' } })).status, 403);
    assert.equal((await request('/api/session')).status, 401);
    assert.equal((await upgrade({ Origin: fixture.origin }, '/ws/workspace')).status, 401);
    assert.equal((await post('setup', credentials, { 'X-Wave-CSRF': '0' })).status, 403);
    assert.equal((await post('setup', credentials, { Origin: 'https://other.example.invalid' })).status, 403);
    const setup = await Promise.all([post('setup', credentials), post('setup', credentials)]);
    assert.deepEqual(setup.map(response => response.status).sort(), [201, 409]);
    assert.equal((await request('/')).headers.location, `${fixture.basePath}/sign-in`);

    const cookie = assertCookie(await post('sign-in', credentials), expirySeconds);
    const { auth } = JSON.parse(await readFile(fixture.database, 'utf8'));
    const [header, payload, signature] = cookie.slice(cookiePrefix.length).split('.');
    assert.deepEqual(JSON.parse(Buffer.from(header, 'base64url')), { alg: 'HS256', typ: 'JWT' });
    assert.equal(signature, sign(`${header}.${payload}`, auth.jwt_key));
    const claims = JSON.parse(Buffer.from(payload, 'base64url'));
    assert.equal(claims.sub, credentials.username);
    const forged = `${cookiePrefix}${header}.${payload}.${signature[0] === 'A' ? 'B' : 'A'}${signature.slice(1)}`;
    assert.equal((await request('/api/session', { headers: { Cookie: forged } })).status, 401);
    const authenticated = { Cookie: cookie, Origin: fixture.origin };
    assert.equal((await upgrade({ Cookie: cookie, Origin: 'https://other.example.invalid' })).status, 403);
    assert.equal((await request('/wave-db.json', { headers: authenticated })).status, 404);

    await fixture.stop();
    await fixture.start();
    const retained = await request('/api/session', { headers: authenticated });
    assert.equal(retained.status, 200);
    const expectedIdentity = { type: 'session', username: claims.sub, expires_at: claims.exp, session_id: claims.jti };
    assert.deepEqual(JSON.parse(retained.body), expectedIdentity);
    assert.equal(retained.headers['cache-control'], 'no-store');

    const ws = await upgrade(authenticated);
    assert.equal(ws.status, 101);
    const terminal = process.platform === 'linux' ? await upgrade(authenticated, '/ws/console') : null;
    try {
        const identity = await ws.frame();
        assert.equal(identity.opcode, 1);
        assert.deepEqual(JSON.parse(identity.payload), expectedIdentity);
        assert.equal((await request('/api/session', { headers: { Cookie: cookiePrefix + claims.jti } })).status, 401);
        if (terminal) {
            assert.equal(terminal.status, 101);
            terminal.socket.write(clientFrame(1, JSON.stringify({ type: 'resize', cols: 80, rows: 24 })));
            assertTerminalStatus(await terminal.frame(), 'ready');
            const paste = clientFrame(2, "printf 'WAVE-WSS-%s\\n' \"$HOME\" \"$HISTFILE\"\r");
            terminal.socket.write(paste.subarray(0, 12));
            await delay(600);
            terminal.socket.write(paste.subarray(12));
            const home = join(fixture.cwd, 'home');
            const expected = `WAVE-WSS-${home}\r\nWAVE-WSS-${join(home, '.bash_history')}\r\n`;
            let output = '';
            while (!output.includes(expected)) {
                const frame = await terminal.frame();
                assert.equal(frame.opcode, 2);
                output += frame.payload.toString('utf8');
            }
        }
        assertCookie(await post('sign-out', {}, authenticated), 0);
        await assertPolicyClose(ws);
        if (terminal) await assertPolicyClose(terminal);
    } finally {
        ws.socket.destroy();
        terminal?.socket?.destroy();
    }

    const shortSession = assertCookie(await post('sign-in', credentials), expirySeconds);
    await fixture.stop();
    // Seed an expired signed session to exercise expiry without waiting on a timer.
    const state = JSON.parse(await readFile(fixture.database, 'utf8'));
    const [newHeader, newPayload] = shortSession.slice(cookiePrefix.length).split('.');
    const newClaims = JSON.parse(Buffer.from(newPayload, 'base64url'));
    state.auth.sessions.find(session => session.id === newClaims.jti).expires_at =
        newClaims.exp = Math.floor(Date.now() / 1000) - 1;
    await writeFile(fixture.database, JSON.stringify(state));
    const body = `${newHeader}.${Buffer.from(JSON.stringify(newClaims)).toString('base64url')}`;
    const expired = { Cookie: `${cookiePrefix}${body}.${sign(body, auth.jwt_key)}`, Origin: fixture.origin };
    await fixture.start();
    assert.equal((await upgrade(authenticated)).status, 401);
    assert.equal((await request('/api/session', { headers: expired })).status, 401);
    assert.equal((await post('sign-in', { ...credentials, password: 'wrong fixture passphrase' })).status, 401);
    assert.equal((await post('sign-in', credentials)).status, 429);
});

test('saves the session expiry in days for new sign-ins and retains it after restart', async t => {
    const fixture = await authFixture(t);
    const { request, post, upgrade } = client(fixture);
    const credentials = { username: 'fixture-admin', password: 'fixture session expiry passphrase 4Hq8v' };

    async function signIn(days) {
        const now = () => Math.floor(Date.now() / 1000);
        const earliest = now();
        const lifetime = days * secondsPerDay;
        const cookie = assertCookie(await post('sign-in', credentials), lifetime);
        const claims = JSON.parse(Buffer.from(cookie.split('.')[1], 'base64url'));
        assert.ok(claims.exp >= earliest + lifetime && claims.exp <= now() + lifetime);
        const database = JSON.parse(await readFile(fixture.database, 'utf8'));
        assert.equal(database.auth.sessions.find(session => session.id === claims.jti).expires_at, claims.exp);
        return { headers: { Cookie: cookie, Origin: fixture.origin }, expiry: claims.exp };
    }

    async function assertCurrentExpiry({ headers, expiry }) {
        const session = await request('/api/session', { headers });
        assert.equal(session.status, 200);
        assert.equal(JSON.parse(session.body).expires_at, expiry);
    }

    await fixture.start();
    assert.equal((await post('setup', credentials)).status, 201);
    const existing = await signIn(3);
    const ws = await upgrade(existing.headers, '/ws/workspace');
    assert.equal(ws.status, 101);
    try {
        const first = await ws.frame();
        assert.equal(first.opcode, 1);
        assert.equal(JSON.parse(first.payload).data.settings.session_expiry_days, 3);

        let id = 0;
        async function saveExpiry(days) {
            const request_id = ++id;
            ws.socket.write(clientFrame(1, JSON.stringify({
                action: 'apply_session_settings', session_expiry_days: days, request_id,
            })));
            for (;;) {
                const frame = await ws.frame();
                assert.equal(frame.opcode, 1);
                const reply = JSON.parse(frame.payload);
                if (reply.request_id === request_id) return reply;
            }
        }

        for (const invalid of [undefined, null, -1, 0, 1.5, 366, '7']) {
            const reply = await saveExpiry(invalid);
            assert.equal(reply.ok, false);
            assert.equal(reply.data.settings.session_expiry_days, 3);
        }
        const saved = await saveExpiry(1);
        assert.equal(saved.ok, true);
        assert.equal(saved.data.settings.session_expiry_days, 1);
    } finally {
        ws.socket.destroy();
    }

    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).session_expiry_days, 1);
    const updated = await signIn(1);
    await assertCurrentExpiry(existing);
    await assertCurrentExpiry(updated);
    await fixture.stop();
    await fixture.start();
    await assertCurrentExpiry(existing);
    await assertCurrentExpiry(updated);
    await signIn(1);
});

test('changes the panel base path live and keeps authenticated sessions', async t => {
    const fixture = await authFixture(t);
    const original = client(fixture);
    const credentials = { username: 'fixture-admin', password: 'fixture base path passphrase 4GX1v' };
    const selection = path => ({ 'panel-base-path': path });
    await fixture.start();
    assert.equal((await original.post('base-path', selection('/ops/wave'))).status, 401);
    assert.equal((await original.post('setup', credentials)).status, 201);
    const headers = {
        Cookie: assertCookie(await original.post('sign-in', credentials), expirySeconds), Origin: fixture.origin,
    };
    assert.equal((await original.post('base-path', selection('/ops/wave'), { ...headers, 'X-Wave-CSRF': '0' })).status, 403);
    assert.equal((await original.post('base-path', selection('/ops/wave'), {
        ...headers, Origin: 'https://other.example.invalid',
    })).status, 403);
    for (const invalid of ['/', '/ops/', '/_wave-setup/admin', '//other.example.invalid']) {
        assert.equal((await original.post('base-path', selection(invalid), headers)).status, 400);
    }

    const saved = await readFile(fixture.database, 'utf8');
    const monitor = await original.upgrade(headers);
    const workspace = await original.upgrade(headers, '/ws/workspace');
    const terminal = process.platform === 'linux' ? await original.upgrade(headers, '/ws/console') : null;
    let selected;
    try {
        assert.equal(monitor.status, 101);
        assert.equal(workspace.status, 101);
        assert.equal(JSON.parse((await monitor.frame()).payload).type, 'session');
        assert.equal(JSON.parse((await workspace.frame()).payload).type, 'snapshot');
        if (terminal) {
            assert.equal(terminal.status, 101);
            terminal.socket.write(clientFrame(1, JSON.stringify({ type: 'resize', cols: 80, rows: 24 })));
            assertTerminalStatus(await terminal.frame(), 'ready');
        }

        const paths = ['/ops/wave', '/admin/console'];
        const responses = await Promise.all(paths.map(path => original.post('base-path', selection(path), headers)));
        assert.deepEqual(responses.map(response => response.status).sort(), [201, 404]);
        const winner = responses.findIndex(response => response.status === 201);
        selected = paths[winner];
        assert.equal(responses[winner].headers.location, `${selected}/`);
        assert.equal(responses[winner].headers['set-cookie'], undefined);
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')), {
            ...JSON.parse(saved), 'panel-base-path': selected,
        });

        const notice = await monitor.frame();
        assert.equal(notice.opcode, 1);
        assert.deepEqual(JSON.parse(notice.payload), { type: 'base_path', path: selected });
        assert.equal((await monitor.frame()).opcode, 8);
        await assertPolicyClose(workspace);
        if (terminal) await assertPolicyClose(terminal);

        assert.equal((await original.request('/', { headers })).status, 404);
        assert.equal((await original.request('/api/session', { headers })).status, 404);
        assert.equal((await original.upgrade(headers)).status, 404);
        const moved = client(fixture, selected);
        assert.equal((await moved.request('/', { headers })).status, 200);
        assert.equal((await moved.request('/api/session', { headers })).status, 200);
    } finally {
        monitor.socket?.destroy();
        workspace.socket?.destroy();
        terminal?.socket?.destroy();
    }

    await fixture.stop();
    await fixture.start();
    const moved = client(fixture, selected);
    assert.equal((await moved.request('/api/session', { headers })).status, 200);
    assert.equal((await original.request('/', { headers })).status, 404);
});

test('saves validated panel TLS settings for restart and keeps preview backups separate', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const { ca, chainPath, keyPath } = await tlsFixture(fixture.cwd);
    const secure = { ...fixture, ca, origin: `https://${fixture.hostname}:${new URL(fixture.origin).port}` };
    const credentials = { username: 'fixture-admin', password: 'fixture TLS switch passphrase 3Bm8Q' };
    const savedFields = { domain: fixture.hostname, certificate_chain: chainPath, private_key: keyPath };
    const emptyFields = { domain: '', certificate_chain: '', private_key: '' };
    const tlsState = (enabled, restart_required, fields = savedFields) =>
        ({ enabled, ...fields, supported: true, restart_required });
    const readDatabase = () => readFile(fixture.database, 'utf8');
    let id = 0;

    async function withPreview(deployment, expected, usePreview) {
        const { post, request, upgrade } = client(deployment);
        const signIn = await post('sign-in', credentials);
        assert.equal(signIn.status, 204);
        const cookie = deployment === secure ? assertCookie(signIn, expirySeconds)
            : signIn.headers['set-cookie'][0].split(';', 1)[0];
        if (deployment === fixture) assert.match(cookie, /^wave_session=/);
        const headers = { Cookie: cookie, Origin: deployment.origin };
        const ws = await upgrade(headers, '/ws/workspace');
        assert.equal(ws.status, 101);
        try {
            const first = await ws.frame();
            assert.equal(first.opcode, 1);
            assert.deepEqual(JSON.parse(first.payload).data.settings.tls, expected);
            async function apply(action, body) {
                const request_id = ++id;
                ws.socket.write(clientFrame(1, JSON.stringify({ action, ...body, request_id })));
                for (;;) {
                    const frame = await ws.frame();
                    assert.equal(frame.opcode, 1);
                    const reply = JSON.parse(frame.payload);
                    if (reply.request_id === request_id) return reply;
                }
            }
            await usePreview(apply, async () => {
                assert.equal((await request('/api/session', { headers })).status, 200);
            });
        } finally {
            ws.socket.destroy();
        }
    }

    await fixture.start();
    const plain = client(fixture);
    assert.equal((await plain.upgrade({ Origin: fixture.origin }, '/ws/workspace')).status, 401);
    assert.equal((await plain.post('setup', credentials)).status, 201);
    await withPreview(fixture, tlsState(false, false, emptyFields), async (apply, assertListener) => {
        const save = settings => apply('apply_tls_settings', { settings });
        const before = await readDatabase();
        for (const invalid of [{ enabled: 'true' }, { enabled: true },
            { enabled: true, ...savedFields, domain: 'nested.panel.example.test' },
            { enabled: true, ...savedFields, private_key: join(fixture.cwd, 'missing.key') }]) {
            const reply = await save(invalid);
            assert.equal(reply.ok, false);
            assert.equal(reply.feedback_in_logs_only, true);
            assert.ok(reply.data.logs.some(row => row.level === 'error' && row.message === reply.feedback));
            assert.deepEqual(reply.data.settings.tls, tlsState(false, false, emptyFields));
        }
        assert.equal(await readDatabase(), before);

        const enabled = await save({ enabled: true, ...savedFields });
        assert.equal(enabled.ok, true);
        assert.deepEqual(enabled.data.settings.tls, tlsState(true, true));
        assert.deepEqual(JSON.parse(await readDatabase()).tls, { enabled: true, ...savedFields });
        const changedSample = await apply('apply_settings', { settings: { domain: 'sample.example.test' } });
        assert.equal(changedSample.ok, true);
        assert.equal(changedSample.data.settings.domain, 'sample.example.test');

        const restored = await apply('restore_preview_backup', { backup: {
            format: 'wave-preview-backup', version: 1, configs: [],
            settings: { domain: 'backup.example.test', certificate_path: '', private_key_path: '',
                tls: { enabled: false } },
        } });
        assert.equal(restored.ok, true);
        assert.equal(restored.data.settings.domain, 'backup.example.test');
        assert.deepEqual(restored.data.settings.tls, tlsState(true, true));
        assert.deepEqual(JSON.parse(await readDatabase()).tls, { enabled: true, ...savedFields });
        await assertListener();
    });

    await fixture.stop();
    await fixture.start({}, { encrypted: true });
    await withPreview(secure, tlsState(true, false), async (apply, assertListener) => {
        const save = settings => apply('apply_tls_settings', { settings });
        const alternate = { ...savedFields, domain: 'second.example.test' };
        const changed = await save({ enabled: true, ...alternate });
        assert.equal(changed.ok, true);
        assert.deepEqual(changed.data.settings.tls, tlsState(true, true, alternate));
        const reverted = await save({ enabled: true, ...savedFields });
        assert.equal(reverted.ok, true);
        assert.deepEqual(reverted.data.settings.tls, tlsState(true, false));
        await unlink(keyPath);
        const disabled = await save({ enabled: false });
        assert.equal(disabled.ok, true);
        assert.deepEqual(disabled.data.settings.tls, tlsState(false, true));
        assert.deepEqual(JSON.parse(await readDatabase()).tls, { enabled: false, ...savedFields });
        await assertListener();
    });

    await fixture.stop();
    await fixture.start();
    await withPreview(fixture, tlsState(false, false), async (_apply, assertListener) => assertListener());
});

for (const transport of ['WebSocket', 'HTTP']) {
    test(`activates panel TLS over ${transport} without restarting the backend process`, async t => {
        const fixture = await authFixture(t, undefined, { encrypted: false, basePath: '/control/room' });
        const { ca, chainPath, keyPath } = await tlsFixture(fixture.cwd);
        const settings = { enabled: true, domain: fixture.hostname, certificate_chain: chainPath, private_key: keyPath };
        const httpsOrigin = `https://${fixture.hostname}:${new URL(fixture.origin).port}`;
        const secure = client({ ...fixture, ca, origin: httpsOrigin });
        const plain = client(fixture);
        const credentials = { username: 'fixture-admin', password: 'fixture TLS activation passphrase 5Qm7R' };
        const core = transport === 'WebSocket' && process.platform === 'linux'
            ? await seedCore(fixture.backend, { executable: true }) : undefined;
        await fixture.start();
        const pid = fixture.pid;
        assert.equal((await plain.post('setup', credentials)).status, 201);
        const signIn = await plain.post('sign-in', credentials);
        assert.equal(signIn.status, 204);
        const headers = { Cookie: signIn.headers['set-cookie'][0].split(';', 1)[0], Origin: fixture.origin };
        const workspace = await plain.upgrade(headers, '/ws/workspace');
        const monitor = await plain.upgrade(headers);
        assert.equal(workspace.status, 101);
        assert.equal(monitor.status, 101);
        t.after(() => {
            workspace.socket?.destroy();
            monitor.socket?.destroy();
        });

        const readSnapshot = async (socket, predicate = () => true) => {
            for (;;) {
                const frame = await socket.frame();
                assert.equal(frame.opcode, 1);
                const payload = JSON.parse(frame.payload);
                assert.equal(payload.type, 'snapshot');
                if (predicate(payload)) return payload;
            }
        };
        const identity = JSON.parse((await monitor.frame()).payload);
        const initial = await readSnapshot(workspace);
        const httpHeaders = { ...headers, 'X-Wave-Session': identity.session_id };
        if (transport === 'HTTP') {
            const snapshot = await plain.request('/api/workspace', { headers: httpHeaders });
            assert.equal(snapshot.status, 200);
            httpHeaders['X-Wave-Workspace'] = JSON.parse(snapshot.body).workspace_id;
        }
        let id = 0;
        async function send(action, fields = {}) {
            const request_id = ++id;
            const body = { action, request_id, schematic_revision: initial.schematic_revision, ...fields };
            let reply;
            if (transport === 'HTTP') {
                const response = await plain.request('/api/workspace', { method: 'POST', data: body,
                    headers: { ...httpHeaders, 'X-Wave-CSRF': '1', 'Content-Type': 'application/json' } });
                assert.equal(response.status, 200);
                reply = JSON.parse(response.body);
            } else {
                workspace.socket.write(clientFrame(1, JSON.stringify(body)));
                reply = await readSnapshot(workspace, frame => frame.request_id === request_id);
            }
            assert.equal(reply.request_id, request_id);
            assert.equal(reply.action, action);
            return reply;
        }

        let corePid;
        if (core) {
            const created = await send('create_core_config', { name: 'TLS continuity' });
            assert.equal(created.ok, true, created.feedback);
            const { id: configId } = created.data.server.core.configs.find(config => config.name === 'TLS continuity');
            const enabled = await send('set_core_config_enabled', { id: configId, enabled: true });
            assert.equal(enabled.ok, true, enabled.feedback);
            const started = await send('start_core');
            assert.equal(started.ok, true, started.feedback);
            if (started.data.server.core.state !== 'running') {
                await readSnapshot(workspace, frame => frame.data.server.core.state === 'running');
            }
            const deadline = Date.now() + timeout;
            while (corePid === undefined) {
                try {
                    corePid = JSON.parse(await readFile(core.processFile, 'utf8')).pid;
                } catch (error) {
                    if (error.code !== 'ENOENT' || Date.now() >= deadline) throw error;
                    await delay(20);
                }
            }
        }

        const before = await readFile(fixture.database, 'utf8');
        const invalid = await send('apply_tls_settings', { activate: true,
            settings: { ...settings, private_key: join(fixture.cwd, 'missing.key') } });
        assert.equal(invalid.ok, false);
        assert.match(invalid.feedback, /private key/i);
        assert.equal(await readFile(fixture.database, 'utf8'), before);
        assert.equal((await plain.request('/api/session', { headers })).status, 200);

        const confirmed = await send('apply_tls_settings', { activate: true, settings });
        assert.equal(confirmed.ok, true, confirmed.feedback);
        assert.equal(confirmed.result?.https_url, httpsOrigin + fixture.basePath + '/');
        const deadline = Date.now() + timeout;
        for (;;) {
            try {
                assert.equal((await secure.request('/api/session', { headers })).status, 401);
                break;
            } catch (error) {
                if (!['ECONNREFUSED', 'ECONNRESET', 'EPIPE', 'ERR_SSL_WRONG_VERSION_NUMBER'].includes(error.code) ||
                    Date.now() >= deadline) throw error;
                await delay(20);
            }
        }
        assert.equal(fixture.pid, pid);
        assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')).tls, settings);

        const tlsCookie = assertCookie(await secure.post('sign-in', credentials), expirySeconds);
        const tlsHeaders = { Cookie: tlsCookie, Origin: httpsOrigin };
        const authenticated = await secure.request('/api/session', { headers: tlsHeaders });
        assert.equal(authenticated.status, 200);
        assert.equal(JSON.parse(authenticated.body).username, credentials.username);
        const active = await secure.upgrade(tlsHeaders, '/ws/workspace');
        assert.equal(active.status, 101);
        try {
            const { data } = await readSnapshot(active);
            assert.deepEqual(data.settings.tls, { ...settings, supported: true, restart_required: false });
            if (corePid) {
                assert.equal(data.server.core.state, 'running');
                assert.equal(JSON.parse(await readFile(core.processFile, 'utf8')).pid, corePid);
                assert.doesNotThrow(() => process.kill(corePid, 0));
            }
        } finally {
            active.socket.destroy();
        }
    });
}

test('closes the TLS terminal when the shell exits during a final keystroke',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t);
        const { post, upgrade } = client(fixture);
        const credentials = { username: 'fixture-admin', password: 'fixture terminal passphrase 8Vy6z' };
        await fixture.start();
        assert.equal((await post('setup', credentials)).status, 201);
        const cookie = assertCookie(await post('sign-in', credentials), expirySeconds);
        const terminal = await upgrade({ Cookie: cookie, Origin: fixture.origin }, '/ws/console');
        assert.equal(terminal.status, 101);
        t.after(() => terminal.socket.destroy());
        terminal.socket.write(clientFrame(1, JSON.stringify({ type: 'resize', cols: 80, rows: 24 })));
        assertTerminalStatus(await terminal.frame(), 'ready');

        // The last key spans two TLS writes while a shell command finishes.
        const key = clientFrame(2, 'x');
        terminal.socket.write(Buffer.concat([clientFrame(2, 'sleep 0.3; exit\r'), key.subarray(0, -1)]));
        let lastByteSent = false;
        const sendLastByte = setTimeout(() => {
            lastByteSent = true;
            terminal.socket.write(key.subarray(-1));
        }, 5000);
        t.after(() => clearTimeout(sendLastByte));

        let frame;
        do {
            frame = await terminal.frame();
        } while (frame.opcode === 2);
        assertTerminalStatus(frame, 'exit');
        const close = await terminal.frame();
        assert.equal(close.opcode, 8);
        assert.equal(close.payload.readUInt16BE(), 1000);
        if (!terminal.socket.readableEnded) {
            const ended = once(terminal.socket, 'end', { signal: AbortSignal.timeout(timeout) });
            terminal.socket.resume();
            await ended;
        }
        assert.equal(lastByteSent, false, 'Shell exit must release the connection while the key is in flight.');
    });

test('administrator changes require the current password and revoke all sessions', async t => {
    const fixture = await authFixture(t);
    const { request, post, upgrade } = client(fixture);
    const original = { username: 'fixture-admin', password: 'fixture operator passphrase 7Pz3u' };
    const change = { username: 'fixture-renamed', current_password: original.password,
        password: 'fixture replacement passphrase 8fQ6w' };
    await fixture.start();
    assert.equal((await post('setup', original)).status, 201);
    const oldHash = JSON.parse(await readFile(fixture.database, 'utf8')).admin[0].password;

    assert.equal((await post('admin', change)).status, 401);
    const first = { Cookie: assertCookie(await post('sign-in', original), expirySeconds), Origin: fixture.origin };
    const second = { Cookie: assertCookie(await post('sign-in', original), expirySeconds), Origin: fixture.origin };
    const ws = await upgrade(second, '/ws/workspace');
    assert.equal(ws.status, 101);
    try {
        assert.equal((await ws.frame()).opcode, 1);
        assert.equal((await post('admin', { ...change, current_password: 'wrong fixture passphrase' }, first)).status, 403);
        await delay(1100);
        assertCookie(await post('admin', change, first), 0);
        await assertPolicyClose(ws);
    } finally {
        ws.socket.destroy();
    }
    for (const headers of [first, second]) {
        assert.equal((await request('/api/session', { headers })).status, 401);
    }
    const config = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.equal(config.admin[0].username, change.username);
    assert.match(config.admin[0].password, /^\$argon2id\$/);
    assert.notEqual(config.admin[0].password, oldHash);
    assert.deepEqual(config.auth.sessions, []);
    assertCookie(await post('sign-in', { username: change.username, password: change.password }), expirySeconds);
});

test('checks administrator password format and strength on setup and replacement', async t => {
    const fixture = await authFixture(t);
    const { request, post } = client(fixture);
    const credentials = { username: 'fixture-admin', password: 'Z7v9Q6' };
    const invalid = [
        ['Q9v7Z', 'five characters'],
        ['ééQ7🔒', 'five Unicode characters with more than six UTF-8 bytes'],
        ['JqvMrg９', 'no ASCII digit'],
        ['é928473', 'no ASCII letter'],
    ];

    await fixture.start();
    for (const [password, reason] of invalid) {
        assert.equal((await post('setup', { ...credentials, password })).status, 400, reason);
    }
    assert.equal((await post('setup', { ...credentials, password: 'Password1' })).status, 422);
    assert.equal((await request('/')).headers.location, `${fixture.basePath}/setup`);
    assert.equal((await post('setup', credentials)).status, 201);
    const headers = { Cookie: assertCookie(await post('sign-in', credentials), expirySeconds), Origin: fixture.origin };
    const oldHash = JSON.parse(await readFile(fixture.database, 'utf8')).admin[0].password;
    const change = { username: credentials.username, current_password: credentials.password };
    for (const [password, reason] of invalid) {
        assert.equal((await post('admin', { ...change, password }, headers)).status, 400, reason);
    }
    assert.equal((await post('admin', { ...change, password: 'Password1' }, headers)).status, 422);
    assert.equal((await request('/api/session', { headers })).status, 200);
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).admin[0].password, oldHash);

    const replacement = 'H8b#7x';
    assertCookie(await post('admin', { ...change, password: replacement }, headers), 0);
    assertCookie(await post('sign-in', { ...credentials, password: replacement }), expirySeconds);
});

test('keeps legacy administrator credentials valid for sign-in and a username-only change', async t => {
    const fixture = await authFixture(t);
    const { request, post } = client(fixture);
    const legacy = { username: 'fixture-admin', password: 'legacy fixture passphrase' };
    // Precomputed Argon2id hash of the synthetic legacy password.
    const hash = '$argon2id$v=19$m=65536,t=3,p=4$d2F2ZS1vbGQtZml4dHVyZQ$1CWoSke1awH/9p2Fh4pTvPDCBM6dLl+rRvo0u5HHLyk';
    const config = JSON.parse(await readFile(fixture.database, 'utf8'));
    config.admin = [{ username: legacy.username, password: hash }];
    await writeFile(fixture.database, JSON.stringify(config));

    await fixture.start();
    const headers = { Cookie: assertCookie(await post('sign-in', legacy), expirySeconds), Origin: fixture.origin };
    const username = 'fixture-renamed';
    const change = { username, current_password: legacy.password, password: '' };
    assertCookie(await post('admin', change, headers), 0);
    assert.equal((await request('/api/session', { headers })).status, 401);
    assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')).admin, [{ username, password: hash }]);
    assertCookie(await post('sign-in', { ...legacy, username }), expirySeconds);
});
