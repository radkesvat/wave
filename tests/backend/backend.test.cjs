const assert = require('node:assert/strict');
const { execFile, spawn } = require('node:child_process');
const { once } = require('node:events');
const { mkdir, mkdtemp, rm, writeFile } = require('node:fs/promises');
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
