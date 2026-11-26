const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { mkdir, mkdtemp, rm, writeFile } = require('node:fs/promises');
const { createServer } = require('node:net');
const { tmpdir } = require('node:os');
const { dirname, join, resolve } = require('node:path');
const { createInterface } = require('node:readline');
const { bindHost, copyBackend, hostname, timeout, tlsFixture, writeTlsConfig } = require('./tls_fixture.cjs');

async function loopbackPort() {
    const listener = createServer();
    listener.listen(0, bindHost);
    await once(listener, 'listening');
    const { port } = listener.address();
    const closed = once(listener, 'close');
    listener.close();
    await closed;
    return port;
}

function fixtureWaterWall() {
    const { readFileSync, writeFileSync } = require('node:fs');
    const args = process.argv.slice(2);
    if (args.length !== 1 || args[0] !== '--config:stdin') {
        process.exit(2);
    }
    const settings = JSON.parse(readFileSync(0, 'utf8'));
    if (!Array.isArray(settings.configs) || !settings.configs.length) {
        process.exit(2);
    }
    for (const file of settings.configs) JSON.parse(readFileSync(file, 'utf8'));
    writeFileSync('fixture-process.json', JSON.stringify({ pid: process.pid, args, cwd: process.cwd(), settings }));
    const parent = process.ppid;
    setInterval(() => {
        try {
            process.kill(parent, 0);
        } catch {
            process.exit(0);
        }
    }, 100);
}

async function seedCore(backend, { executable = false } = {}) {
    const directory = dirname(backend);
    const bin = join(directory, 'waterwall_bin');
    const configDir = join(directory, 'configs');
    await mkdir(bin, { recursive: true });
    await mkdir(configDir, { recursive: true });
    const program = join(bin, 'Waterwall');
    if (executable) {
        await writeFile(program, '#!/usr/bin/env node\n(' + fixtureWaterWall + ')();\n', { mode: 0o700 });
    }
    return { bin, configDir, program, processFile: join(directory, 'fixture-process.json') };
}

async function authFixture(t, webSite, { encrypted = true, prepareCore = true } = {}) {
    assert.ok(process.env.WAVE_BACKEND, 'Set WAVE_BACKEND to a native Wave build.');
    const cwd = await mkdtemp(join(tmpdir(), 'wave-auth-test-'));
    let server;
    let closed;

    async function stop() {
        if (server) {
            server.kill();
            await closed;
            server = undefined;
        }
    }

    t.after(async () => {
        try {
            await stop();
        } finally {
            await rm(cwd, { recursive: true, force: true });
        }
    });

    const backend = await copyBackend(resolve(process.env.WAVE_BACKEND), cwd);
    if (prepareCore) await seedCore(backend);
    const siteDir = webSite ? resolve(webSite) : join(cwd, 'site');
    const index = '<!doctype html><title>Wave auth fixture</title>';
    if (!webSite) {
        await mkdir(siteDir);
        await writeFile(join(siteDir, 'index.html'), index);
        await writeFile(join(siteDir, 'index.js'), '');
        await writeFile(join(siteDir, 'index.wasm'), '');
    }
    const port = await loopbackPort();
    const database = join(dirname(backend), 'wave-db.json');
    const listen = { host: bindHost, port };
    const tls = encrypted ? await tlsFixture(cwd) : undefined;
    const installAttempted = prepareCore ? true : undefined;
    if (tls) {
        await writeTlsConfig(database, tls, { listen, installAttempted });
    } else {
        await writeFile(database, JSON.stringify({ listen, waterwall_install_attempted: installAttempted }),
            { mode: 0o600 });
    }
    const origin = encrypted ? `https://${hostname}:${port}` : `http://${bindHost}:${port}`;
    const args = ['--site-dir', siteDir];

    function lookup(_name, options, callback) {
        if (options.all) {
            callback(null, [{ address: bindHost, family: 4 }]);
        } else {
            callback(null, bindHost, 4);
        }
    }

    async function start(env = process.env) {
        server = spawn(backend, args, { cwd, env, stdio: ['ignore', 'pipe', 'pipe'] });
        closed = once(server, 'close');
        let errors = '';
        server.stderr.setEncoding('utf8').on('data', data => { errors += data; });
        const lines = createInterface({ input: server.stdout });
        try {
            const [line] = await Promise.race([
                once(lines, 'line', { signal: AbortSignal.timeout(timeout) }),
                closed.then(([code, signal]) => { throw new Error(`Fixture stopped (${signal ?? code}). ${errors}`); }),
            ]);
            assert.equal(line, encrypted ? `Authenticated HTTPS listening on port ${port}`
                : `Listening on ${origin}/`);
        } finally {
            lines.close();
        }
    }

    return { cwd, backend, ca: tls?.ca, rootPath: tls?.rootPath, hostname, bindHost, lookup, origin,
        index, database, start, stop };
}

module.exports = { authFixture, seedCore, timeout };
