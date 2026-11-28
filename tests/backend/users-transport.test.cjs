const assert = require('node:assert/strict');
const { once } = require('node:events');
const { readFile, rename, writeFile } = require('node:fs/promises');
const { createServer } = require('node:net');
const { dirname, join } = require('node:path');
const { test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { authFixture, fixtureSession, nextPreviewSnapshot, openPreview, requestPreview,
    seedCore, timeout } = require('./auth_fixture.cjs');

const profileName = 'Transport profile';
const node = 'staff';
const cursorWithLostReply = '34'.repeat(32);
const stalledCursor = '56'.repeat(32);
const restartCursor = '78'.repeat(32);
const editToken = '9a'.repeat(24);

function transportCore({ profileName, cursorWithLostReply, stalledCursor, restartCursor, editToken }) {
    const { appendFileSync, existsSync, readFileSync, writeFileSync } = require('node:fs');
    const { createServer } = require('node:net');
    const settings = JSON.parse(readFileSync(0, 'utf8'));
    const profile = settings.configs.map(path => JSON.parse(readFileSync(path, 'utf8')))
        .find(config => config.name === profileName);
    let pendingRestart;

    function listen(peer) {
        const server = createServer(connection => {
            let input = Buffer.alloc(0);
            connection.on('error', () => {});
            connection.on('data', chunk => {
                input = Buffer.concat([input, chunk]);
                if (input.length < 4 || input.length < 4 + input.readUInt32BE(0)) return;
                const request = JSON.parse(input.subarray(4).toString('utf8'));
                const body = JSON.parse(request.body);
                appendFileSync('transport-requests.jsonl', JSON.stringify({ peer, ...request, body }) + '\n',
                    { mode: 0o600 });
                const revision = { config_revision: '1', edit_token: editToken };
                const result = data => ({ code: data.error ? 1 : 0, content_type: 'application/json',
                    body_encoding: 'utf8', body: JSON.stringify(data) });
                const envelope = { id: request.id, error: null, result: null };
                const error = (code = 'invalid_request') => ({ code, message: 'private fixture error message' });

                if (request.operation === 'list-users') {
                    if (body.cursor === stalledCursor) return;
                    if (peer === 'first' && body.cursor === cursorWithLostReply) return connection.destroy();
                    if (peer === 'first' && body.cursor === restartCursor) {
                        pendingRestart = connection;
                        return;
                    }
                    envelope.result = result({ ...revision, total_users: '0', stats_revision: '1',
                        accounting_time_ms: '0', users: [], next_cursor: null });
                } else if (request.operation === 'get-user') {
                    envelope.result = result({ ...revision, stats_revision: '1', accounting_time_ms: '1800000000000',
                        user: { id: body.id, gid: '0', enabled: true, name: 'Clock fixture', email: '', notes: '',
                            wireguard_allowed_ips: '', record_stat_interval_ms: '120000',
                            time: { expire_at_ms: '0', expire_after_first_usage_ms: '86400000',
                                first_usage_at_ms: '1800000000000', created_at_ms: '1' },
                            limit: { traffic: { u: '0', d: '0', total: '0' }, bandwidth: { u: '0', d: '0' },
                                ips: '0', devices: '0', cons_in: '0', cons_out: '0' },
                            usage: { u: '0', d: '0' } } });
                } else if (peer === 'first' && request.operation === 'update-user') {
                    envelope.result = result({ error: { code: 'conflict' } });
                } else {
                    envelope.result = result({ ...revision, id: body.id });
                    if (peer === 'first') {
                        switch (body.config?.name) {
                        case 'lost acknowledgement': return connection.destroy();
                        case 'null success': envelope.id = null; break;
                        case 'missing correlation':
                            delete envelope.id;
                            envelope.error = error();
                            envelope.result = null;
                            break;
                        case 'wrong correlation': envelope.id = 'different'; break;
                        case 'framing refusal':
                            envelope.id = null;
                            envelope.error = error();
                            envelope.result = null;
                            break;
                        case 'gateway timeout':
                            envelope.error = error('timeout');
                            envelope.result = null;
                            break;
                        }
                    }
                }
                const payload = Buffer.from(JSON.stringify(envelope));
                const header = Buffer.alloc(4);
                header.writeUInt32BE(payload.length);
                const acknowledge = () => connection.end(Buffer.concat([header, payload]));
                if (body.config?.name === 'delayed acknowledgement' || request.selector.node === 'slow reader') {
                    setTimeout(acknowledge, 1350);
                } else {
                    acknowledge();
                }
            });
        });
        const listener = profile.nodes.find(node => node.name === `${peer} listener`);
        const port = listener.settings.port?.[1] ?? listener.settings['port-range'][1];
        server.listen(port, '127.0.0.1',
            () => writeFileSync(`${peer}-ready`, String(process.pid)));
    }

    // Leave the second listener available briefly after closing the active request on restart.
    process.on('SIGTERM', () => {
        pendingRestart?.destroy();
        setTimeout(() => process.exit(0), 250);
    });
    listen('second');
    const enableFirst = setInterval(() => {
        if (existsSync('enable-first')) {
            clearInterval(enableFirst);
            listen('first');
        }
    }, 20);
}

async function waitFor(read, matches) {
    const deadline = Date.now() + timeout;
    do {
        const result = await read();
        if (matches(result)) return result;
        await delay(20);
    } while (Date.now() < deadline);
    assert.fail('The expected listener activity did not occur.');
}

test('uses eligible fallback listeners within one deadline and never resends an uncertain write', {
    skip: process.platform !== 'linux', timeout: 30_000,
}, async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const core = await seedCore(fixture.backend);
    const options = JSON.stringify({ profileName, cursorWithLostReply, stalledCursor, restartCursor, editToken });
    await writeFile(core.program, `#!/usr/bin/env node\n(${transportCore})(${options});\n`, { mode: 0o700 });
    const directory = dirname(fixture.backend);
    const read = async name => readFile(join(directory, name), 'utf8').catch(error => {
        if (error.code === 'ENOENT') return '';
        throw error;
    });
    const requests = async () => (await read('transport-requests.jsonl')).trim().split('\n').filter(Boolean)
        .map(line => JSON.parse(line));

    const reservations = Array.from({ length: 4 }, () => createServer());
    t.after(() => Promise.all(reservations.map(server => server.listening && new Promise(resolve => server.close(resolve)))));
    const ports = await Promise.all(reservations.map(async server => {
        server.listen(0, '127.0.0.1');
        await once(server, 'listening');
        return server.address().port;
    }));
    const [unusedPort, firstPort, otherUnusedPort, secondPort] = ports.sort((a, b) => b - a);
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket } = await openPreview(t, fixture.origin, session);
    const { socket: observer } = await openPreview(t, fixture.origin, session);
    const send = (action, fields, client = socket) => requestPreview(client, action, fields);
    async function apply(action, fields, client) {
        const reply = await send(action, fields, client);
        assert.equal(reply.ok, true, reply.feedback);
        return reply;
    }
    const created = await apply('create_core_config', { name: profileName });
    const [{ id: configId }] = created.data.server.core.configs;
    const { result: { config, revision } } = await apply('get_core_config', { id: configId });
    config.nodes = [node, 'slow reader', 'blocked'].map(name => ({ name, type: 'AuthenticationServer' }));
    for (const [i, peer] of ['first', 'second'].entries()) {
        const allow = [{ config: profileName, node, operations: ['list-users', 'get-user', 'create-user', 'update-user'] }];
        if (peer === 'first') {
            allow.push({ config: profileName, node: 'slow reader', operations: ['list-users'] });
        }
        const listenerPorts = peer === 'first' ? { port: [unusedPort, firstPort, otherUnusedPort] }
                                               : { 'port-range': [1, secondPort] };
        config.nodes.push({ name: `${peer} listener`, type: 'TcpListener', next: `${peer} API`,
            settings: { address: '127.0.0.1', ...listenerPorts } });
        config.nodes.push({ name: `${peer} API`, type: 'APIServer', settings: { clients: [
            { name: `${peer} client`, token: String(i + 1).repeat(64), allow },
        ] } });
    }
    // All but the last port of the second listener overlap; every port of the blocked API overlaps.
    config.nodes.push({ name: 'overlapping listener', type: 'TcpListener', next: 'blocked API',
        settings: { address: '127.0.0.1', 'port-range': [1, secondPort - 1], 'multiport-backend': 'socket' } });
    config.nodes.push({ name: 'blocked API', type: 'APIServer', settings: { clients: [
        { name: 'blocked client', token: '3'.repeat(64), allow: [
            { config: profileName, node: 'blocked', operations: ['list-users'] },
        ] },
    ] } });
    await apply('save_core_config', { id: configId, config, revision });
    await apply('set_core_config_enabled', { id: configId, enabled: true });
    await Promise.all(reservations.map(server => new Promise(resolve => server.close(resolve))));
    const running = () => nextPreviewSnapshot(observer, frame => frame.data.server.core.state === 'running');
    const start = running();
    await apply('start_core');
    const access = frame => frame.data.server.core.configs.find(row => row.id === configId).users_access[node];
    const startedFrame = await start;
    const blocked = startedFrame.data.server.core.configs[0].users_access.blocked;
    assert.equal(blocked.source, 'blocked');
    assert.match(blocked.reason, /dedicated listener port/);
    const initial = access(startedFrame);
    assert.deepEqual(initial.operations, ['list-users', 'get-user', 'create-user', 'update-user']);
    const initialPid = await waitFor(() => read('second-ready'), Boolean);
    assert.equal(await read('first-ready'), '');

    const scope = { config_id: configId, node, source: 'live', run_id: initial.run_id };
    const list = (cursor, selection = scope) => send('get_live_config_users', {
        ...selection, ...(cursor && { cursor }),
    });
    const password = 'transport-test account credential';
    const create = (id, name) => send('create_live_config_user', { ...scope, user_id: id, user: { password, name } });
    assert.equal((await list()).result.total, '0');
    const legacy = await apply('get_live_config_users', { config_id: configId, node });
    assert.deepEqual(legacy.result, { available: true, total: 0, users: [], next_cursor: null });
    assert.equal((await create('41', 'safe fallback')).result.outcome, 'applied');
    const routed = await requests();
    assert.deepEqual(routed.map(request => [request.peer, request.operation]), [
        ['second', 'list-users'], ['second', 'list-users'], ['second', 'create-user'],
    ]);
    assert.deepEqual(routed[2].body, { id: '41', password, config: { name: 'safe fallback' } });
    for (const request of routed) {
        assert.deepEqual(request.selector, { config: profileName, node });
        assert.equal(request.auth.client, 'second client');
    }

    await writeFile(join(directory, 'enable-first'), 'yes');
    await waitFor(() => read('first-ready'), pid => pid === initialPid);
    assert.equal((await create('40', 'delayed acknowledgement')).result.outcome, 'applied');
    assert.deepEqual((await requests()).filter(request => request.body.id === '40').map(request => request.peer),
        ['first'], 'A fully sent write can use the rest of the request deadline for its acknowledgement.');
    const slowReader = startedFrame.data.server.core.configs[0].users_access['slow reader'];
    assert.deepEqual(slowReader.operations, ['list-users']);
    assert.equal((await list(undefined, { ...scope, node: 'slow reader', run_id: slowReader.run_id })).ok, true,
        'A read on one reachable multiport listener has the same reply deadline as a single-port listener.');
    assert.deepEqual((await requests()).filter(request => request.selector.node === 'slow reader').map(request => request.peer),
        ['first']);
    for (const [i, name] of ['lost acknowledgement', 'null success', 'missing correlation', 'wrong correlation',
        'gateway timeout', 'framing refusal'].entries()) {
        const before = (await requests()).length;
        const reply = await create(String(42 + i), name);
        assert.equal(reply.ok, false, name);
        assert.equal(reply.result.outcome, name === 'framing refusal' ? 'not_sent' : 'unknown', name);
        assert.doesNotMatch(JSON.stringify(reply), /private fixture error message|transport-test account credential/);
        const delivered = (await requests()).slice(before);
        assert.deepEqual(delivered.map(request => [request.peer, request.body.id]), [['first', String(42 + i)]]);
    }
    const conflict = await send('edit_live_config_user', { ...scope, user_id: '41', if_match: editToken,
        user: { enabled: false } });
    assert.equal(conflict.result.outcome, 'conflict');
    assert.deepEqual((await requests()).filter(request => request.operation === 'update-user')
        .map(request => request.peer), ['first']);

    assert.equal((await list(cursorWithLostReply)).ok, true);
    const byCursor = async cursor => (await requests()).filter(request => request.body.cursor === cursor);
    assert.deepEqual((await byCursor(cursorWithLostReply)).map(request => request.peer), ['first', 'second']);
    const started = performance.now();
    assert.equal((await list(stalledCursor)).ok, false);
    assert.ok(performance.now() - started < 3500, 'The two listeners share one two-second transport deadline.');
    assert.deepEqual((await byCursor(stalledCursor)).map(request => request.peer), ['first', 'second']);

    const pending = list(restartCursor);
    await waitFor(() => byCursor(restartCursor), entries => entries.length === 1);
    const restarted = nextPreviewSnapshot(observer, frame => frame.data.server.core.state === 'running' &&
        access(frame).run_id !== scope.run_id);
    await apply('restart_core', {}, observer);
    const outdated = await pending;
    assert.equal(outdated.ok, false);
    assert.match(outdated.feedback, /changed run/i);
    const current = access(await restarted);
    await waitFor(() => read('first-ready'), pid => pid && pid !== initialPid);
    assert.deepEqual((await byCursor(restartCursor)).map(request => request.peer), ['first']);
    assert.equal((await list(undefined, { ...scope, run_id: current.run_id })).ok, true);
    assert.equal((await requests()).at(-1).peer, 'first');
    const wallBefore = Date.now();
    const detail = await apply('get_live_config_user', { ...scope, run_id: current.run_id, user_id: '41' });
    assert.equal(detail.result.accounting_time_ms, '1800000000000');
    assert.match(detail.result.host_time_ms, /^[1-9][0-9]*$/);
    const wallAfter = Date.now();
    assert.ok(Number(detail.result.host_time_ms) >= wallBefore && Number(detail.result.host_time_ms) <= wallAfter);
    assert.equal(detail.result.user.time['expire-after-first-usage-ms'], '86400000');
});

function rosterCore({ profileName, listNode, statusNode }) {
    const { appendFileSync, readFileSync, writeFileSync } = require('node:fs');
    const { createServer } = require('node:net');
    const settings = JSON.parse(readFileSync(0, 'utf8'));
    const profile = settings.configs.map(path => JSON.parse(readFileSync(path, 'utf8')))
        .find(config => config.name === profileName);
    const listener = profile.nodes.find(node => node.type === 'TcpListener');
    const controls = () => JSON.parse(readFileSync('roster-controls.json', 'utf8'));
    const record = entry => appendFileSync('roster-requests.jsonl', JSON.stringify(entry) + '\n', { mode: 0o600 });
    const maxId = '18446744073709551615';
    let serial = 0;
    let firstMixedPageSent = false;
    let staleCursorSent = false;

    createServer(connection => {
        let input = Buffer.alloc(0);
        connection.on('error', () => {});
        connection.on('data', chunk => {
            input = Buffer.concat([input, chunk]);
            if (input.length < 4 || input.length < 4 + input.readUInt32BE(0)) return;
            const request = JSON.parse(input.subarray(4).toString('utf8'));
            const body = JSON.parse(request.body);
            const operation = request.operation;
            const node = request.selector.node;
            const control = controls()[node] ?? {};
            const id = ++serial;
            record({ id, node, operation, body, time: Date.now(), mode: control.mode });
            const config = control.config ?? 1;
            let stats = control.stats ?? 1;
            const count = control.count ?? (node === listNode ? 1000 : 2);
            const offset = body.cursor ? Number.parseInt(body.cursor, 16) - 1 : 0;
            const cursor = position => (position + 1).toString(16).padStart(64, '0');
            const token = config.toString(16).padStart(48, control.mode === 'new-instance' ? 'b' : '0');
            const envelope = { id: request.id, error: null, result: null };
            function result(data) {
                envelope.result = { code: data.error ? 1 : 0, content_type: 'application/json',
                    body_encoding: 'utf8', body: JSON.stringify(data) };
            }
            function send() {
                const payload = Buffer.from(JSON.stringify(envelope));
                const header = Buffer.alloc(4);
                header.writeUInt32BE(payload.length);
                record({ completed: id, time: Date.now() });
                connection.end(Buffer.concat([header, payload]));
            }

            if (node === listNode && control.mode === 'mixed') {
                if (!firstMixedPageSent && operation === 'list-users' && body.limit === 50 && !offset) {
                    firstMixedPageSent = true;
                } else if (firstMixedPageSent) {
                    stats += 1;
                }
            }
            if (control.mode === 'stats-backward' && offset === 37) {
                stats += 1;
            }
            const revisions = { config_revision: String(config), stats_revision: String(stats), edit_token: token };
            if (operation === 'status' && control.mode === 'status-fails') {
                envelope.error = { code: 'forbidden', message: 'private mocked status error' };
            } else if (operation === 'status') {
                result({ ...revisions, user_count: String(count), database_loaded: true, database_dirty: false,
                    primary_checkpoint_valid: true, configured_producer_count: '0', session_record_count: '0',
                    primary_save: { last_result: 'not_attempted', last_attempt_age_ms: null, last_success_age_ms: null },
                    audit: { enabled: false, healthy: true, queued_records: '0', written_records: '0',
                        dropped_records: '0', suppressed_rejections: '0', write_failures: '0', last_error: null } });
            } else if (control.mode === 'forbidden') {
                envelope.error = { code: 'forbidden', message: 'private mocked list error' };
            } else if (control.mode === 'stale-cursor' && offset && !staleCursorSent) {
                staleCursorSent = true;
                result({ error: { code: 'stale_cursor' } });
            } else {
                // The management limit is an upper bound. Force short nonterminal pages.
                const end = Math.min(offset + Math.min(body.limit, 37), count);
                const users = [];
                for (let i = offset; i < end; ++i) {
                    const record = control.mode === 'duplicate' && offset && i === offset ? 0 : i;
                    users.push({ id: record === 998 ? maxId : String(record + 1), gid: '0', enabled: true,
                        name_preview: i === 0 && control.name ? control.name :
                            record === 999 ? 'Alice' : `Account ${record + 1}`,
                        name_preview_state: 'complete', traffic_limit: { u: '0', d: '0', total: '0' },
                        usage: { u: record === 0 && stats >= 3 ? '18446744073709551615' : '0',
                            d: record === 999 ? control.aliceUsage ?? '0' : '0' },
                        time: { expire_at_ms: '0', expire_after_first_usage_ms: '0', first_usage_at_ms: '0' },
                        email: 'private mocked account email', password: 'private mocked account password' });
                }
                const next = control.mode === 'loop' && offset ? body.cursor : cursor(end);
                const total = control.mode === 'wrong-total' && offset ? count + 1 : count;
                result({ ...revisions, accounting_time_ms: '1800000000000', users, total_users: String(total),
                    next_cursor: end < count ? next : null });
            }
            if (node === statusNode && operation === 'status' && control.delay) {
                setTimeout(send, control.delay);
            } else if (node === listNode && control.mode === 'initial' && !offset && body.limit === 50) {
                setTimeout(send, 240);
            } else {
                send();
            }
        });
    }).listen(listener.settings.port, '127.0.0.1', () => writeFileSync('roster-ready', String(process.pid)));
}

function rosterViewer(socket, subscription, scope) {
    const frames = [];
    const frameBytes = new WeakMap();
    socket.addEventListener('message', message => {
        const frame = JSON.parse(message.data);
        const payload = Buffer.byteLength(message.data);
        frameBytes.set(frame, payload + (payload < 126 ? 2 : payload <= 65535 ? 4 : 10));
        frames.push(frame);
    });
    const send = (action, fields) => socket.send(JSON.stringify({ action, subscription, ...fields }));
    const roster = frame => frame.type === 'users_roster' && frame.subscription === subscription;
    const wait = (matches, from = 0) => waitFor(() => frames.slice(from).find(matches), Boolean);
    const event = (name, from) => wait(frame => roster(frame) && frame.event === name, from);
    const state = (name, from) => wait(frame => roster(frame) && frame.event === 'state' && frame.state === name, from);
    async function transfer(from) {
        const end = await event('end', from);
        const attempt = frames.filter(frame => roster(frame) && frame.attempt === end.attempt);
        const begin = attempt.find(frame => frame.event === 'begin');
        const chunks = attempt.filter(frame => frame.event === 'chunk');
        assert.ok(begin, 'A transfer must start with a begin frame.');
        assert.equal(chunks.length, end.chunks);
        assert.deepEqual(chunks.map(chunk => chunk.index), chunks.map((_, index) => index));
        assert.doesNotMatch(JSON.stringify(attempt), /edit_token|if_match|next_cursor|private mocked account/);
        return { begin, chunks, bytes: attempt.reduce((bytes, frame) => bytes + frameBytes.get(frame), 0),
            rows: chunks.flatMap(chunk => chunk.rows ?? []),
            removed: chunks.flatMap(chunk => chunk.removed ?? []) };
    }
    return { frames, event, state, transfer, wait, socket,
        subscribe: () => send('users_roster_subscribe', scope),
        refresh: fields => send('users_roster_refresh', fields), unsubscribe: () => send('users_roster_unsubscribe') };
}

test('shares complete live rosters, reconciles mixed revisions, and retains only authorized complete rows', {
    skip: process.platform !== 'linux', timeout: 32_000,
}, async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    const core = await seedCore(fixture.backend);
    const profileName = 'Roster profile';
    const listNode = 'list only';
    const statusNode = 'status and list';
    const blockedNode = 'status only';
    const coreDirectory = dirname(fixture.backend);
    const controlPath = join(coreDirectory, 'roster-controls.json');
    const options = JSON.stringify({ profileName, listNode, statusNode });
    const controls = { [listNode]: { mode: 'initial' }, [statusNode]: { delay: 950 } };
    const update = async (node, value) => {
        controls[node] = value;
        await writeFile(controlPath + '.new', JSON.stringify(controls), { mode: 0o600 });
        await rename(controlPath + '.new', controlPath);
    };
    await update(listNode, controls[listNode]);
    await writeFile(core.program, `#!/usr/bin/env node\n(${rosterCore})(${options});\n`, { mode: 0o700 });
    const read = name => readFile(join(coreDirectory, name), 'utf8').catch(error => {
        if (error.code === 'ENOENT') return '';
        throw error;
    });
    const records = async () => (await read('roster-requests.jsonl')).trim().split('\n').filter(Boolean)
        .map(line => JSON.parse(line));
    const reads = async node => (await records()).filter(request => request.node === node);

    const reservation = createServer();
    reservation.listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const { port } = reservation.address();
    t.after(() => reservation.listening && new Promise(resolve => reservation.close(resolve)));
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket: controlsSocket } = await openPreview(t, fixture.origin, session);
    const apply = async (action, fields = {}) => {
        const reply = await requestPreview(controlsSocket, action, fields);
        assert.equal(reply.ok, true, reply.feedback);
        return reply;
    };
    const created = await apply('create_core_config', { name: profileName });
    const [{ id: configId }] = created.data.server.core.configs;
    const { result: { config, revision } } = await apply('get_core_config', { id: configId });
    config.nodes = [listNode, statusNode, blockedNode].map(name => ({ name, type: 'AuthenticationServer' }));
    config.nodes.push({ name: 'roster listener', type: 'TcpListener', next: 'roster API',
        settings: { address: '127.0.0.1', port } });
    config.nodes.push({ name: 'roster API', type: 'APIServer', settings: { clients: [
        { name: 'roster client', token: 'a'.repeat(64), allow: [
            { config: profileName, node: listNode, operations: ['list-users'] },
            { config: profileName, node: statusNode, operations: ['list-users', 'status'] },
            { config: profileName, node: blockedNode, operations: ['status'] },
        ] },
    ] } });
    await apply('save_core_config', { id: configId, config, revision });
    await apply('set_core_config_enabled', { id: configId, enabled: true });
    await new Promise(resolve => reservation.close(resolve));
    const running = nextPreviewSnapshot(controlsSocket, frame => frame.data.server.core.state === 'running');
    await apply('start_core');
    const started = await running;
    assert.equal(started.users_roster, true);
    const corePid = await waitFor(() => read('roster-ready'), Boolean);
    const coreStatus = await readFile(`/proc/${corePid}/status`, 'utf8');
    const backendPid = /^PPid:\s*(\d+)/m.exec(coreStatus)[1];

    const access = started.data.server.core.configs[0].users_access;
    const secondLogin = await fetch(new URL(`${fixture.basePath}/api/auth/sign-in`, fixture.origin), {
        method: 'POST', headers: { Origin: fixture.origin, 'X-Wave-CSRF': '1', 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: 'fixture-admin', password: 'fixture core test passphrase 4Kq7v' }),
        signal: AbortSignal.timeout(timeout),
    });
    assert.equal(secondLogin.status, 204);
    const otherSession = secondLogin.headers.get('set-cookie').split(';', 1)[0];
    assert.notEqual(otherSession, session);
    const viewerScope = node => ({ config_id: configId, node, source: 'live',
            run_id: access[node].run_id, database_generation: access[node].database_generation,
            schematic_revision: started.schematic_revision });
    async function viewer(name, node, cookie = session) {
        const { socket } = await openPreview(t, fixture.origin, cookie);
        return rosterViewer(socket, name, viewerScope(node));
    }

    const first = await viewer('first-viewer', listNode);
    const second = await viewer('second-viewer', listNode, otherSession);
    const loadStarted = performance.now();
    first.subscribe();
    await waitFor(() => reads(listNode), entries => entries.length > 0);
    second.subscribe();
    const firstBegin = await first.event('begin');
    const requestId = 'baseline-interleaved-action';
    first.socket.send(JSON.stringify({ action: 'get_core_config', id: configId, request_id: requestId,
        schematic_revision: started.schematic_revision }));
    const actionReply = await first.wait(frame => frame.type === 'snapshot' && frame.request_id === requestId);
    assert.equal(actionReply.ok, true, actionReply.feedback);
    const initial = await Promise.all([first.transfer(), second.transfer()]);
    const loadMs = Math.round(performance.now() - loadStarted);
    assert.ok(first.frames.indexOf(actionReply) > first.frames.indexOf(firstBegin));
    assert.ok(first.frames.indexOf(actionReply) < first.frames.indexOf(await first.event('end')));
    for (const baseline of initial) {
        assert.equal(Number(baseline.begin.total), 1000);
        assert.equal(baseline.rows.length, 1000);
        assert.equal(new Set(baseline.rows.map(row => row.id)).size, 1000);
        assert.ok(baseline.rows.some(row => row.id === '18446744073709551615'));
        assert.deepEqual(baseline.removed, []);
    }
    assert.deepEqual(initial[0].rows, initial[1].rows);
    const traversal = (await reads(listNode)).filter(request => request.body.limit === 50);
    assert.equal(traversal.length, Math.ceil(1000 / 37));
    assert.equal(traversal.filter(request => !request.body.cursor).length, 1);

    const missedChunkFrom = second.frames.length;
    second.refresh({ resync: true });
    const resynced = await second.transfer(missedChunkFrom);
    assert.equal(resynced.begin.base_sequence, undefined);
    assert.deepEqual(resynced.rows, initial[1].rows);
    assert.deepEqual((await reads(listNode)).filter(request => request.body.limit === 50), traversal);

    const unchangedFrom = second.frames.length;
    const probes = await waitFor(async () => (await reads(listNode)).filter(request => request.body.limit === 1),
        probes => probes.length >= 3);
    for (let i = 1; i < 3; ++i) {
        const interval = probes[i].time - probes[i - 1].time;
        assert.ok(interval >= 300 && interval <= 1500, `Shared idle probe interval was ${interval} ms.`);
    }
    assert.ok(second.frames.slice(unchangedFrom).every(frame => frame.event !== 'chunk'));

    const status = await viewer('status-viewer', statusNode);
    status.subscribe();
    assert.equal((await status.transfer()).rows.length, 2);
    const unauthorized = await viewer('unauthorized-viewer', blockedNode);
    unauthorized.subscribe();
    await unauthorized.wait(frame => frame.type === 'users_roster' && frame.event === 'state' &&
        ['error', 'lost'].includes(frame.state));
    assert.deepEqual(await reads(blockedNode), []);
    assert.ok(unauthorized.frames.every(frame => frame.event !== 'chunk'));
    const pending = await waitFor(async () => {
        const log = await records();
        return log.find(entry => entry.node === statusNode && entry.operation === 'status' &&
            !log.some(other => other.completed === entry.id));
    }, Boolean);
    const backgroundReply = await apply('get_core_config', { id: configId });
    assert.ok(backgroundReply.result.config);
    assert.ok(!(await records()).some(entry => entry.completed === pending.id),
        'Workspace controls can respond while a roster read is pending.');
    await waitFor(() => reads(statusNode), entries => entries.filter(request => request.operation === 'status').length >= 2);
    const slowCalls = (await reads(statusNode)).filter(request => request.operation === 'status');
    const firstCompleted = (await records()).find(entry => entry.completed === slowCalls[0].id);
    assert.ok(firstCompleted.time <= slowCalls[1].time, 'Slow probes stay one at a time.');
    const fallbackStart = (await records()).at(-1).time;
    await update(statusNode, { mode: 'status-fails' });
    await waitFor(() => reads(statusNode), entries => entries.some(request => request.time >= fallbackStart &&
        request.mode === 'status-fails' && request.operation === 'list-users' && request.body.limit === 1));
    status.unsubscribe();

    const mixedFrom = [first.frames.length, second.frames.length];
    await update(listNode, { mode: 'mixed', stats: 2 });
    const changed = await Promise.all([first.transfer(mixedFrom[0]), second.transfer(mixedFrom[1])]);
    for (const delta of changed) {
        assert.ok(delta.begin.base_sequence !== undefined);
        assert.equal(delta.rows.length, 1);
        assert.equal(delta.rows[0].id, '1');
        assert.equal(delta.rows[0].stats.traffic.up, '18446744073709551615');
        assert.deepEqual(delta.removed, []);
    }

    const aliceFrom = [first.frames.length, second.frames.length];
    const aliceUsage = '9007199254740993';
    await update(listNode, { stats: 4, aliceUsage });
    const aliceDeltas = await Promise.all([first.transfer(aliceFrom[0]), second.transfer(aliceFrom[1])]);
    for (const delta of aliceDeltas) {
        assert.ok(delta.begin.base_sequence !== undefined);
        assert.equal(delta.rows.length, 1);
        assert.equal(delta.rows[0].id, '1000');
        assert.equal(delta.rows[0].name, 'Alice');
        assert.equal(delta.rows[0].stats.traffic.down, aliceUsage);
        assert.deepEqual(delta.removed, []);
    }

    const malformedWalks = ['duplicate', 'loop', 'wrong-total', 'stats-backward'];
    for (const [i, mode] of malformedWalks.entries()) {
        const from = second.frames.length;
        await update(listNode, { mode, config: i + 2, stats: 4, aliceUsage });
        second.refresh();
        await second.state('stale', from);
        assert.ok(second.frames.slice(from).every(frame => frame.event !== 'chunk'));
        const recoveryFrom = second.frames.length;
        const recovery = second.state('ready', recoveryFrom);
        await update(listNode, { config: i + 2, stats: 4, aliceUsage });
        second.refresh();
        await recovery;
        assert.ok(second.frames.slice(recoveryFrom).every(frame => frame.event !== 'chunk'),
            'Configuration-only changes recover without emitting unchanged account rows.');
    }

    const editedFrom = second.frames.length;
    const editedConfig = malformedWalks.length + 2;
    await update(listNode, { mode: 'stale-cursor', config: editedConfig, stats: 4, count: 999,
        name: 'Renamed account' });
    second.refresh();
    const edited = await second.transfer(editedFrom);
    assert.equal(Number(edited.begin.total), 999);
    assert.equal(edited.rows.length, 1);
    assert.equal(edited.rows[0].name, 'Renamed account');
    assert.deepEqual(edited.removed, ['1000']);
    assert.ok((await reads(listNode)).filter(request => request.mode === 'stale-cursor' &&
        request.body.limit === 50 && !request.body.cursor).length >= 2);

    const replacedFrom = second.frames.length;
    await update(listNode, { mode: 'new-instance', config: editedConfig, stats: 4, count: 999,
        name: 'Replacement account' });
    assert.equal((await second.state('lost', replacedFrom)).cause, 'identity');
    assert.ok(second.frames.slice(replacedFrom).every(frame => frame.event !== 'chunk'));
    const replacementFrom = second.frames.length;
    second.refresh();
    const replacement = await second.transfer(replacementFrom);
    assert.equal(replacement.begin.base_sequence, undefined);
    assert.equal(replacement.rows.length, 999);
    assert.equal(replacement.rows.find(row => row.id === '1').name, 'Replacement account');

    const lostFrom = second.frames.length;
    const firstLostFrom = first.frames.length;
    await update(listNode, { mode: 'forbidden' });
    assert.equal((await second.state('lost', lostFrom)).cause, 'permission');
    assert.equal((await first.state('lost', firstLostFrom)).cause, 'permission');
    assert.ok(second.frames.slice(lostFrom).every(frame => frame.event !== 'chunk'));
    first.unsubscribe();
    second.unsubscribe();
    const denied = rosterViewer(second.socket, 'still-denied', viewerScope(listNode));
    denied.subscribe();
    assert.equal((await denied.state('lost')).cause, 'permission');
    assert.ok(!denied.frames.some(frame => frame.subscription === 'still-denied' && frame.event === 'chunk'));
    denied.unsubscribe();

    await update(listNode, { mode: 'new-instance', config: editedConfig, stats: 4, count: 999,
        name: 'Replacement account' });
    const restored = rosterViewer(second.socket, 'single-click-recovery', viewerScope(listNode));
    restored.subscribe();
    assert.equal((await restored.transfer()).rows.length, 999);
    assert.ok(!restored.frames.some(frame => frame.subscription === 'single-click-recovery' && frame.state === 'lost'));
    assert.ok(!restored.frames.some(frame => frame.subscription === 'still-denied' && frame.event === 'chunk'));

    const restoredReads = (await reads(listNode)).length;
    await waitFor(() => reads(listNode), entries => entries.slice(restoredReads).some(request => request.body.limit === 1));
    restored.unsubscribe();
    await delay(700);
    const stopped = (await reads(listNode)).length;
    await delay(850);
    assert.equal((await reads(listNode)).length, stopped, 'The open browser showing a loss notice does not keep polling.');
    assert.ok(first.frames.slice(firstLostFrom).every(frame => frame.event !== 'chunk'),
        'Recovery in the other browser must not send rows to the released viewer.');

    const later = rosterViewer(first.socket, 'later-retry', viewerScope(listNode));
    later.subscribe();
    assert.equal((await later.transfer()).rows.length, 999);
    later.unsubscribe();
    const peakRssKiB = /^VmHWM:\s*(\d+)\s+kB/m.exec(await readFile(`/proc/${backendPid}/status`, 'utf8'))[1];
    t.diagnostic(`1,000 live accounts in two sockets loaded in ${loadMs} ms. Backend peak RSS ${peakRssKiB} KiB. ` +
        `Baseline ${initial[0].bytes} WebSocket bytes in ${initial[0].chunks.length} chunks per viewer. ` +
        `Idle polls ${probes[1].time - probes[0].time}/${probes[2].time - probes[1].time} ms apart. ` +
        `One last-page account update ${aliceDeltas[0].bytes} WebSocket bytes per viewer.`);
});
