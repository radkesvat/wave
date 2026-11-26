const assert = require('node:assert/strict');
const { createHmac, randomBytes } = require('node:crypto');
const { once } = require('node:events');
const { readFile, writeFile } = require('node:fs/promises');
const https = require('node:https');
const { join } = require('node:path');
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

function clientFrame(opcode, data) {
    const payload = Buffer.from(data);
    assert.ok(payload.length < 126);
    const mask = randomBytes(4);
    const maskedPayload = payload.map((byte, index) => byte ^ mask[index % mask.length]);
    return Buffer.concat([Buffer.from([0x80 | opcode, 0x80 | payload.length]), mask, maskedPayload]);
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
    const credentials = { username: 'fixture-admin', password: 'fake fixture passphrase' };

    await fixture.start();
    await assert.rejects(request('/setup', { ca: [] }), { code: 'UNABLE_TO_GET_ISSUER_CERT_LOCALLY' });
    await assert.rejects(request('/setup', { hostname: 'nested.panel.example.test' }),
        { code: 'ERR_TLS_CERT_ALTNAME_INVALID' });
    const entry = await request('/panel');
    assert.equal(entry.status, 303);
    assert.equal(entry.headers.location, '/setup');
    assert.equal((await request('/', { headers: { Host: 'other.example.invalid' } })).status, 403);
    assert.equal((await request('/api/session')).status, 401);
    assert.equal((await upgrade({ Origin: fixture.origin }, '/ws/preview')).status, 401);
    assert.equal((await post('setup', credentials, { 'X-Wave-CSRF': '0' })).status, 403);
    assert.equal((await post('setup', credentials, { Origin: 'https://other.example.invalid' })).status, 403);
    const setup = await Promise.all([post('setup', credentials), post('setup', credentials)]);
    assert.deepEqual(setup.map(response => response.status).sort(), [201, 409]);
    assert.equal((await request('/panel')).headers.location, '/sign-in');

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
    assert.deepEqual(JSON.parse(retained.body), { type: 'session', username: claims.sub, expires_at: claims.exp });
    assert.equal(retained.headers['cache-control'], 'no-store');

    const ws = await upgrade(authenticated);
    assert.equal(ws.status, 101);
    const terminal = process.platform === 'linux' ? await upgrade(authenticated, '/ws/console') : null;
    try {
        assert.equal((await ws.frame()).opcode, 1);
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

test('closes the TLS terminal when the shell exits during a final keystroke',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await authFixture(t);
        const { post, upgrade } = client(fixture);
        const credentials = { username: 'fixture-admin', password: 'fixture terminal passphrase' };
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
    const original = { username: 'fixture-admin', password: 'fixture operator passphrase' };
    const change = { username: 'fixture-renamed', current_password: original.password,
        password: 'fixture replacement passphrase' };
    await fixture.start();
    assert.equal((await post('setup', original)).status, 201);
    const oldHash = JSON.parse(await readFile(fixture.database, 'utf8')).admin[0].password;

    assert.equal((await post('admin', change)).status, 401);
    const first = { Cookie: assertCookie(await post('sign-in', original), expirySeconds), Origin: fixture.origin };
    const second = { Cookie: assertCookie(await post('sign-in', original), expirySeconds), Origin: fixture.origin };
    const ws = await upgrade(second, '/ws/preview');
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
