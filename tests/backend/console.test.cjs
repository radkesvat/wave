const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const { once } = require('node:events');
const { writeFile } = require('node:fs/promises');
const { request } = require('node:http');
const { dirname, join } = require('node:path');
const { test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { authFixture, timeout } = require('./auth_fixture.cjs');

async function administrator(t) {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const credentials = { username: 'fixture-admin', password: 'console fixture passphrase' };
    function post(action, data = {}, cookie) {
        const headers = { Origin: fixture.origin, 'Content-Type': 'application/json', 'X-Wave-CSRF': '1' };
        if (cookie) headers.Cookie = cookie;
        return fetch(new URL(`/api/auth/${action}`, fixture.origin), {
            method: 'POST', headers, body: JSON.stringify(data), signal: AbortSignal.timeout(timeout),
        });
    }
    assert.equal((await post('setup', credentials)).status, 201);
    const login = await post('sign-in', credentials);
    assert.equal(login.status, 204);
    return { ...fixture, post, cookie: login.headers.get('set-cookie').split(';', 1)[0] };
}

function upgradeStatus(origin, headers) {
    return new Promise((resolve, reject) => {
        const probe = request(new URL('/ws/console', origin), {
            headers: {
                Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13',
                'Sec-WebSocket-Key': randomBytes(16).toString('base64'), ...headers,
            },
            signal: AbortSignal.timeout(timeout),
        });
        probe.on('response', response => {
            response.resume();
            resolve(response.statusCode);
        });
        probe.on('upgrade', (response, socket) => {
            socket.destroy();
            resolve(response.statusCode);
        });
        probe.on('error', reject);
        probe.end();
    });
}

test('requires an administrator session and exact origin to upgrade the console', async t => {
    const { origin, cookie } = await administrator(t);
    assert.equal(await upgradeStatus(origin, { Origin: origin }), 401);
    assert.equal(await upgradeStatus(origin, { Cookie: cookie }), 403);
    assert.equal(await upgradeStatus(origin, { Cookie: cookie, Origin: 'https://other.example.test' }), 403);
});

async function until(check, failure) {
    const deadline = Date.now() + timeout;
    do {
        const result = check();
        if (result) return result;
        await delay(20);
    } while (Date.now() < deadline);
    assert.fail(failure());
}

async function consoleTerminal(t, { origin, cookie }, cols, rows) {
    const endpoint = new URL('/ws/console', origin);
    endpoint.protocol = 'ws:';
    const socket = new WebSocket(endpoint, { headers: { Cookie: cookie, Origin: origin } });
    socket.binaryType = 'arraybuffer';
    t.after(() => socket.close());

    const output = [];
    const statuses = [];
    let closeEvent;
    socket.addEventListener('message', ({ data }) => {
        if (typeof data === 'string') statuses.push(JSON.parse(data));
        else output.push(Buffer.from(data));
    });
    socket.addEventListener('close', event => { closeEvent = event; });

    function resize(cols, rows) {
        socket.send(JSON.stringify({ type: 'resize', cols, rows }));
    }
    function expectOutput(pattern) {
        return until(() => {
            const bytes = Buffer.concat(output);
            if (Buffer.isBuffer(pattern)) return bytes.includes(pattern);
            return pattern.exec(bytes.toString('utf8'));
        }, () => `Expected ${pattern} in terminal output ${JSON.stringify(Buffer.concat(output).toString('utf8'))}` +
            ` with status messages ${JSON.stringify(statuses)} and close code ${closeEvent?.code}`);
    }

    await once(socket, 'open', { signal: AbortSignal.timeout(timeout) });
    resize(cols, rows);
    const expectStatus = type => until(() => statuses.find(status => status.type === type),
        () => `Expected terminal status ${type}, got ${JSON.stringify(statuses)}`);
    await expectStatus('ready');
    const send = data => socket.send(Buffer.from(data));
    send('unset HISTFILE\r');
    return {
        resize, expectOutput, send, expectStatus,
        close: () => socket.close(),
        closed: () => until(() => closeEvent, () => 'Expected the terminal socket to close'),
    };
}

function reaped(pid) {
    return until(() => {
        try {
            process.kill(pid, 0);
            return false;
        } catch (error) {
            assert.equal(error.code, 'ESRCH');
            return true;
        }
    }, () => `Expected terminal shell ${pid} to be reaped`);
}

test('streams an interactive PTY, resizes foreground apps, and reaps closed sessions',
    { skip: process.platform !== 'linux' }, async t => {
        const fixture = await administrator(t);
        const first = await consoleTerminal(t, fixture, 80, 24);
        first.send('if [ -t 0 ] && [ -t 1 ] && [ -t 2 ]; then ' +
            'printf "WAVE-PTY:%s:%s:%s:%s\n" "$$" "$TERM" "$(pwd)" "$(stty size)"; fi\r');
        const [, firstPid, term, directory, size] = await first.expectOutput(/WAVE-PTY:(\d+):([^:\r]+):([^:\r]+):([^\r]+)\r\n/);
        assert.equal(term, 'xterm-256color');
        assert.equal(directory, dirname(fixture.backend));
        assert.equal(size, '24 80');

        const second = await consoleTerminal(t, fixture, 91, 26);
        second.send('printf "WAVE-ISOLATED:%s:%s\n" "$$" "$(stty size)"\r');
        const [, secondPid] = await second.expectOutput(/WAVE-ISOLATED:(\d+):26 91\r\n/);
        assert.notEqual(secondPid, firstPid);
        second.close();
        await second.closed();
        await reaped(Number(secondPid));

        const foreground = [
            `trap 'printf "WAVE-RESIZE:%s\\n" "$(stty size)"' WINCH`,
            `trap 'printf "WAVE-INTERRUPTED\\n"; exit' INT`,
            `printf 'WAVE-FOREGROUND\\n'`,
            'while :; do sleep 1; done', '',
        ].join('\n');
        await writeFile(join(directory, 'terminal-probe.sh'), foreground);
        first.send('sh terminal-probe.sh\r');
        await first.expectOutput(/WAVE-FOREGROUND\r\n/);
        first.resize(117, 34);
        await first.expectOutput(/WAVE-RESIZE:34 117\r\n/);
        first.send([0x03]);
        await first.expectOutput(/WAVE-INTERRUPTED\r\n/);

        first.send("stty raw -echo; printf 'WAVE-RAW-%s\\n' READY; dd bs=1 count=6 2>/dev/null; " +
            "stty sane; printf 'WAVE-RAW-%s\\n' DONE\r");
        await first.expectOutput(/WAVE-RAW-READY\n/);
        const raw = Buffer.from([0x00, 0x1b, 0x5b, 0x5a, 0x80, 0xff]);
        first.send(raw);
        await first.expectOutput(raw);
        await first.expectOutput(/WAVE-RAW-DONE\r\n/);

        first.send("printf '\\033[?1049h\\033[2J\\033[3;12H\\033[31mWAVE\\033[0m\\033[?1049l'\r");
        await first.expectOutput(Buffer.from('\x1b[?1049h\x1b[2J\x1b[3;12H\x1b[31mWAVE\x1b[0m\x1b[?1049l'));

        const exiting = await consoleTerminal(t, fixture, 80, 24);
        exiting.send('exit\r');
        await exiting.expectStatus('exit');
        assert.equal((await exiting.closed()).code, 1000);

        assert.equal((await fixture.post('sign-out', {}, fixture.cookie)).status, 204);
        assert.equal((await first.closed()).code, 1008);
        await reaped(Number(firstPid));
        assert.equal(await upgradeStatus(fixture.origin, { Cookie: fixture.cookie, Origin: fixture.origin }), 401);
    });
