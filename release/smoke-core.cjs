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
const origin = `http://127.0.0.1:${port}`;
const manager = join(instance, basename(instance));
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

    const setup = await fetch(`${origin}/_wave-setup/api/auth/base-path`, {
        method: 'POST',
        headers: { Origin: origin, 'X-Wave-CSRF': '1', 'Content-Type': 'application/json' },
        body: JSON.stringify({ 'panel-base-path': panelBasePath }),
        signal: AbortSignal.timeout(5000),
    });
    assert.equal(setup.status, 201);
    const session = await fixtureSession({ origin, basePath: panelBasePath });
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
