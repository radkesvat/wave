const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { once } = require('node:events');
const { chmod, copyFile } = require('node:fs/promises');
const { createConnection, createServer } = require('node:net');
const { test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { authFixture, fixtureSession, nextPreviewSnapshot, openPreview, requestPreview,
    seedCore, timeout } = require('./auth_fixture.cjs');

const waterwall = process.env.WAVE_WATERWALL_V160;
const node = 'AuthenticationServer';
const accountId = '18446744073709551615';

async function watchRoster(t, fixture, cookie, profile, access) {
    const { socket, schematicRevision } = await openPreview(t, fixture.origin, cookie);
    const subscription = `real-${profile.id}`;
    const frames = [];
    const completed = [];
    let transfer;
    socket.addEventListener('message', event => {
        const frame = JSON.parse(event.data);
        if (frame.type !== 'users_roster' || frame.subscription !== subscription) return;
        frames.push(frame);
        if (frame.event === 'begin') transfer = { begin: frame, rows: [], chunks: 0 };
        if (frame.event === 'chunk' && transfer && transfer.begin.attempt === frame.attempt) {
            assert.equal(frame.index, transfer.chunks++);
            transfer.rows.push(...frame.rows);
        }
        if (frame.event === 'end' && transfer && transfer.begin.attempt === frame.attempt) {
            assert.equal(frame.chunks, transfer.chunks);
            completed.push(transfer);
            transfer = undefined;
        }
    });
    socket.send(JSON.stringify({
        action: 'users_roster_subscribe', subscription, config_id: profile.id, node,
        source: access.source, run_id: access.run_id, database_generation: access.database_generation,
        schematic_revision: schematicRevision,
    }));
    async function waitFor(match) {
        const deadline = Date.now() + timeout;
        do {
            const entry = completed.find(match);
            if (entry) return entry;
            await delay(25);
        } while (Date.now() < deadline);
        assert.fail(`No matching committed roster transfer. ${JSON.stringify(frames.map(frame => frame.event))}`);
    }
    return { frames, waitFor };
}

async function pushTraffic(port, credentials, firstUsage) {
    // WaterWall's existing producer connection supplies actual charged usage.
    const socket = createConnection({ port, host: '127.0.0.1' });
    try {
        await once(socket, 'connect', { signal: AbortSignal.timeout(timeout) });
        socket.setTimeout(timeout, () => socket.destroy(new Error('Producer response timed out.')));
        const stream = socket[Symbol.asyncIterator]();
        let pending = Buffer.alloc(0);
        async function read(size) {
            while (pending.length < size) {
                const { done, value } = await stream.next();
                assert.equal(done, false, 'WaterWall closed the producer socket.');
                pending = Buffer.concat([pending, value]);
            }
            const result = pending.subarray(0, size);
            pending = pending.subarray(size);
            return result;
        }
        async function request(type, correlation, token, body, expectedType) {
            const payload = Buffer.from(JSON.stringify(body));
            const prefix = Buffer.alloc(77);
            prefix.writeUInt32BE(prefix.length - 4 + payload.length, 0);
            token.copy(prefix, 4);
            prefix.writeUInt8(type, 68);
            prefix.writeUInt32BE(correlation, 69);
            prefix.writeUInt32BE(payload.length, 73);
            socket.write(Buffer.concat([prefix, payload]));
            const size = (await read(4)).readUInt32BE();
            assert.ok(size >= 25 && size <= 1024 * 1024, 'Invalid producer response length.');
            const response = await read(size);
            assert.equal(response.readUInt8(16), expectedType, 'Producer request rejected.');
            assert.equal(response.readUInt32BE(17), correlation);
            assert.equal(response.readUInt32BE(21), size - 25);
            return response.subarray(25);
        }
        const session = await request(10, 1, Buffer.alloc(64), credentials, 4);
        assert.equal(session.length, 128);
        await request(11, 2, session.subarray(0, 64), {
            epoch: session.subarray(64).toString('ascii'), sequence: '1', users: [
                { id: accountId, up: '13', down: '29', 'first-usage-at-ms': firstUsage },
            ],
        }, 0);
    } finally {
        socket.destroy();
    }
}

test('manages scoped users through a real WaterWall 1.6.0 deployment', {
    skip: waterwall ? false : 'Set WAVE_WATERWALL_V160 to a WaterWall 1.6.0 executable.',
    timeout: 45_000,
}, async t => {
    assert.match(execFileSync(waterwall, ['--version'], { encoding: 'utf8', timeout }), /\b1\.6\.0\s*$/);
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const { program } = await seedCore(fixture.backend);
    await copyFile(waterwall, program);
    await chmod(program, 0o700);
    await fixture.start();
    const cookie = await fixtureSession(fixture);
    const { socket } = await openPreview(t, fixture.origin, cookie);
    const send = (action, fields) => requestPreview(socket, action, fields);
    async function apply(action, fields) {
        const deadline = Date.now() + timeout;
        let reply = await send(action, fields);
        // A core checkpoint or producer report can briefly contend with an explicit read.
        while (!reply.ok && action.startsWith('get_live_') && reply.result?.code === 'busy' && Date.now() < deadline) {
            await delay(50);
            reply = await send(action, fields);
        }
        assert.equal(reply.ok, true, reply.feedback);
        return reply;
    }
    const row = (reply, name) => reply.data.server.core.configs.find(item => item.name === name);
    async function starter(name) {
        return row(await apply('create_core_config', { name, add_auth_nodes: true }), name);
    }
    async function configure(id, change) {
        const { result: { config, revision } } = await apply('get_core_config', { id });
        change(config);
        await apply('save_core_config', { id, config, revision });
    }
    const producerReservation = createServer();
    t.after(() => producerReservation.listening && new Promise(resolve => producerReservation.close(resolve)));
    producerReservation.listen(0, '127.0.0.1');
    await once(producerReservation, 'listening');
    const producerPort = producerReservation.address().port;
    const managed = await starter('Managed core fixture');
    const monitor = await starter('Monitor core fixture');
    await configure(monitor.id, config => {
        const api = config.nodes.find(item => item.type === 'APIServer');
        api.settings.clients[0].allow[0].operations = ['list-users'];
    });
    const noApi = await starter('No API core fixture');
    await apply('create_config_user', {
        config_id: monitor.id, node,
        user: { id: '45', name: 'List-only fixture account', password: 'throwaway list-only core password' },
    });
    let producer;
    let apiToken;
    let sharedPort;
    await configure(noApi.id, config => {
        const listener = config.nodes.find(item => item.type === 'TcpListener');
        sharedPort = listener.settings.port;
        listener.next = node;
        config.nodes = config.nodes.filter(item => item.type !== 'AuthenticationClient' && item.type !== 'APIServer');
    });
    await configure(managed.id, config => {
        const server = config.nodes.find(item => item.name === node);
        const [{ name, secret }] = server.settings['auth-clients'];
        producer = { name, secret };
        server.settings['file-save-rate-ms'] = 500;
        const api = config.nodes.find(item => item.type === 'APIServer');
        apiToken = api.settings.clients[0].token;
        api.settings.max_request_envelope = 1024;
        // Only the second port is dedicated to the API across the launched configs.
        const listener = config.nodes.find(item => item.type === 'TcpListener');
        listener.settings.port = [sharedPort, listener.settings.port];
        // Supply the test's producer over private loopback to the starter's AuthenticationServer.
        const index = config.nodes.findIndex(item => item.type === 'AuthenticationClient');
        config.nodes[index] = {
            name: 'Producer listener', type: 'TcpListener', next: node,
            settings: { address: '127.0.0.1', port: producerPort },
        };
    });
    for (const profile of [managed, monitor, noApi]) {
        await apply('set_core_config_enabled', { id: profile.id, enabled: true });
    }
    await apply('apply_core_settings', { settings: { misc: { workers: 2 } } });
    await new Promise(resolve => producerReservation.close(resolve));
    async function start() {
        const running = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'running' ||
            Boolean(frame.data.server.core.error), 15_000);
        await apply('start_core');
        const state = await running;
        assert.equal(state.data.server.core.state, 'running', state.data.server.core.error);
        return state;
    }
    const started = await start();
    function scope(profile, state = started) {
        const { source, run_id } = row(state, profile.name).users_access[node];
        return { config_id: profile.id, node, source, run_id };
    }
    assert.deepEqual(row(started, monitor.name).users_access[node].operations, ['list-users']);
    assert.equal(scope(noApi).source, 'blocked');
    assert.match(row(started, noApi.name).users_access[node].reason, /loopback TcpListener/);
    for (const profile of [monitor, noApi]) {
        const denied = await send('create_live_config_user', {
            ...scope(profile), user: { id: accountId, password: 'discarded fixture credential' },
        });
        assert.equal(denied.ok, false);
    }
    assert.equal((await send('get_config_users', { config_id: noApi.id, node })).ok, false);
    // Wave records the launched process before its TCP listeners finish starting.
    async function firstList(target) {
        const readyDeadline = Date.now() + timeout;
        let reply;
        do {
            reply = await send('get_live_config_users', target);
            if (reply.ok) return reply.result;
            await delay(100);
        } while (Date.now() < readyDeadline);
        assert.fail(reply.feedback);
    }
    const monitored = await firstList(scope(monitor));
    assert.deepEqual(monitored.users.map(user => user.id), ['45']);
    const monitoredRoster = await watchRoster(t, fixture, cookie, monitor, row(started, monitor.name).users_access[node]);
    const listOnly = await monitoredRoster.waitFor(entry => entry.begin.total === 1);
    assert.equal(listOnly.rows[0].name, 'List-only fixture account');

    const current = scope(managed);
    const managedRoster = await watchRoster(t, fixture, cookie, managed, row(started, managed.name).users_access[node]);
    assert.deepEqual((await managedRoster.waitFor(entry => entry.begin.total === 0)).rows, []);
    const initial = (await apply('get_live_config_user_status', current)).result.status;
    assert.equal(initial.database_loaded, true);
    assert.equal(initial.user_count, '0');
    const password = 'throwaway initial core password';
    const replacement = 'throwaway rotated core password';
    const target = { ...current, user_id: accountId };
    const overLimit = await send('create_live_config_user', {
        ...target, user: { notes: 'n'.repeat(2000), password },
    });
    assert.equal(overLimit.ok, false);
    assert.deepEqual(overLimit.result, { code: 'invalid_request', outcome: 'not_sent', user_id: accountId });
    assert.match(overLimit.feedback, /rejected the request frame/i);
    assert.ok(!overLimit.feedback.includes(password));
    const missing = await send('get_live_config_user', target);
    assert.equal(missing.ok, false);
    assert.equal(missing.result.code, 'not_found');
    const create = await apply('create_live_config_user', {
        ...current, user: { id: accountId, name: 'Live fixture account', enabled: false, password },
    });
    assert.equal(create.result.user_id, accountId);
    assert.equal(create.result.outcome, 'applied');
    const createdInRoster = await managedRoster.waitFor(entry => entry.rows.some(user => user.id === accountId));
    assert.equal(createdInRoster.rows[0].enabled, false);
    const detail = () => apply('get_live_config_user', target);
    const read = await detail();
    assert.equal(read.result.user.enabled, false);
    assert.match(read.result.if_match, /^[0-9a-f]{48}$/);
    const quota = '9007199254740993';
    const edit = await apply('edit_live_config_user', {
        ...target, if_match: read.result.if_match, user: { enabled: true, limit: { traffic: { up: quota } } },
    });
    assert.equal(edit.result.outcome, 'applied');
    const edited = (await detail()).result;
    assert.equal(edited.user.limit.traffic.up, quota);
    const editedInRoster = await managedRoster.waitFor(entry => entry.rows.some(user => user.limit.traffic.up === quota));
    assert.equal(editedInRoster.rows[0].enabled, true);
    const changed = await apply('change_live_config_user_password', {
        ...target, if_match: edited.if_match, password: replacement,
    });
    assert.equal(changed.result.outcome, 'applied');
    const before = (await apply('get_live_config_users', current)).result;
    assert.deepEqual(before.users[0].stats.traffic, { up: '0', down: '0' });
    await pushTraffic(producerPort, producer, before.accounting_time_ms);
    const after = await apply('get_live_config_users', current);
    assert.equal(after.result.users[0].id, accountId);
    assert.deepEqual(after.result.users[0].stats.traffic, { up: '13', down: '29' });
    assert.equal(after.result.config_revision, before.config_revision);
    assert.notEqual(after.result.stats_revision, before.stats_revision);
    const usageInRoster = await managedRoster.waitFor(entry => entry.rows.some(user => user.stats.traffic.down === '29'));
    assert.deepEqual(usageInRoster.rows[0].stats.traffic, { up: '13', down: '29' });
    const deniedReset = await send('reset_config_user_traffic', { ...target, roster: true });
    assert.equal(deniedReset.ok, false);
    assert.match(deniedReset.feedback, /stop WaterWall/i);
    for (const entry of [started, create, read, edit, changed, after, ...managedRoster.frames, ...monitoredRoster.frames]) {
        const publicData = JSON.stringify(entry);
        for (const credential of [producer.secret, apiToken, password, replacement, 'throwaway list-only core password']) {
            assert.ok(!publicData.includes(credential), 'A user action exposed a private credential.');
        }
    }

    const saveDeadline = Date.now() + timeout;
    let saved;
    do {
        saved = (await apply('get_live_config_user_status', current)).result.status;
        if (saved.primary_checkpoint_valid && !saved.database_dirty && saved.primary_save.last_result === 'success') {
            break;
        }
        await delay(100);
    } while (Date.now() < saveDeadline);
    assert.equal(saved.primary_save.last_result, 'success');
    assert.equal(saved.database_dirty, false);
    const stopped = nextPreviewSnapshot(socket, frame => frame.data.server.core.state === 'stopped');
    await apply('stop_core');
    const disk = row(await stopped, managed.name).users_access[node];
    assert.equal(disk.source, 'offline');
    assert.ok(disk.database_generation);
    assert.equal(disk.database_generation, row(started, managed.name).users_access[node].database_generation);
    const offline = { config_id: managed.id, node, source: disk.source, run_id: disk.run_id, user_id: accountId };
    const fromDisk = (await apply('get_config_users', { ...offline, search: accountId })).result.users[0];
    assert.equal(fromDisk.enabled, true);
    assert.equal(fromDisk.limit.traffic.up, quota);
    assert.deepEqual(fromDisk.stats.traffic, { up: '13', down: '29' });
    assert.equal((await apply('get_config_user_password', offline)).result.password === replacement, true);
    const reset = await apply('reset_config_user_traffic', { ...offline, roster: true });
    assert.deepEqual(reset.result, { user_id: accountId });
    const restarted = scope(managed, await start());
    const resetUser = (await firstList(restarted)).users.find(user => user.id === accountId);
    assert.deepEqual(resetUser.stats.traffic, { up: '0', down: '0' });
    assert.equal(resetUser.limit.traffic.up, quota);
});
