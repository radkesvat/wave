const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { chmod, chown, mkdir, mkdtemp, rm, writeFile } = require('node:fs/promises');
const { createServer } = require('node:net');
const { tmpdir } = require('node:os');
const { delimiter, dirname, join, resolve } = require('node:path');
const { createInterface } = require('node:readline');
const { bindHost, copyBackend, coreReleaseDir, hostname, timeout, tlsFixture, writeTlsConfig } =
    require('./tls_fixture.cjs');

const panelBasePath = '/panel';
const catalogTools = 'catalog tools';

async function emptyCatalogPath(cwd) {
    const bin = join(cwd, catalogTools);
    await mkdir(bin);
    await writeFile(join(bin, 'curl'), [
        '#!/bin/sh',
        'for url do :; done',
        'case "$url" in',
        '  "https://api.github.com/repos/radkesvat/wave/contents/backend/schematics?ref=main") printf "[]" ;;',
        '  *) exit 22 ;;',
        'esac',
        '',
    ].join('\n'), { mode: 0o700 });
    return bin + delimiter + process.env.PATH;
}

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

function rotatingLogPath(settings, source) {
    const log = settings.log ?? {};
    const now = new Date();
    const day = [now.getFullYear(), now.getMonth() + 1, now.getDate()]
        .map(value => String(value).padStart(2, '0')).join('');
    const file = (log[source]?.file ?? `${source}.log`).replace(/\.log$/, '');
    return `${log.path ?? 'log/'}${file}.${day}.log`;
}

function fixtureWaterWall(logs, observeShutdown) {
    const { appendFileSync, mkdirSync, readFileSync, writeFileSync } = require('node:fs');
    const { dirname } = require('node:path');
    const args = process.argv.slice(2);
    if (args.length !== 1 || args[0] !== '--config:stdin') {
        process.exit(2);
    }
    const settings = JSON.parse(readFileSync(0, 'utf8'));
    if (!Array.isArray(settings.configs) || !settings.configs.length) {
        process.exit(2);
    }
    if (observeShutdown) {
        process.on('SIGTERM', () => {
            writeFileSync('fixture-terminated', String(process.pid));
            setTimeout(() => process.exit(0), 350);
        });
    }
    for (const file of settings.configs) JSON.parse(readFileSync(file, 'utf8'));
    for (const { source, text } of logs) {
        const file = rotatingLogPath(settings, source);
        mkdirSync(dirname(file), { recursive: true });
        appendFileSync(file, text);
    }
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

async function seedCore(backend, { executable = false, logs = [], observeShutdown = false } = {}) {
    const directory = dirname(backend);
    const releaseDir = coreReleaseDir(backend);
    const configDir = join(directory, 'configs');
    await mkdir(releaseDir, { recursive: true, mode: 0o755 });
    await mkdir(configDir, { recursive: true, mode: 0o755 });
    const program = join(releaseDir, 'Waterwall');
    if (executable) {
        const args = `${JSON.stringify(logs)}, ${observeShutdown}`;
        await writeFile(program, `#!/usr/bin/env node\n${rotatingLogPath}\n(${fixtureWaterWall})(${args});\n`,
            { mode: 0o700 });
    }
    return { configDir, program, processFile: join(directory, 'fixture-process.json'),
        terminatedFile: join(directory, 'fixture-terminated') };
}

async function authFixture(t, webSite, {
    encrypted = true, prepareCore = true, basePath = panelBasePath, runAs,
} = {}) {
    assert.ok(process.env.WAVE_BACKEND, 'Set WAVE_BACKEND to a native Wave build.');
    const cwd = await mkdtemp(join(tmpdir(), 'wave-auth-test-'));
    let server;
    let closed;
    let stderr = '';

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

    const home = join(cwd, 'home');
    await mkdir(home);
    const fixturePath = await emptyCatalogPath(cwd);
    const backend = await copyBackend(resolve(process.env.WAVE_BACKEND), cwd);
    const core = prepareCore ? await seedCore(backend) : undefined;
    const siteDir = webSite ? resolve(webSite) : join(cwd, 'site');
    const index = '<!doctype html><title>Wave auth fixture</title>';
    const siteAssets = { 'index.html': index, 'index.js': '', 'index.wasm': '' };
    if (!webSite) {
        await mkdir(siteDir);
        for (const [file, contents] of Object.entries(siteAssets)) {
            await writeFile(join(siteDir, file), contents);
        }
    }
    const port = await loopbackPort();
    const database = join(dirname(backend), 'wave-db.json');
    const listen = { host: bindHost, port };
    const tls = encrypted ? await tlsFixture(cwd) : undefined;
    const installAttempted = prepareCore ? true : undefined;
    if (tls) {
        await writeTlsConfig(database, tls, { listen, installAttempted, basePath });
    } else {
        await writeFile(database, JSON.stringify({
            listen, 'panel-base-path': basePath ?? undefined, waterwall_install_attempted: installAttempted,
        }), { mode: 0o600 });
    }
    if (runAs) {
        const tools = join(cwd, catalogTools);
        const permissions = [
            [cwd, 0o700], [home, 0o700], [dirname(backend), 0o700], [backend, 0o700],
            [database, 0o600], [tools, 0o700], [join(tools, 'curl'), 0o700],
        ];
        if (core) {
            const release = dirname(core.program);
            permissions.push([dirname(release), 0o700], [release, 0o700], [core.configDir, 0o700]);
        }
        if (!webSite) {
            permissions.push([siteDir, 0o700]);
            for (const file of Object.keys(siteAssets)) permissions.push([join(siteDir, file), 0o600]);
        }
        if (tls) permissions.push([tls.chainPath, 0o600], [tls.keyPath, 0o600]);
        for (const [path, mode] of permissions) {
            await chown(path, runAs.uid, runAs.gid);
            await chmod(path, mode);
        }
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

    async function start(overrides = {}, { encrypted: useTls = encrypted } = {}) {
        // Give interactive shells a clean startup environment and private history.
        const env = {
            PATH: fixturePath,
            ...overrides,
            HOME: home,
            HISTFILE: join(home, '.bash_history'),
        };
        stderr = '';
        server = spawn(backend, args, { cwd, env, ...runAs, stdio: ['ignore', 'pipe', 'pipe'] });
        closed = once(server, 'close');
        server.stderr.setEncoding('utf8').on('data', data => { stderr += data; });
        const lines = createInterface({ input: server.stdout });
        try {
            const [line] = await Promise.race([
                once(lines, 'line', { signal: AbortSignal.timeout(timeout) }),
                closed.then(([code, signal]) => { throw new Error(`Fixture stopped (${signal ?? code}). ${stderr}`); }),
            ]);
            assert.equal(line, useTls ? `Authenticated HTTPS listening on port ${port}`
                : `Listening on http://${bindHost}:${port}/`);
        } finally {
            lines.close();
        }
    }

    return { cwd, backend, ca: tls?.ca, rootPath: tls?.rootPath, hostname, bindHost, lookup, origin, basePath,
        index, database, start, stop, get stderr() { return stderr; } };
}

module.exports = { authFixture, emptyCatalogPath, panelBasePath, rotatingLogPath, seedCore, timeout };
