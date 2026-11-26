const assert = require('node:assert/strict');
const { createHmac, randomBytes } = require('node:crypto');
const { readFile, readdir, stat, writeFile } = require('node:fs/promises');
const https = require('node:https');
const { setTimeout: delay } = require('node:timers/promises');
const { test } = require('node:test');
const { authFixture, timeout } = require('./auth_fixture.cjs');

const cookiePrefix = '__Host-wave_session=';
const expirySeconds = 86400;

function client(fixture) {
    async function request(path, { method = 'GET', headers = {}, data, ca = fixture.ca,
        hostname = fixture.hostname } = {}) {
        const body = data === undefined ? undefined : JSON.stringify(data);
        const url = new URL(path, fixture.origin);
        url.hostname = hostname;
        return new Promise((resolve, reject) => {
            const req = https.request(url, {
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
            const req = https.request(fixture.origin + path, {
                ca: fixture.ca, lookup: fixture.lookup, servername: fixture.hostname, agent: false,
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

function assertCookie(response, maxAge) {
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

test('TLS setup, protected routes, durable admin and session, logout, and expiry', async t => {
    const fixture = await authFixture(t);
    const { request, post, upgrade } = client(fixture);
    const diskPaths = (await readdir(fixture.cwd, { recursive: true })).sort();
    const candidates = [
        { username: 'fixture-alfa', password: 'fake passphrase for alder' },
        { username: 'fixture-bravo', password: 'fake passphrase for birch' },
    ];

    await fixture.start();
    await assert.rejects(request('/setup', { ca: [] }), { code: 'UNABLE_TO_GET_ISSUER_CERT_LOCALLY' });
    await assert.rejects(request('/setup', { hostname: 'nested.panel.example.test' }),
        { code: 'ERR_TLS_CERT_ALTNAME_INVALID' });
    for (const path of ['/', '/sign-in', '/panel']) {
        const response = await request(path);
        assert.equal(response.status, 303, path);
        assert.equal(response.headers.location, '/setup');
    }
    assert.equal((await request('/setup')).body, fixture.index);
    assert.equal((await request('/api/session')).status, 401);
    assert.equal((await request('/', { headers: { Host: 'other.example.invalid' } })).status, 403);
    assert.equal((await upgrade({ Origin: fixture.origin })).status, 401);
    assert.equal((await upgrade({})).status, 403);
    assert.equal((await post('setup', { ...candidates[0], password: 'short' })).status, 400);
    assert.equal((await post('setup', candidates[0], { 'X-Wave-CSRF': '0' })).status, 403);
    assert.equal((await post('setup', candidates[0], { Origin: 'https://other.example.invalid' })).status, 403);
    assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')).admin, []);

    const setup = await Promise.all(candidates.map(input => post('setup', input)));
    assert.deepEqual(setup.map(response => response.status).sort(), [201, 409]);
    const { username, password } = candidates[setup.findIndex(response => response.status === 201)];
    assert.equal((await post('setup', candidates[0])).status, 409);
    assert.equal((await request('/setup')).headers.location, '/sign-in');
    assert.equal((await stat(fixture.database)).mode & 0o777, 0o600);
    const stored = await readFile(fixture.database, 'utf8');
    const { admin, auth } = JSON.parse(stored);
    assert.deepEqual(admin, [{ username, password: admin[0].password }]);
    assert.match(admin[0].password, /^\$argon2id\$v=19\$m=\d+,t=\d+,p=\d+\$/);
    assert.ok(!stored.includes(password));
    assert.deepEqual((await readdir(fixture.cwd, { recursive: true })).sort(), diskPaths);

    await fixture.stop();
    await fixture.start();
    assert.equal((await request('/setup')).headers.location, '/sign-in');
    assert.equal((await post('setup', candidates[0])).status, 409);
    assert.equal((await post('sign-in', { username, password: 'fake incorrect passphrase' })).status, 401);
    assert.equal((await post('sign-in', { username, password })).status, 429);
    await delay(1100);

    const login = await post('sign-in', { username, password });
    assert.equal(login.status, 204);
    assert.equal(login.body, '');
    const cookie = assertCookie(login, expirySeconds);
    const components = cookie.slice(cookiePrefix.length).split('.');
    assert.equal(components.length, 3);
    const [header, payload, signature] = components;
    assert.deepEqual(JSON.parse(Buffer.from(header, 'base64url')), { alg: 'HS256', typ: 'JWT' });
    assert.equal(signature, sign(`${header}.${payload}`, auth.jwt_key));
    const claims = JSON.parse(Buffer.from(payload, 'base64url'));
    assert.equal(claims.sub, username);
    assert.ok(Math.abs(claims.exp - Math.floor(Date.now() / 1000) - expirySeconds) < 5);
    const badToken = `${header}.${payload}.${signature[0] === 'A' ? 'B' : 'A'}${signature.slice(1)}`;
    const identity = { type: 'session', username, expires_at: claims.exp };
    const authenticated = { Cookie: cookie, Origin: fixture.origin };
    for (const invalid of ['bad-token', badToken]) {
        assert.equal((await request('/api/session', { headers: { Cookie: cookiePrefix + invalid } })).status, 401);
        assert.equal((await upgrade({ Origin: fixture.origin, Cookie: cookiePrefix + invalid })).status, 401);
    }
    assert.equal((await upgrade({ Cookie: cookie, Origin: 'https://other.example.invalid' })).status, 403);
    assert.equal((await upgrade({ Cookie: cookie })).status, 403);
    const preview = await upgrade(authenticated, '/ws/preview');
    assert.equal(preview.status, 101);
    try {
        assert.equal(JSON.parse((await preview.frame()).payload).type, 'snapshot');
    } finally {
        preview.socket.destroy();
    }
    assert.equal((await request('/panel', { headers: authenticated })).body, fixture.index);
    assert.equal((await request('/wave-db.json', { headers: authenticated })).status, 404);

    await fixture.stop();
    await fixture.start();
    const retained = await request('/api/session', { headers: authenticated });
    assert.equal(retained.status, 200);
    assert.deepEqual(JSON.parse(retained.body), identity);
    assert.equal(retained.headers['cache-control'], 'no-store');

    const ws = await upgrade(authenticated);
    assert.equal(ws.status, 101);
    try {
        const initial = await ws.frame();
        assert.equal(initial.opcode, 1);
        assert.deepEqual(JSON.parse(initial.payload), identity);
        assert.equal((await post('sign-out', {}, { ...authenticated, 'X-Wave-CSRF': '0' })).status, 403);
        assert.equal((await request('/api/session', { headers: authenticated })).status, 200);
        const logout = await post('sign-out', {}, authenticated);
        assert.equal(logout.status, 204);
        assertCookie(logout, 0);
        const closing = await ws.frame();
        assert.equal(closing.opcode, 8);
        assert.equal(closing.payload.readUInt16BE(), 1008);
    } finally {
        ws.socket.destroy();
    }
    assert.equal((await request('/panel', { headers: authenticated })).headers.location, '/sign-in');
    assert.equal((await upgrade(authenticated)).status, 401);

    const secondLogin = await post('sign-in', { username, password });
    assert.equal(secondLogin.status, 204);
    const newCookie = assertCookie(secondLogin, expirySeconds);
    await fixture.stop();
    // Shorten only the isolated fixture's session and sign its matching JWT so the
    // expiry checks exercise a genuinely signed, previously accepted session.
    const state = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.equal(state.auth.sessions.length, 1);
    const [newHeader, newPayload] = newCookie.slice(cookiePrefix.length).split('.');
    const newClaims = JSON.parse(Buffer.from(newPayload, 'base64url'));
    const expiry = Math.floor(Date.now() / 1000) + 6;
    state.auth.sessions[0].expires_at = newClaims.exp = expiry;
    await writeFile(fixture.database, JSON.stringify(state));
    const body = `${newHeader}.${Buffer.from(JSON.stringify(newClaims)).toString('base64url')}`;
    const expiring = { Cookie: `${cookiePrefix}${body}.${sign(body, auth.jwt_key)}`, Origin: fixture.origin };
    await fixture.start();
    assert.equal((await request('/api/session', { headers: authenticated })).status, 401);
    assert.equal((await request('/api/session', { headers: expiring })).status, 200);
    const expiringSocket = await upgrade(expiring);
    assert.equal(expiringSocket.status, 101);
    try {
        assert.equal((await expiringSocket.frame()).opcode, 1);
        const closing = await expiringSocket.frame();
        assert.equal(closing.opcode, 8);
        assert.equal(closing.payload.readUInt16BE(), 1008);
    } finally {
        expiringSocket.socket.destroy();
    }
    assert.equal((await request('/api/session', { headers: expiring })).status, 401);
    assert.equal((await upgrade(expiring)).status, 401);
    assert.deepEqual((await readdir(fixture.cwd, { recursive: true })).sort(), diskPaths);
});

test('administrator changes require the current password and revoke active sessions', async t => {
    const fixture = await authFixture(t);
    const { request, post, upgrade } = client(fixture);
    const original = { username: 'fixture-admin', password: 'fixture operator passphrase' };
    const renamed = { username: 'fixture-renamed', current_password: original.password, password: '' };
    await fixture.start();
    assert.equal((await post('setup', original)).status, 201);
    const oldHash = JSON.parse(await readFile(fixture.database, 'utf8')).admin[0].password;

    assert.equal((await post('admin', renamed)).status, 401);
    const first = { Cookie: assertCookie(await post('sign-in', original), expirySeconds), Origin: fixture.origin };
    assert.equal((await post('admin', { ...renamed, current_password: 'wrong passphrase' }, first)).status, 403);
    assert.equal((await post('admin', { ...renamed, username: 'bad/username' }, first)).status, 400);
    assert.equal((await post('admin', renamed, first)).status, 429);
    await delay(1100);
    assert.deepEqual(JSON.parse(await readFile(fixture.database, 'utf8')).admin,
        [{ username: original.username, password: oldHash }]);
    const ws = await upgrade(first, '/ws/preview');
    assert.equal(ws.status, 101);
    try {
        assert.equal((await ws.frame()).opcode, 1);
        const changed = await post('admin', renamed, first);
        assert.equal(changed.status, 204);
        assertCookie(changed, 0);
        let frame;
        do {
            frame = await ws.frame();
        } while (frame.opcode === 1);
        assert.equal(frame.opcode, 8);
        assert.equal(frame.payload.readUInt16BE(), 1008);
    } finally {
        ws.socket.destroy();
    }
    assert.equal((await request('/api/session', { headers: first })).status, 401);
    const config = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.deepEqual(config.admin, [{ username: renamed.username, password: oldHash }]);
    assert.deepEqual(config.auth.sessions, []);

    const login = await post('sign-in', { username: renamed.username, password: original.password });
    const second = { Cookie: assertCookie(login, expirySeconds), Origin: fixture.origin };
    const updatedPassword = 'fixture replacement passphrase';
    assert.equal((await post('admin', { ...renamed, password: updatedPassword }, second)).status, 204);
    const configAfterUpdate = JSON.parse(await readFile(fixture.database, 'utf8'));
    assert.notEqual(configAfterUpdate.admin[0].password, oldHash);
    assert.match(configAfterUpdate.admin[0].password, /^\$argon2id\$/);
    await fixture.stop();
    // A manually provisioned valid hash does not require a pre-existing JWT signing key.
    delete configAfterUpdate.auth;
    await writeFile(fixture.database, JSON.stringify(configAfterUpdate));
    await fixture.start();
    assert.equal((await request('/setup')).headers.location, '/sign-in');
    assert.equal((await post('sign-in', { username: renamed.username, password: updatedPassword })).status, 204);
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8')).auth.sessions.length, 1);
});
