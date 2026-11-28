const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const { readFile, rm, writeFile } = require('node:fs/promises');
const { basename, join } = require('node:path');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { promisify } = require('node:util');
const { fixtureSession, openPreview, panelBasePath, requestPreview, seedCore } =
    require('../tests/backend/auth_fixture.cjs');

const run = promisify(execFile);
const [instance, port] = process.argv.slice(2);
const origin = `http://127.0.0.2:${port}`;
const manager = join(instance, basename(instance));
const database = join(instance, 'wave-db.json');
const ready = join(instance, 'fixture-ready');
const stopped = join(instance, 'fixture-stopped');

const fixtureProgram = `#!/usr/bin/python3
import os
from pathlib import Path
import signal
import sys
import time

def stop(*args):
    time.sleep(0.4)
    Path('fixture-stopped').write_text(str(os.getpid()))
    sys.exit(0)

signal.signal(signal.SIGTERM, stop)
Path('fixture-ready').write_text(str(os.getpid()))
signal.pause()
`;

async function startCore(socket) {
    await rm(ready, { force: true });
    await rm(stopped, { force: true });
    const reply = await requestPreview(socket, 'start_core');
    assert.equal(reply.ok, true, reply.feedback);
    for (let attempt = 0; attempt < 100; ++attempt) {
        const pid = await readFile(ready, 'utf8').catch(error => {
            if (error.code !== 'ENOENT') throw error;
            return '';
        });
        if (/^[1-9][0-9]*$/.test(pid)) return pid;
        await delay(50);
    }
    assert.fail('The WaterWall fixture did not start.');
}

test('the systemd manager waits for WaterWall shutdown before stopping or restarting Wave', async t => {
    await run(manager, ['stop']);
    const { program } = await seedCore(join(instance, 'wave-backend'));
    await rm(program, { force: true });
    await writeFile(program, fixtureProgram, { mode: 0o700 });
    await run(manager, ['start']);

    const { setup_token: setupToken } = JSON.parse(await readFile(database, 'utf8'));
    assert.ok(typeof setupToken === 'string' && /^[0-9a-f]{64}$/.test(setupToken), 'Expected a setup key.');
    const wrongToken = (setupToken[0] === '0' ? '1' : '0') + setupToken.slice(1);
    const postSetup = (route, body, token) => fetch(origin + route, {
        method: 'POST',
        headers: {
            Origin: origin, 'X-Wave-CSRF': '1', 'Content-Type': 'application/json',
            ...(token === undefined ? {} : { 'X-Wave-Setup-Token': token }),
        },
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(5000),
    });
    const baseRoute = '/_wave-setup/api/auth/base-path';
    const selection = { 'panel-base-path': panelBasePath };
    assert.equal((await postSetup(baseRoute, selection)).status, 403);
    assert.equal((await postSetup(baseRoute, selection, wrongToken)).status, 403);
    assert.equal((await postSetup(baseRoute, selection, setupToken)).status, 201);

    const adminRoute = `${panelBasePath}/api/auth/setup`;
    const otherAdmin = { username: 'other-admin', password: 'other admin smoke test passphrase 5Gs2n' };
    assert.equal((await postSetup(adminRoute, otherAdmin)).status, 403);
    assert.equal((await postSetup(adminRoute, otherAdmin, wrongToken)).status, 403);
    const session = await fixtureSession({ origin, basePath: panelBasePath }, { setupToken });
    assert.equal('setup_token' in JSON.parse(await readFile(database, 'utf8')), false);
    const { socket } = await openPreview(t, origin, session);
    const name = 'Shutdown fixture';
    const created = await requestPreview(socket, 'create_core_config', { name });
    assert.equal(created.ok, true, created.feedback);
    const { id } = created.data.server.core.configs.find(config => config.name === name);
    const enabled = await requestPreview(socket, 'set_core_config_enabled', { id, enabled: true });
    assert.equal(enabled.ok, true, enabled.feedback);

    const firstPid = await startCore(socket);
    await run(manager, ['stop']);
    assert.equal(await readFile(stopped, 'utf8'), firstPid);

    await run(manager, ['start']);
    const reopened = await openPreview(t, origin, session);
    const nextPid = await startCore(reopened.socket);
    await run(manager, ['restart']);
    assert.equal(await readFile(stopped, 'utf8'), nextPid);
});
