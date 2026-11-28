const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const { randomBytes } = require('node:crypto');
const { once } = require('node:events');
const { chown, mkdir, readFile, readdir, readlink, stat, unlink, writeFile } = require('node:fs/promises');
const { dirname, join } = require('node:path');
const { test } = require('node:test');
const { promisify } = require('node:util');
const { crc32, inflateRawSync } = require('node:zlib');
const { authFixture, fixtureSession, nextPreviewSnapshot, openPreview, requestPreview, seedCore, timeout } =
    require('./auth_fixture.cjs');
const { bundledSchematicFile, schematicFile, tlsFixture } = require('./tls_fixture.cjs');

const archiveVersion = '1.6.1';

function call(fixture, route, { session, body, type = 'application/json', headers = {}, ...options } = {}) {
    return fetch(`${fixture.origin}${fixture.basePath}${route}`, {
        ...(body === undefined ? {} : { method: 'POST', body }),
        headers: {
            ...(session ? { Cookie: session } : {}),
            ...(body === undefined ? {} : { Origin: fixture.origin, 'X-Wave-CSRF': '1', 'Content-Type': type }),
            ...headers,
        },
        signal: AbortSignal.timeout(timeout),
        ...options,
    });
}

const prepare = (fixture, session, body) => call(fixture, '/api/backup/prepare', {
    session, body, type: 'application/zip',
});
const apply = (fixture, session, token) => call(fixture, '/api/backup/apply', {
    session, body: JSON.stringify({ token }),
});

async function readReply(response, ok) {
    const reply = await response.json();
    assert.equal(response.ok, ok, reply.error);
    assert.equal(reply.ok, ok);
    if (!ok) assert.ok(reply.error);
    return reply;
}

async function provisionRelease(fixture) {
    const target = schematicFile(fixture.backend, archiveVersion);
    const source = await readFile(bundledSchematicFile, 'utf8');
    await mkdir(dirname(target), { mode: 0o700, recursive: true });
    await writeFile(target, source.replace('"version": "1.6.0"', `"version": "${archiveVersion}"`), { mode: 0o600 });
}

function readZip(archive) {
    // Wave emits ordinary ZIP files without a ZIP comment or ZIP64 records.
    const end = archive.length - 22;
    assert.equal(archive.readUInt32LE(end), 0x06054b50);
    const count = archive.readUInt16LE(end + 10);
    let offset = archive.readUInt32LE(end + 16);
    const entries = new Map();
    for (let index = 0; index < count; ++index) {
        assert.equal(archive.readUInt32LE(offset), 0x02014b50);
        const method = archive.readUInt16LE(offset + 10);
        const size = archive.readUInt32LE(offset + 20);
        const local = archive.readUInt32LE(offset + 42);
        const nameSize = archive.readUInt16LE(offset + 28);
        const name = archive.toString('utf8', offset + 46, offset + 46 + nameSize);
        assert.equal(archive.readUInt32LE(local), 0x04034b50);
        const start = local + 30 + archive.readUInt16LE(local + 26) + archive.readUInt16LE(local + 28);
        const stored = archive.subarray(start, start + size);
        assert.ok(method === 0 || method === 8);
        entries.set(name, method === 8 ? inflateRawSync(stored) : stored);
        offset += 46 + nameSize + archive.readUInt16LE(offset + 30) + archive.readUInt16LE(offset + 32);
    }
    return entries;
}

function writeZip(entries) {
    const files = [];
    const directory = [];
    let fileBytes = 0;
    for (const [path, body] of entries) {
        const name = Buffer.from(path);
        const crc = crc32(body);
        const local = Buffer.alloc(30);
        local.writeUInt32LE(0x04034b50);
        local.writeUInt16LE(20, 4);
        local.writeUInt16LE(0x0800, 6);
        local.writeUInt32LE(crc, 14);
        local.writeUInt32LE(body.length, 18);
        local.writeUInt32LE(body.length, 22);
        local.writeUInt16LE(name.length, 26);
        const record = Buffer.alloc(46);
        record.writeUInt32LE(0x02014b50);
        record.writeUInt16LE(20, 4);
        local.copy(record, 6, 4, 28);
        record.writeUInt32LE(fileBytes, 42);
        files.push(local, name, body);
        directory.push(record, name);
        fileBytes += local.length + name.length + body.length;
    }
    const end = Buffer.alloc(22);
    end.writeUInt32LE(0x06054b50);
    end.writeUInt16LE(entries.size, 8);
    end.writeUInt16LE(entries.size, 10);
    end.writeUInt32LE(directory.reduce((length, chunk) => length + chunk.length, 0), 12);
    end.writeUInt32LE(fileBytes, 16);
    return Buffer.concat([...files, ...directory, end]);
}

function withManifest(entries, manifest) {
    return writeZip(new Map(entries).set('manifest.json', Buffer.from(JSON.stringify(manifest))));
}

test('ZIP routes require a session, check the upload origin, and keep JSON request limits', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false, basePath: '/workspace/settings' });
    await fixture.start();
    const session = await fixtureSession(fixture);
    assert.equal((await call(fixture, '/api/backup')).status, 401);
    assert.equal((await prepare(fixture, undefined, 'bad archive')).status, 401);
    assert.equal((await apply(fixture, undefined, 'token')).status, 401);
    for (const headers of [
        { Origin: 'http://other.invalid' },
        { 'X-Wave-CSRF': '0' },
        { 'Content-Type': 'application/json' },
    ]) {
        const reply = await call(fixture, '/api/backup/prepare', {
            session, body: 'bad archive', type: 'application/zip', headers,
        });
        assert.equal(reply.status, 403);
    }
    assert.equal((await call(fixture, '/api/backup/apply', {
        session, body: JSON.stringify({ token: 'token' }), headers: { 'X-Wave-CSRF': '0' },
    })).status, 403);

    const tooLarge = Buffer.alloc(4097, ' ');
    assert.equal((await call(fixture, '/api/auth/sign-in', { body: tooLarge })).status, 413);
    const chunked = buffer => new ReadableStream({
        start(controller) {
            controller.enqueue(buffer);
            controller.close();
        },
    });
    assert.equal((await call(fixture, '/api/auth/sign-in', {
        body: chunked(Buffer.from('{}')), duplex: 'half',
    })).status, 401);
    assert.equal((await call(fixture, '/api/auth/sign-in', { body: chunked(tooLarge), duplex: 'half' })).status, 413);
});

test('keeps the restore proposal when a release check temporarily blocks Apply', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const directory = dirname(fixture.backend);
    const tools = join(directory, 'held-catalog');
    const hold = join(directory, 'hold-catalog');
    await mkdir(tools);
    await writeFile(join(tools, 'curl'), [
        '#!/bin/sh',
        'while test -f "$WAVE_BACKUP_TEST_HOLD_CATALOG"; do sleep 0.02; done',
        'printf "[]"',
        '',
    ].join('\n'), { mode: 0o700 });
    await fixture.start({ PATH: `${tools}:${process.env.PATH}`, WAVE_BACKUP_TEST_HOLD_CATALOG: hold });
    const session = await fixtureSession(fixture);
    const { socket } = await openPreview(t, fixture.origin, session);
    const download = await call(fixture, '/api/backup', { session });
    assert.equal(download.status, 200);
    const proposal = await readReply(await prepare(fixture, session, await download.arrayBuffer()), true);

    await writeFile(hold, '');
    try {
        const check = await requestPreview(socket, 'check_core_updates');
        assert.equal(check.ok, true, check.feedback);
        assert.equal(check.data.server.core.checking_updates, true);
        const response = await apply(fixture, session, proposal.token);
        assert.equal(response.status, 409);
        const busy = await readReply(response, false);
        assert.equal(busy.retryable, true);
        assert.match(busy.error, /release check/i);
        const finished = nextPreviewSnapshot(socket, frame => !frame.data.server.core.checking_updates);
        await unlink(hold);
        await finished;
        await readReply(await apply(fixture, session, proposal.token), true);
    } finally {
        await unlink(hold).catch(error => {
            if (error.code !== 'ENOENT') throw error;
        });
    }
});

test('restores the ZIP version, saved core settings, config bytes and referenced databases', async t => {
    const source = await authFixture(t, undefined, { encrypted: false });
    await provisionRelease(source);
    const sourceDb = JSON.parse(await readFile(source.database, 'utf8'));
    await writeFile(source.database, JSON.stringify({ ...sourceDb, waterwall_version: archiveVersion }));
    await source.start();
    const sourceSession = await fixtureSession(source);
    const { socket } = await openPreview(t, source.origin, sourceSession);
    const settings = { misc: { workers: 3 }, dns: { 'hosts-path': 'custom-hosts.fixture' } };
    const settingsReply = await requestPreview(socket, 'apply_core_settings', { settings });
    assert.equal(settingsReply.ok, true, settingsReply.feedback);

    const name = 'Office workspace';
    const created = await requestPreview(socket, 'create_core_config', { name, add_auth_nodes: true });
    assert.equal(created.ok, true, created.feedback);
    const [{ id }] = created.data.server.core.configs;
    const enabled = await requestPreview(socket, 'set_core_config_enabled', { id, enabled: true });
    assert.equal(enabled.ok, true, enabled.feedback);
    const { result: { config } } = await requestPreview(socket, 'get_core_config', { id });
    const databasePath = config.nodes.find(node => node.type === 'AuthenticationServer').settings['db-path'];
    const configPath = join('configs', name, 'config.json');
    const sourceDirectory = dirname(source.backend);
    const configBytes = await readFile(join(sourceDirectory, configPath));
    const userBytes = Buffer.from(`{\n "users": [{"id": 9007199254740993, "password": "fixture-secret",` +
        ` "stats": {"traffic": {"u": "18446744073709551614"}}}],\n "note": "${randomBytes(8192).toString('hex')}"\n}\n`);
    const recoveryBytes = Buffer.from('{"users":[{"id":"12","password":"older-fixture-secret"}]}\n');
    await writeFile(join(sourceDirectory, databasePath), userBytes);
    await writeFile(join(sourceDirectory, `${databasePath}.backup`), recoveryBytes, { mode: 0o600 });
    await writeFile(join(sourceDirectory, 'configs', name, 'local-note.txt'), 'Source auxiliary file');

    const download = await call(source, '/api/backup', { session: sourceSession });
    assert.equal(download.status, 200, download.ok ? undefined : (await download.json()).error);
    assert.equal(download.headers.get('content-type'), 'application/zip');
    assert.match(download.headers.get('content-disposition'), /^attachment;.+\.zip/i);
    assert.equal(download.headers.get('cache-control'), 'no-store');
    const downloaded = Buffer.from(await download.arrayBuffer());
    assert.equal(downloaded.subarray(0, 4).toString('hex'), '504b0304');
    assert.ok(downloaded.length > 4096, 'The ZIP exceeds the authentication route payload limit.');
    const entries = readZip(downloaded);
    const legacyManifest = JSON.parse(entries.get('manifest.json'));
    delete legacyManifest.panel_settings;
    delete legacyManifest.wave_version;
    const archive = withManifest(entries, { ...legacyManifest, version: 1 });
    await writeFile(join(sourceDirectory, databasePath), '{"users":[{}]}');
    await writeFile(join(sourceDirectory, `${databasePath}.backup`), 'invalid recovery');
    await readReply(await call(source, '/api/backup', { session: sourceSession }), false);
    await writeFile(join(sourceDirectory, databasePath), userBytes);

    const destination = await authFixture(t, undefined, { encrypted: false, basePath: '/destination' });
    await seedCore(destination.backend, { executable: true });
    await destination.start();
    const destinationSession = await fixtureSession(destination);
    const destinationView = await openPreview(t, destination.origin, destinationSession, destination.basePath);
    const old = await requestPreview(destinationView.socket, 'create_core_config', { name });
    assert.equal(old.ok, true, old.feedback);
    const extraName = 'Only on destination';
    const extra = await requestPreview(destinationView.socket, 'create_core_config', { name: extraName });
    assert.equal(extra.ok, true, extra.feedback);
    const destinationDirectory = dirname(destination.backend);
    const extraPath = join(destinationDirectory, 'configs', extraName, 'config.json');
    const extraBytes = await readFile(extraPath);
    const originalConfig = await readFile(join(destinationDirectory, configPath));
    const localNote = join(destinationDirectory, 'configs', name, 'retained-note.txt');
    await writeFile(localNote, 'Destination auxiliary file');
    const otherSession = await fixtureSession(destination, { setup: false });
    const stored = await readFile(destination.database, 'utf8');

    await readReply(await prepare(destination, destinationSession, Buffer.from('invalid ZIP')), false);
    const unavailable = await readReply(await prepare(destination, destinationSession, archive), false);
    assert.match(unavailable.error, /1\.6\.1|release|schematic/i);
    assert.equal(await readFile(destination.database, 'utf8'), stored);
    assert.deepEqual(await readFile(join(destinationDirectory, configPath)), originalConfig);

    await provisionRelease(destination);
    const stale = await readReply(await prepare(destination, destinationSession, archive), true);
    const [destinationConfig] = old.data.server.core.configs;
    const changeEnabled = enabled => requestPreview(destinationView.socket, 'set_core_config_enabled', {
        id: destinationConfig.id, enabled,
    });
    assert.equal((await changeEnabled(true)).ok, true);
    assert.equal((await apply(destination, destinationSession, stale.token)).ok, false);
    assert.deepEqual(await readFile(join(destinationDirectory, configPath)), originalConfig);
    const running = nextPreviewSnapshot(destinationView.socket, frame => frame.data.server.core.state === 'running');
    const started = await requestPreview(destinationView.socket, 'start_core');
    assert.equal(started.ok, true, started.feedback);
    await running;
    const activeBackup = await readReply(await call(destination, '/api/backup', { session: destinationSession }), false);
    assert.match(activeBackup.error, /stop WaterWall/i);
    const activeRestore = await readReply(await prepare(destination, destinationSession, archive), false);
    assert.match(activeRestore.error, /stop WaterWall/i);
    const stopped = nextPreviewSnapshot(destinationView.socket, frame => frame.data.server.core.state === 'stopped');
    assert.equal((await requestPreview(destinationView.socket, 'stop_core')).ok, true);
    await stopped;
    assert.equal((await changeEnabled(false)).ok, true);

    const edited = await readReply(await prepare(destination, destinationSession, archive), true);
    const changedOnDisk = Buffer.concat([originalConfig, Buffer.from('\n')]);
    await writeFile(join(destinationDirectory, configPath), changedOnDisk);
    const outdated = await readReply(await apply(destination, destinationSession, edited.token), false);
    assert.match(outdated.error, /files changed/i);
    assert.deepEqual(await readFile(join(destinationDirectory, configPath)), changedOnDisk);
    await writeFile(join(destinationDirectory, configPath), originalConfig);

    const changedRelease = await readReply(await prepare(destination, destinationSession, archive), true);
    const releaseFile = schematicFile(destination.backend, archiveVersion);
    const originalSchematic = await readFile(releaseFile, 'utf8');
    const revisedSchematic = originalSchematic.replace(/"commit": "[a-f0-9]{40}"/, `"commit": "${'f'.repeat(40)}"`);
    assert.notEqual(revisedSchematic, originalSchematic);
    await writeFile(releaseFile, revisedSchematic);
    const staleRelease = await readReply(await apply(destination, destinationSession, changedRelease.token), false);
    assert.match(staleRelease.error, /schematic changed/i);
    assert.deepEqual(await readFile(join(destinationDirectory, configPath)), originalConfig);
    await writeFile(releaseFile, originalSchematic);

    const interrupted = await readReply(await prepare(destination, destinationSession, archive), true);
    await writeFile(destination.database, 'invalid JSON');
    try {
        await readReply(await apply(destination, destinationSession, interrupted.token), false);
        assert.equal(await readFile(destination.database, 'utf8'), 'invalid JSON');
        assert.deepEqual(await readFile(join(destinationDirectory, configPath)), originalConfig);
        await assert.rejects(stat(join(destinationDirectory, databasePath)), { code: 'ENOENT' });
        await assert.rejects(stat(join(destinationDirectory, '.wave-restore')), { code: 'ENOENT' });
    } finally {
        await writeFile(destination.database, stored);
    }

    const proposal = await readReply(await prepare(destination, destinationSession, archive), true);
    assert.equal(proposal.waterwall_version, archiveVersion);
    assert.equal(proposal.current_waterwall_version, '1.6.0');
    assert.equal(proposal.panel_settings, undefined);
    assert.deepEqual(proposal.configs.map(({ name, enabled }) => ({ name, enabled })), [{ name, enabled: true }]);
    assert.deepEqual(proposal.replaced_configs, [name]);
    assert.deepEqual(proposal.removed_configs, [extraName]);
    assert.ok(proposal.missing_dependencies.some(message => message.includes('custom-hosts.fixture')));
    assert.ok(typeof proposal.token === 'string' && proposal.token.length > 0);
    assert.equal(await readFile(destination.database, 'utf8'), stored);
    assert.deepEqual(await readFile(join(destinationDirectory, configPath)), originalConfig);
    const unauthorized = await apply(destination, otherSession, proposal.token);
    assert.equal(unauthorized.ok, false);
    assert.equal(await readFile(destination.database, 'utf8'), stored);

    const changed = nextPreviewSnapshot(destinationView.socket, frame => frame.data.server.core.version === archiveVersion);
    await readReply(await apply(destination, destinationSession, proposal.token), true);
    const restoredView = await changed;
    assert.deepEqual(restoredView.data.core_settings, settings);
    assert.equal(restoredView.data.server.core.configs[0].enabled, true);
    assert.ok(restoredView.data.server.core.available_versions.includes(archiveVersion));
    assert.ok(['stopped', 'missing'].includes(restoredView.data.server.core.state));
    assert.equal((await apply(destination, destinationSession, proposal.token)).ok, false);
    for (const action of ['set_core_config_enabled', 'remove_core_config']) {
        const stale = await requestPreview(destinationView.socket, action, {
            id, enabled: false, schematic_revision: destinationView.schematicRevision,
        });
        assert.equal(stale.ok, false);
    }

    for (const [path, expected] of [[configPath, configBytes], [databasePath, userBytes],
        [`${databasePath}.backup`, recoveryBytes]]) {
        const restored = join(destinationDirectory, path);
        assert.deepEqual(await readFile(restored), expected);
        assert.equal((await stat(restored)).mode & 0o777, 0o600);
    }
    await assert.rejects(stat(join(destinationDirectory, 'configs', name, 'local-note.txt')), { code: 'ENOENT' });
    assert.equal(await readFile(localNote, 'utf8'), 'Destination auxiliary file');
    assert.deepEqual(await readFile(extraPath), extraBytes);

    const before = JSON.parse(stored);
    const after = JSON.parse(await readFile(destination.database, 'utf8'));
    assert.deepEqual(after.admin, before.admin);
    assert.deepEqual(after.auth, before.auth);
    assert.deepEqual(after.listen, before.listen);
    assert.equal(after['panel-base-path'], before['panel-base-path']);
    assert.equal(after.waterwall_version, archiveVersion);
    assert.deepEqual(after.core_settings, settings);
    assert.deepEqual(after.core_configs, [{ id, name, enabled: true }]);
    await destination.stop();
    await destination.start();
    const reopened = await openPreview(t, destination.origin, destinationSession, destination.basePath);
    assert.equal(reopened.initial.server.core.version, archiveVersion);
    assert.equal(reopened.initial.server.core.configs[0].name, name);
});

test('backs up and restores Wave panel settings and administrator credentials without sessions', async t => {
    const source = await authFixture(t, undefined, { encrypted: false, basePath: '/backup-origin' });
    const newPath = '/archived/wave';
    const sourceCredentials = { username: 'source-admin', password: 'backup source test passphrase 9Qf4W' };
    const oldCredentials = { username: 'destination-admin', password: 'backup destination test passphrase 6Kn7X' };
    const auth = (fixture, action, credentials) => call(fixture, `/api/auth/${action}`, {
        body: JSON.stringify(credentials),
    });
    async function signIn(fixture, credentials, setup = false) {
        if (setup) assert.equal((await auth(fixture, 'setup', credentials)).status, 201);
        const response = await auth(fixture, 'sign-in', credentials);
        assert.equal(response.status, 204);
        return response.headers.get('set-cookie').split(';', 1)[0];
    }

    let tls = { enabled: false, domain: source.hostname,
        certificate_chain: join(source.cwd, 'saved certificate.pem'), private_key: join(source.cwd, 'saved key.pem') };
    const initialDb = JSON.parse(await readFile(source.database, 'utf8'));
    await writeFile(source.database, JSON.stringify({ ...initialDb, tls, fixture_only: 'not a panel setting' }));
    await source.start();
    const sourceSession = await signIn(source, sourceCredentials, true);
    const pathChange = await call(source, '/api/auth/base-path', {
        session: sourceSession, body: JSON.stringify({ 'panel-base-path': newPath }),
    });
    assert.equal(pathChange.status, 201);
    const exporting = { ...source, basePath: newPath };
    const sourceView = await openPreview(t, source.origin, sourceSession, newPath);
    if (sourceView.initial.settings.tls.supported) {
        const { chainPath, keyPath } = await tlsFixture(source.cwd);
        tls = { ...tls, enabled: true, certificate_chain: chainPath, private_key: keyPath };
    }
    for (const [action, fields] of [
        ['apply_session_settings', { session_expiry_days: 9 }],
        ['apply_theme', { theme: 'light' }],
        ['apply_donation', { answer: '15' }],
        ...(tls.enabled ? [['apply_tls_settings', { settings: tls }]] : []),
    ]) {
        const reply = await requestPreview(sourceView.socket, action, fields);
        assert.equal(reply.ok, true, reply.feedback);
    }
    const sourceDb = JSON.parse(await readFile(source.database, 'utf8'));
    const download = await call(exporting, '/api/backup', { session: sourceSession });
    assert.equal(download.status, 200, download.ok ? undefined : await download.text());
    const archive = Buffer.from(await download.arrayBuffer());
    const entries = readZip(archive);
    assert.deepEqual([...entries.keys()], ['manifest.json']);
    const manifestText = entries.get('manifest.json').toString('utf8');
    const manifest = JSON.parse(manifestText);
    assert.equal(manifest.version, 2);
    assert.match(manifest.wave_version, /^\d+\.\d+\.\d+(?:[-+][\w.-]+)?$/);
    assert.deepEqual(manifest.panel_settings, {
        admin: sourceDb.admin, 'panel-base-path': newPath, listen: sourceDb.listen, tls,
        session_expiry_days: 9, theme: 'light', donation: sourceDb.donation,
    });
    assert.match(manifest.panel_settings.admin[0].password, /^\$argon2id\$/);
    assert.equal(manifest.panel_settings.donation.answer, '15');
    for (const privateValue of [sourceCredentials.password, sourceSession.slice('wave_session='.length),
        sourceDb.auth.jwt_key, sourceDb.auth.sessions[0].id]) {
        assert.equal(manifestText.includes(privateValue), false);
    }
    if (tls.enabled) await unlink(tls.private_key);

    const destination = await authFixture(t, undefined, { encrypted: false, basePath: '/current/panel' });
    await destination.start();
    const destinationSession = await signIn(destination, oldCredentials, true);
    const peer = await signIn(destination, oldCredentials);
    const destinationView = await openPreview(t, destination.origin, destinationSession, destination.basePath);
    const before = await readFile(destination.database, 'utf8');
    for (const change of [
        panel => { panel.auth = sourceDb.auth; },
        panel => { panel.admin[0].password = 'plaintext'; },
        panel => { panel['panel-base-path'] = 'missing-leading-slash'; },
        panel => { panel.listen.port = 65536; },
        panel => { panel.donation.answered_at = 'yesterday'; },
    ]) {
        const invalid = structuredClone(manifest);
        change(invalid.panel_settings);
        const response = await prepare(destination, destinationSession, withManifest(entries, invalid));
        assert.equal(response.status, 400);
        await readReply(response, false);
    }
    assert.equal(await readFile(destination.database, 'utf8'), before);

    const proposal = await readReply(await prepare(destination, destinationSession, archive), true);
    assert.equal(proposal.wave_version, manifest.wave_version);
    assert.deepEqual(proposal.panel_settings, {
        administrator: sourceCredentials.username, base_path: newPath, restart_required: true,
    });
    if (tls.enabled) {
        assert.match(proposal.missing_dependencies.join('\n'), /panel TLS.*Cannot read the TLS private key PEM/i);
    }
    assert.equal(await readFile(destination.database, 'utf8'), before);
    await nextPreviewSnapshot(destinationView.socket, frame => !frame.data.server.core.checking_updates);

    const panelSocket = new WebSocket(`${destination.origin.replace('http:', 'ws:')}${destination.basePath}/ws/panel`, {
        headers: { Cookie: peer, Origin: destination.origin },
    });
    t.after(() => panelSocket.close());
    const [identityMessage] = await once(panelSocket, 'message', { signal: AbortSignal.timeout(timeout) });
    assert.equal(JSON.parse(identityMessage.data).username, oldCredentials.username);
    const moved = once(panelSocket, 'message', { signal: AbortSignal.timeout(timeout) });
    assert.deepEqual(await readReply(await apply(destination, destinationSession, proposal.token), true), {
        ok: true, base_path: newPath,
    });
    const [notice] = await moved;
    assert.deepEqual(JSON.parse(notice.data), { type: 'base_path', path: newPath });

    const after = JSON.parse(await readFile(destination.database, 'utf8'));
    for (const [name, value] of Object.entries(manifest.panel_settings)) {
        assert.deepEqual(after[name], value, `Restored ${name}`);
    }
    assert.deepEqual(after.auth.sessions, []);
    assert.notEqual(after.auth.jwt_key, sourceDb.auth.jwt_key);
    assert.notEqual(after.auth.jwt_key, JSON.parse(before).auth.jwt_key);
    assert.equal((await call(destination, '/api/session', { session: destinationSession })).status, 404);
    const restored = { ...destination, basePath: newPath };
    for (const session of [sourceSession, destinationSession, peer]) {
        assert.equal((await call(restored, '/api/session', { session })).status, 401);
    }
    const newSession = await signIn(restored, sourceCredentials);
    assert.equal((await auth(restored, 'sign-in', oldCredentials)).status, 401);
    const session = await (await call(restored, '/api/session', { session: newSession })).json();
    assert.equal(session.username, sourceCredentials.username);
    assert.ok(session.expires_at - Math.floor(Date.now() / 1000) >= 9 * 86400 - 5);
    const { initial } = await openPreview(t, restored.origin, newSession, newPath);
    assert.equal(initial.settings.session_expiry_days, 9);
    assert.equal(initial.settings.theme, 'light');
    assert.equal(initial.settings.donation.answer, '15');
    assert.equal(initial.settings.donation.answered_at, sourceDb.donation.answered_at);
    assert.equal(initial.settings.tls.enabled, tls.enabled);
    assert.equal(initial.settings.tls.restart_required, tls.enabled);
});

test('preserves empty rosters and byte-exact AuthenticationServer recovery pairs in ZIPs', async t => {
    const source = await authFixture(t, undefined, { encrypted: false });
    await source.start();
    const sourceSession = await fixtureSession(source);
    const { socket } = await openPreview(t, source.origin, sourceSession);
    const pairs = [
        { name: 'Empty roster', primary: Buffer.from('null\n') },
        { name: 'Healthy primary', primary: Buffer.from('{"users":[]}\n'), recovery: Buffer.from('malformed backup\n') },
        { name: 'Healthy recovery', primary: Buffer.from('{"users":[{}]}\n'), recovery: Buffer.from('{"users":null}\n') },
    ];
    const files = new Map();
    for (const { name, primary, recovery } of pairs) {
        const created = await requestPreview(socket, 'create_core_config', { name, add_auth_nodes: true });
        assert.equal(created.ok, true, created.feedback);
        const { id } = created.data.server.core.configs.find(config => config.name === name);
        const loaded = await requestPreview(socket, 'get_core_config', { id });
        assert.equal(loaded.ok, true, loaded.feedback);
        const database = loaded.result.config.nodes.find(node => node.type === 'AuthenticationServer').settings['db-path'];
        await writeFile(join(dirname(source.backend), database), primary);
        files.set(database, primary);
        if (recovery) {
            const path = `${database}.backup`;
            await writeFile(join(dirname(source.backend), path), recovery, { mode: 0o600 });
            files.set(path, recovery);
        }
    }
    const download = await call(source, '/api/backup', { session: sourceSession });
    assert.equal(download.status, 200, download.ok ? undefined : (await download.json()).error);
    const archive = Buffer.from(await download.arrayBuffer());

    const destination = await authFixture(t, undefined, { encrypted: false });
    await destination.start();
    const destinationSession = await fixtureSession(destination);
    const proposal = await readReply(await prepare(destination, destinationSession, archive), true);
    await readReply(await apply(destination, destinationSession, proposal.token), true);
    for (const [path, bytes] of files) {
        assert.deepEqual(await readFile(join(dirname(destination.backend), path)), bytes);
    }
});

test('round trips disabled drafts retained across a release switch and warns before enabling them', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await seedCore(fixture.backend, { executable: true });
    await provisionRelease(fixture);
    const release = schematicFile(fixture.backend, archiveVersion);
    const oldRelease = await readFile(release, 'utf8');
    const type = 'RetainedFixtureNode';
    const descriptor = { fields: [], ports: { previous: true, next: false, dual_ended: false },
        rules: [], graph_rules: [], links: [] };
    await writeFile(release, oldRelease.replace('"nodes": {', `"nodes": {"${type}":${JSON.stringify(descriptor)},`));
    const database = JSON.parse(await readFile(fixture.database, 'utf8'));
    await writeFile(fixture.database, JSON.stringify({ ...database, waterwall_version: archiveVersion }));
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await openPreview(t, fixture.origin, session);

    const name = 'Older draft';
    const created = await requestPreview(socket, 'create_core_config', { name });
    assert.equal(created.ok, true, created.feedback);
    const [{ id }] = created.data.server.core.configs;
    const loaded = await requestPreview(socket, 'get_core_config', { id });
    const draft = { ...loaded.result.config, nodes: [{ name: 'retained', type }] };
    const saved = await requestPreview(socket, 'save_core_config', { id, revision: loaded.result.revision, config: draft });
    assert.equal(saved.ok, true, saved.feedback);
    if (saved.data.server.core.checking_updates) {
        await nextPreviewSnapshot(socket, frame => !frame.data.server.core.checking_updates);
    }
    const changed = await requestPreview(socket, 'update_core', { version: '1.6.0' });
    assert.equal(changed.ok, true, changed.feedback);
    assert.equal(changed.data.server.core.version, '1.6.0');
    const retained = await requestPreview(socket, 'save_core_config', { id, revision: saved.result.revision, config: draft });
    assert.equal(retained.ok, true, retained.feedback);
    // The next sampler pass checks the release catalog again after a version change.
    await nextPreviewSnapshot(socket, frame => !frame.data.server.core.checking_updates);

    const path = join(dirname(fixture.backend), 'configs', name, 'config.json');
    const before = await readFile(path);
    const download = await call(fixture, '/api/backup', { session });
    assert.equal(download.status, 200, download.ok ? undefined : (await download.json()).error);
    const archive = Buffer.from(await download.arrayBuffer());
    const proposal = await readReply(await prepare(fixture, session, archive), true);
    assert.equal(proposal.configs[0].enabled, false);
    assert.match(proposal.compatibility_warnings.join('\n'), /Older draft.*RetainedFixtureNode.*1\.6\.0/);
    await readReply(await apply(fixture, session, proposal.token), true);
    assert.deepEqual(await readFile(path), before);
});

test('lists required Router databases before confirmation and skips unused paths', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await openPreview(t, fixture.origin, session);
    const name = 'Router dependencies';
    const created = await requestPreview(socket, 'create_core_config', { name });
    assert.equal(created.ok, true, created.feedback);
    const [{ id }] = created.data.server.core.configs;
    const loaded = await requestPreview(socket, 'get_core_config', { id });
    const config = { ...loaded.result.config, nodes: [
        { name: 'default', type: 'BlackHole' },
        { name: 'route', type: 'BlackHole' },
        { name: 'router', type: 'Router', next: 'default', settings: {
            'geoip-db-path': 'required-geoip.mmdb',
            'geosite-db-path': 'required-geosite.json',
            rules: [
                { 'source-ips': ['192.0.2.0/24', '!GeoIP:IR'], target: 'route' },
                { 'destination-domain': 'GeoSite:IR', target: 'route' },
            ],
        } },
        { name: 'unconditional', type: 'Router', next: 'default', settings: {
            'geoip-db-path': 'unused-geoip.mmdb',
            'geosite-db-path': 'unused-geosite.json',
            rules: [{ 'destination-ip': '192.0.2.0/24', target: 'route' }],
        } },
    ] };
    const saved = await requestPreview(socket, 'save_core_config', { id, revision: loaded.result.revision, config });
    assert.equal(saved.ok, true, saved.feedback);
    const enabled = await requestPreview(socket, 'set_core_config_enabled', { id, enabled: true });
    assert.equal(enabled.ok, true, enabled.feedback);
    const download = await call(fixture, '/api/backup', { session });
    assert.equal(download.status, 200, download.ok ? undefined : (await download.json()).error);
    const archive = Buffer.from(await download.arrayBuffer());
    const proposal = await readReply(await prepare(fixture, session, archive), true);
    const dependencies = proposal.missing_dependencies.join('\n');
    assert.match(dependencies, /geoip-db-path at required-geoip\.mmdb/);
    assert.match(dependencies, /geosite-db-path at required-geosite\.json/);
    assert.doesNotMatch(dependencies, /unused-(geoip|geosite)/);
});

test('resolves interrupted ZIP restores on startup before serving the workspace', async t => {
    for (const { name, committed, version, stagedOnly, rolledBack } of [
        { name: 'rolls back a legacy journal before the commit' },
        { name: 'cleans up a legacy journal after the commit', committed: true },
        { name: 'cleans up an interrupted staging operation', version: 2, stagedOnly: true },
        { name: 'rolls back a destination-filesystem journal', version: 2 },
        { name: 'retries cleanup after rolling back a destination-filesystem journal', version: 2, rolledBack: true },
        { name: 'cleans up a committed destination-filesystem journal', version: 2, committed: true },
    ]) {
        await t.test(name, async subtest => {
            const fixture = await authFixture(subtest, undefined, { encrypted: false });
            const backendDirectory = dirname(fixture.backend);
            const id = 'a'.repeat(32);
            const existing = { id: 1, name: 'Existing workspace', enabled: false };
            const introduced = { id: 2, name: 'Added workspace', enabled: false };
            const configContents = (name, state) => JSON.stringify({ name, nodes: [], state });
            const configPath = row => join('configs', row.name, 'config.json');
            const original = configContents(existing.name, 'before restore');
            const restored = configContents(existing.name, 'after restore');
            const added = configContents(introduced.name, 'after restore');
            const existingFile = join(backendDirectory, configPath(existing));
            const addedFile = join(backendDirectory, configPath(introduced));
            await mkdir(dirname(existingFile), { mode: 0o700 });
            if (!stagedOnly && !rolledBack) {
                await mkdir(dirname(addedFile), { mode: 0o700 });
                await writeFile(addedFile, added, { mode: 0o600 });
            }
            await writeFile(existingFile, stagedOnly || rolledBack ? original : restored, { mode: 0o600 });

            const transaction = join(backendDirectory, '.wave-restore');
            await mkdir(transaction, { mode: 0o700 });
            const payloads = [];
            if (version === 2) {
                const payloadName = '.wave-workspace-restore-staged-files-' + id;
                const existingPayload = join(dirname(existingFile), payloadName);
                const addedPayload = join(backendDirectory, 'configs', payloadName);
                payloads.push(existingPayload, addedPayload);
                for (const payload of payloads) {
                    await mkdir(payload, { mode: 0o700 });
                }
                if (stagedOnly) {
                    await writeFile(join(existingPayload, '0.new'), restored, { mode: 0o600 });
                    await writeFile(join(addedPayload, '1.new'), added, { mode: 0o600 });
                } else {
                    if (!rolledBack) {
                        await writeFile(join(existingPayload, '0.old'), original, { mode: 0o600 });
                    }
                    await writeFile(join(transaction, 'publishing'), '', { mode: 0o600 });
                }
            } else {
                await writeFile(join(transaction, '0.old'), original, { mode: 0o600 });
            }
            await writeFile(join(transaction, 'journal.json'), JSON.stringify({
                id, version,
                files: [{ path: configPath(existing), old_file: true }, { path: configPath(introduced), old_file: false }],
                new_directories: [introduced.name],
            }), { mode: 0o600 });

            const settings = JSON.parse(await readFile(fixture.database, 'utf8'));
            await writeFile(fixture.database, JSON.stringify({
                ...settings,
                waterwall_version: '1.6.0',
                core_configs: committed ? [existing, introduced] : [existing],
                ...(committed ? { workspace_restore_id: id } : {}),
            }));
            await fixture.start();
            assert.equal(await readFile(existingFile, 'utf8'), committed ? restored : original);
            if (committed) {
                assert.equal(await readFile(addedFile, 'utf8'), added);
            } else {
                await assert.rejects(stat(dirname(addedFile)), { code: 'ENOENT' });
            }
            await assert.rejects(stat(transaction), { code: 'ENOENT' });
            for (const payload of payloads) {
                await assert.rejects(stat(payload), { code: 'ENOENT' });
            }
        });
    }
});

const run = promisify(execFile);

async function exerciseRestoreStorage(t, { runAs, separateFilesystem } = {}) {
    const source = await authFixture(t, undefined, { encrypted: false });
    const configs = [
        { id: 1, name: 'Existing config', enabled: false },
        { id: 2, name: 'Added config', enabled: false },
    ];
    const newConfig = row => JSON.stringify({ name: row.name, nodes: [], note: 'restored' });
    const configPath = (root, row) => join(root, row.name, 'config.json');
    for (const row of configs) {
        const path = configPath(join(dirname(source.backend), 'configs'), row);
        await mkdir(dirname(path), { mode: 0o700 });
        await writeFile(path, newConfig(row), { mode: 0o600 });
    }
    const sourceSettings = JSON.parse(await readFile(source.database, 'utf8'));
    await writeFile(source.database, JSON.stringify({ ...sourceSettings, core_configs: configs }));
    await source.start();
    const sourceSession = await fixtureSession(source);
    const download = await call(source, '/api/backup', { session: sourceSession });
    assert.equal(download.status, 200, download.ok ? undefined : (await download.json()).error);
    const archive = Buffer.from(await download.arrayBuffer());

    const target = await authFixture(t, undefined, { encrypted: false, runAs });
    const root = join(dirname(target.backend), 'configs');
    if (separateFilesystem) {
        await run('mount', ['-t', 'tmpfs', '-o', 'mode=0700', 'tmpfs', root]);
        assert.notEqual((await stat(root)).dev, (await stat(target.database)).dev);
    }
    try {
        const oldPath = configPath(root, configs[0]);
        const oldConfig = JSON.stringify({ name: configs[0].name, nodes: [], note: 'before restore' });
        await mkdir(dirname(oldPath), { mode: 0o700 });
        await writeFile(oldPath, oldConfig, { mode: 0o644 });
        if (runAs) {
            await chown(dirname(oldPath), runAs.uid, runAs.gid);
            assert.equal((await stat(oldPath)).uid, 0);
        }
        const original = await stat(oldPath);
        const oldSettings = JSON.parse(await readFile(target.database, 'utf8'));
        await writeFile(target.database, JSON.stringify({ ...oldSettings, core_configs: configs.slice(0, 1) }));
        await target.start();
        const session = await fixtureSession(target);
        const before = await readFile(target.database);

        const failed = await readReply(await prepare(target, session, archive), true);
        await writeFile(target.database, 'invalid JSON');
        try {
            await readReply(await apply(target, session, failed.token), false);
            assert.equal(await readFile(oldPath, 'utf8'), oldConfig);
            const rolledBack = await stat(oldPath);
            assert.equal(rolledBack.ino, original.ino);
            assert.equal(rolledBack.uid, original.uid);
            assert.equal(rolledBack.mode, original.mode);
            assert.deepEqual(await readdir(root), [configs[0].name]);
            assert.deepEqual(await readdir(dirname(oldPath)), ['config.json']);
        } finally {
            await writeFile(target.database, before);
        }

        const proposal = await readReply(await prepare(target, session, archive), true);
        assert.deepEqual(proposal.replaced_configs, [configs[0].name]);
        await readReply(await apply(target, session, proposal.token), true);
        for (const row of configs) {
            const path = configPath(root, row);
            assert.equal(await readFile(path, 'utf8'), newConfig(row));
            assert.equal((await stat(path)).mode & 0o777, 0o600);
            assert.deepEqual(await readdir(dirname(path)), ['config.json']);
        }
        assert.deepEqual((await readdir(root)).sort(), configs.map(row => row.name).sort());
        assert.deepEqual(JSON.parse(await readFile(target.database, 'utf8')).core_configs, configs);
    } finally {
        await target.stop();
        if (separateFilesystem) {
            await run('umount', [root]);
        }
    }
}

test('replaces a readable root-owned config through an unprivileged Wave service',
    { skip: process.getuid?.() !== 0 }, async t => {
        await exerciseRestoreStorage(t, { runAs: { uid: 65534, gid: 65534 } });
    });

const mountTest = 'adds and replaces configs on a separate filesystem';
test(mountTest, { skip: process.platform !== 'linux' }, async t => {
    const parentNamespace = process.env.WAVE_BACKUP_TEST_PARENT_MOUNT_NAMESPACE;
    const currentNamespace = await readlink('/proc/self/ns/mnt');
    if (parentNamespace && parentNamespace !== currentNamespace) {
        await exerciseRestoreStorage(t, { separateFilesystem: true });
        return;
    }

    const namespaceArgs = ['--user', '--map-root-user', '--mount', '--propagation', 'private'];
    try {
        await run('unshare', [...namespaceArgs, 'true']);
    } catch (error) {
        if (error.code === 'ENOENT' || error.stderr?.includes('Operation not permitted')) {
            t.skip('Private mount namespaces are unavailable');
            return;
        }
        throw error;
    }
    const env = { ...process.env, WAVE_BACKUP_TEST_PARENT_MOUNT_NAMESPACE: currentNamespace };
    delete env.NODE_TEST_CONTEXT;
    try {
        await run('unshare', [...namespaceArgs, process.execPath, '--test', '--test-name-pattern',
            mountTest, __filename], { env, timeout: 30000 });
    } catch (error) {
        assert.fail(`${error.message}\n${error.stdout ?? ''}\n${error.stderr ?? ''}`);
    }
});
