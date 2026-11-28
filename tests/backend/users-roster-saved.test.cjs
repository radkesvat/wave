const assert = require('node:assert/strict');
const { once } = require('node:events');
const { readFile, rename, stat, symlink, unlink, utimes, writeFile } = require('node:fs/promises');
const { dirname, join } = require('node:path');
const { test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { authFixture, fixtureSession, openPreview, requestPreview, timeout } = require('./auth_fixture.cjs');

const rosterType = 'users_roster';
const profileName = 'Saved roster fixture';

function watch(socket, schematicRevision) {
    const frames = [];
    socket.addEventListener('message', message => frames.push({
        frame: JSON.parse(message.data), bytes: Buffer.byteLength(message.data),
    }));
    let request = 100;
    const send = (action, fields) => socket.send(JSON.stringify({ action, ...fields }));
    const wait = async (after, matches) => {
        const deadline = performance.now() + timeout;
        for (let index = after; performance.now() < deadline;) {
            for (; index < frames.length; ++index) {
                if (matches(frames[index].frame)) return frames[index];
            }
            await delay(10);
        }
        assert.fail(`Expected a workspace frame after position ${after}. Saw ${frames.slice(after).map(({ frame }) =>
            frame.type === rosterType ? `${frame.subscription}:${frame.event}:${frame.state ?? ''}` : frame.type).join(', ')}`);
    };
    return {
        frames, send, wait,
        subscribe(subscription, target) {
            send('users_roster_subscribe', { subscription, schematic_revision: schematicRevision, ...target });
        },
        async action(action, fields) {
            const requestId = ++request;
            const reply = wait(frames.length, frame => frame.type === 'snapshot' && frame.request_id === requestId);
            send(action, { request_id: requestId, schematic_revision: schematicRevision, ...fields });
            const { frame } = await reply;
            assert.equal(frame.action, action);
            assert.equal(frame.ok, true, frame.feedback);
            return frame.result;
        },
    };
}

async function transfer(viewer, subscription, after = 0) {
    const matches = event => frame => frame.type === rosterType && frame.subscription === subscription &&
        frame.event === event;
    const begin = await viewer.wait(after, matches('begin'));
    const end = await viewer.wait(after, frame => matches('end')(frame) && frame.attempt === begin.frame.attempt);
    const chunks = viewer.frames.slice(viewer.frames.indexOf(begin) + 1, viewer.frames.indexOf(end))
        .filter(({ frame }) => matches('chunk')(frame));
    assert.match(begin.frame.sequence, /^[1-9][0-9]*$/);
    assert.match(end.frame.host_time, /^[1-9][0-9]*$/);
    assert.equal(end.frame.accounting_time, undefined);
    assert.equal(chunks.length, end.frame.chunks);
    chunks.forEach(({ frame, bytes }, index) => {
        assert.equal(frame.attempt, begin.frame.attempt);
        assert.equal(frame.index, index);
        assert.ok(bytes <= 128 * 1024, `chunk size ${bytes} exceeds the stream budget`);
    });
    return {
        total: begin.frame.total,
        sequence: begin.frame.sequence,
        base: begin.frame.base_sequence,
        rows: chunks.flatMap(({ frame }) => frame.rows),
        removed: chunks.flatMap(({ frame }) => frame.removed),
        chunks: chunks.length,
        bytes: begin.bytes + end.bytes + chunks.reduce((sum, chunk) => sum + chunk.bytes, 0),
        nextOffset: viewer.frames.indexOf(end) + 1,
    };
}

function observedRows(viewer, subscription, after) {
    return viewer.frames.slice(after).filter(({ frame }) => frame.type === rosterType &&
        frame.subscription === subscription).flatMap(({ frame }) => frame.rows ?? []);
}

async function recordMemory(t, fixture, label) {
    if (process.platform === 'linux') {
        const status = await readFile(`/proc/${fixture.pid}/status`, 'utf8');
        const peak = /^VmHWM:\s+(\d+)\s+kB$/m.exec(status)?.[1];
        if (peak) t.diagnostic(`${label} reached ${peak} KiB peak backend RSS`);
    }
}

test('resets only the selected saved user’s traffic and publishes the updated row', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const session = await fixtureSession(fixture);
    const { socket, schematicRevision } = await openPreview(t, fixture.origin, session);
    const created = await requestPreview(socket, 'create_core_config', { name: 'Traffic reset', add_auth_nodes: true });
    assert.equal(created.ok, true, created.feedback);
    const [profile] = created.data.server.core.configs;
    const [node] = profile.auth_nodes;
    const { source, run_id, database_generation } = profile.users_access[node];
    const scope = { config_id: profile.id, node, source, run_id, database_generation };
    const { result: { config } } = await requestPreview(socket, 'get_core_config', { id: profile.id });
    const file = join(dirname(fixture.backend), config.nodes.find(entry => entry.name === node).settings['db-path']);
    const target = {
        id: '18446744073709551615', pass: 'unchanged-secret', enabled: false,
        time: { 'first-usage-at-ms': '123456789' }, limit: { traffic: { total: '98765' } },
        stats: { traffic: { u: '18446744073709551615', up: '1', d: '36', down: '2', extension: 'keep' },
            speed: { u: '7', d: '3' }, devices: '2' },
    };
    const neighbor = { id: '2', password: 'another-secret', stats: { traffic: { u: '15', d: '92' } } };
    const saved = { users: [target, neighbor], metadata: { keep: true } };
    await writeFile(file, JSON.stringify(saved));
    const viewer = watch(socket, schematicRevision);
    viewer.subscribe('reset', scope);
    const initial = await transfer(viewer, 'reset');
    const original = initial.rows.find(row => row.id === target.id);
    assert.deepEqual(original.stats.traffic, { up: target.stats.traffic.u, down: '36' });
    const input = { ...scope, roster: true, user_id: target.id };
    for (const invalid of [{ run_id: 'outdated' }, { user_id: '3' }]) {
        const denied = await requestPreview(socket, 'reset_config_user_traffic', { ...input, ...invalid });
        assert.equal(denied.ok, false);
        assert.deepEqual(JSON.parse(await readFile(file, 'utf8')), saved);
    }
    const result = await viewer.action('reset_config_user_traffic', input);
    assert.deepEqual(result, { user_id: target.id });
    const delta = await transfer(viewer, 'reset', initial.nextOffset);
    assert.equal(delta.base, initial.sequence);
    assert.deepEqual(delta.removed, []);
    const zero = { up: '0', down: '0' };
    assert.deepEqual(delta.rows, [{ ...original, stats: { ...original.stats, traffic: zero } }]);
    target.stats.traffic = { ...zero, extension: 'keep' };
    assert.deepEqual(JSON.parse(await readFile(file, 'utf8')), saved);
});

test('streams a complete saved roster and reconciles edits across sessions and database bindings', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const firstCookie = await fixtureSession(fixture);
    const secondCookie = await fixtureSession(fixture, { setup: false });
    assert.notEqual(firstCookie, secondCookie);
    const observer = await openPreview(t, fixture.origin, secondCookie);
    const observed = watch(observer.socket, observer.schematicRevision);
    const creation = await requestPreview(observer.socket, 'create_core_config', { name: profileName });
    assert.equal(creation.ok, true, creation.feedback);
    assert.equal(creation.users_roster, true);
    const profile = creation.data.server.core.configs.find(config => config.name === profileName);
    const getConfig = () => requestPreview(observer.socket, 'get_core_config', { id: profile.id });
    const initial = (await getConfig()).result;
    const names = ['staff', 'alias', 'other'];
    const saved = await requestPreview(observer.socket, 'save_core_config', {
        id: profile.id, revision: initial.revision,
        config: { ...initial.config, nodes: names.map(name => ({ name, type: 'AuthenticationServer' })) },
    });
    assert.equal(saved.ok, true, saved.feedback);
    const graph = saved.result.config;
    const graphFile = join(dirname(fixture.backend), 'configs', profile.name, 'config.json');
    const staffFile = join(dirname(fixture.backend), graph.nodes[0].settings['db-path']);
    const otherFile = join(dirname(fixture.backend), graph.nodes[2].settings['db-path']);
    // Configs imported from disk can bind multiple nodes to one file.
    graph.nodes[1].settings['db-path'] = graph.nodes[0].settings['db-path'];
    await writeFile(graphFile, JSON.stringify(graph));
    const scope = (snapshot, node) => {
        const access = snapshot.data.server.core.configs.find(config => config.id === profile.id).users_access[node];
        return { config_id: profile.id, node, source: 'offline', run_id: access.run_id,
            database_generation: access.database_generation };
    };
    const configured = await getConfig();
    const staff = scope(configured, 'staff');
    const alias = scope(configured, 'alias');
    assert.notEqual(staff.database_generation, alias.database_generation);

    const max = '18446744073709551615';
    const accounts = Array.from({ length: 1000 }, (_, index) => ({
        id: index === 999 ? max : String(index + 1), password: `private-credential-${index}`,
        name: index === 999 ? `${'é'.repeat(128)}x` : `Account ${index + 1}`,
        notes: index === 998 ? 'private-note'.repeat(1450) : 'private-note', email: 'private@example.test',
        stats: { traffic: { up: index === 999 ? max : '0', ...(index === 0 ? {} : { down: '0' }) } },
    }));
    await writeFile(staffFile, JSON.stringify({ users: accounts, wrapper_metadata: { keep: true } }));
    await writeFile(otherFile, JSON.stringify({ users: [{ id: '1', name: 'Unrelated account', password: 'other-pass' }] }));
    const primarySocket = await openPreview(t, fixture.origin, firstCookie);
    const peerSocket = await openPreview(t, fixture.origin, secondCookie);
    const first = watch(primarySocket.socket, primarySocket.schematicRevision);
    const second = watch(peerSocket.socket, peerSocket.schematicRevision);
    first.subscribe('1', { ...staff, database_generation: 'wrong' });
    await first.wait(0, frame => frame.type === rosterType && frame.subscription === '1' && frame.event === 'state');

    const start = performance.now();
    first.subscribe('2', staff);
    second.subscribe('3', alias);
    const [primary, peer, detail] = await Promise.all([
        transfer(first, '2'), transfer(second, '3'),
        first.action('get_config_user', { ...staff, user_id: '1' }),
    ]);
    t.diagnostic(`1,000 saved accounts loaded in ${Math.round(performance.now() - start)} ms across two sockets. ` +
        `The primary baseline used ${primary.chunks} chunks and ${primary.bytes} bytes.`);
    await recordMemory(t, fixture, 'Initial saved load');
    assert.equal(detail.user.notes, 'private-note');
    assert.equal(detail.user.password, undefined);
    assert.equal(primary.total, 1000);
    assert.equal(primary.base, undefined);
    assert.ok(primary.chunks > 1);
    assert.deepEqual(primary.removed, []);
    assert.deepEqual(peer.rows, primary.rows);
    assert.equal(new Set(primary.rows.map(row => row.id)).size, 1000);
    assert.equal(observedRows(first, '1', 0).length, 0);
    assert.doesNotMatch(JSON.stringify(primary.rows), /private-credential|private-note|private@example|edit_token/);
    assert.equal(primary.rows.find(row => row.id === '1').stats.traffic.down, null);
    assert.equal(primary.rows.find(row => row.id === '2').stats.traffic.down, '0');
    assert.equal(primary.rows.find(row => row.id === '999').oversized, true);
    const last = primary.rows.find(row => row.id === max);
    assert.equal(last.stats.traffic.up, max);
    assert.equal(last.name_preview_state, 'truncated');
    assert.equal(last.name, 'é'.repeat(128));

    let revisions = [primary.sequence, peer.sequence];
    const changes = async (action, input, expected) => {
        const afterFirst = first.frames.length;
        const afterSecond = second.frames.length;
        const reply = await first.action(action, { ...staff, roster: true, ...input });
        assert.deepEqual(reply, { user_id: '1001' });
        const deltas = await Promise.all([transfer(first, '2', afterFirst), transfer(second, '3', afterSecond)]);
        deltas.forEach((delta, index) => {
            assert.equal(delta.base, revisions[index]);
            assert.deepEqual(delta.rows.map(row => row.name), expected ? [expected] : []);
            assert.deepEqual(delta.removed, expected ? [] : ['1001']);
        });
        revisions = deltas.map(delta => delta.sequence);
        return deltas[0].bytes;
    };
    await changes('create_config_user', { user: { id: '1001', name: 'New account', password: 'new-credential' } },
        'New account');
    const editBytes = await changes('edit_config_user', { user_id: '1001', user: { name: 'Updated account' } },
        'Updated account');
    t.diagnostic(`One changed saved summary used ${editBytes} bytes including stream envelopes`);
    await changes('delete_config_user', { user_id: '1001' });

    const external = JSON.parse(await readFile(staffFile, 'utf8'));
    assert.deepEqual(external.wrapper_metadata, { keep: true });
    assert.equal(external.users[0].notes, 'private-note');
    external.users[0].name = 'External edit';
    await writeFile(staffFile, JSON.stringify(external));
    await utimes(staffFile, new Date('2020-01-01T00:00:00Z'), new Date());
    const accessTime = (await stat(staffFile)).atimeMs;
    const after = first.frames.length;
    await delay(1150);
    assert.equal((await stat(staffFile)).atimeMs, accessTime, 'idle saved viewers reread the database');
    assert.equal(observedRows(first, '2', after).length, 0);
    first.send('users_roster_refresh', { subscription: '2' });
    const refreshed = await transfer(first, '2', after);
    assert.equal(refreshed.total, 1000);
    assert.equal(refreshed.rows.find(row => row.id === '1').name, 'External edit');

    for (const account of external.users.slice(2)) account.notes = 'x'.repeat(16450);
    const nearLimit = JSON.stringify(external);
    assert.ok(Buffer.byteLength(nearLimit) > 15 * 1024 * 1024);
    assert.ok(Buffer.byteLength(nearLimit) < 16 * 1024 * 1024);
    await writeFile(staffFile, nearLimit);
    const raceAt = first.frames.length;
    first.send('users_roster_refresh', { subscription: '2' });
    const loading = await first.wait(raceAt, frame => frame.type === rosterType && frame.subscription === '2' &&
        frame.event === 'state' && frame.state === 'loading');
    const raceWrite = await first.action('edit_config_user', {
        ...staff, roster: true, user_id: '1', user: { name: 'Concurrent edit' },
    });
    assert.deepEqual(raceWrite, { user_id: '1' });
    await first.wait(first.frames.indexOf(loading) + 1, frame => frame.type === rosterType &&
        frame.subscription === '2' && frame.event === 'state' && frame.state === 'ready');
    await first.wait(raceAt, frame => frame.type === rosterType && frame.subscription === '2' &&
        frame.rows?.some(row => row.id === '1' && row.name === 'Concurrent edit'));
    assert.equal(observedRows(first, '2', raceAt).filter(row => row.id === '1').at(-1).name, 'Concurrent edit');
    await recordMemory(t, fixture, 'Near-limit refresh and concurrent saved edit');

    const moved = staffFile + '.fixture-backup';
    await rename(staffFile, moved);
    await symlink(moved, staffFile);
    const lostAt = [first.frames.length, second.frames.length];
    first.send('users_roster_refresh', { subscription: '2' });
    await Promise.all([first, second].map((viewer, index) => viewer.wait(lostAt[index],
        frame => frame.type === rosterType && frame.event === 'state' && frame.state === 'lost')));
    assert.equal(observedRows(first, '2', lostAt[0]).length, 0);
    assert.equal(observedRows(second, '3', lostAt[1]).length, 0);
    await unlink(staffFile);
    await rename(moved, staffFile);
    const restoredAt = [first.frames.length, second.frames.length];
    first.send('users_roster_refresh', { subscription: '2' });
    second.send('users_roster_refresh', { subscription: '3' });
    const restored = await Promise.all([transfer(first, '2', restoredAt[0]), transfer(second, '3', restoredAt[1])]);
    assert.ok(restored.every(result => result.base === undefined && result.total === 1000));

    graph.nodes[0].settings['db-path'] = graph.nodes[2].settings['db-path'];
    await writeFile(graphFile, JSON.stringify(graph));
    const remapAt = first.frames.length;
    const remapped = scope(await getConfig(), 'staff');
    assert.notEqual(remapped.database_generation, staff.database_generation);
    await first.wait(remapAt, frame => frame.type === rosterType && frame.subscription === '2' &&
        frame.event === 'state' && frame.state === 'lost');
    first.subscribe('4', remapped);
    const alternate = await transfer(first, '4', remapAt);
    assert.deepEqual(alternate.rows.map(row => row.name), ['Unrelated account']);
    assert.equal(observedRows(first, '2', remapAt).length, 0);

    const closed = once(primarySocket.socket, 'close', { signal: AbortSignal.timeout(timeout) });
    const loggedOut = await fetch(new URL(`${fixture.basePath}/api/auth/sign-out`, fixture.origin), {
        method: 'POST', body: '{}',
        headers: { Origin: fixture.origin, 'X-Wave-CSRF': '1', 'Content-Type': 'application/json', Cookie: firstCookie },
    });
    assert.equal(loggedOut.status, 204);
    await closed;
    const stayedSignedIn = await second.action('get_config_user', { ...alias, user_id: '1' });
    assert.equal(stayedSignedIn.user.name, 'Concurrent edit');

    await writeFile(staffFile, '{"users":[]}');
    const emptyAt = second.frames.length;
    second.send('users_roster_refresh', { subscription: '3' });
    const emptied = await transfer(second, '3', emptyAt);
    assert.equal(emptied.total, 0);
    assert.equal(emptied.removed.length, 1000);
    const resyncAt = second.frames.length;
    second.send('users_roster_refresh', { subscription: '3', resync: true });
    const resynced = await transfer(second, '3', resyncAt);
    assert.equal(resynced.total, 0);
    assert.equal(resynced.base, undefined);
    assert.equal(resynced.chunks, 0);
    assert.equal(observed.frames.some(({ frame }) => frame.type === rosterType), false);
});

test('validates saved entry and recovers shared file loss and temporary config errors in one subscribe', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const firstCookie = await fixtureSession(fixture);
    const secondCookie = await fixtureSession(fixture, { setup: false });
    assert.notEqual(firstCookie, secondCookie);
    const { socket: observer } = await openPreview(t, fixture.origin, firstCookie);
    const name = 'Saved entry validation';
    const created = await requestPreview(observer, 'create_core_config', { name });
    assert.equal(created.ok, true, created.feedback);
    const profile = created.data.server.core.configs.find(config => config.name === name);
    const initial = (await requestPreview(observer, 'get_core_config', { id: profile.id })).result;
    const saved = await requestPreview(observer, 'save_core_config', {
        id: profile.id, revision: initial.revision,
        config: { ...initial.config, nodes: ['staff', 'other'].map(node => ({ name: node, type: 'AuthenticationServer' })) },
    });
    assert.equal(saved.ok, true, saved.feedback);
    const graph = saved.result.config;
    const directory = dirname(fixture.backend);
    const graphFile = join(directory, 'configs', name, 'config.json');
    const staffFile = join(directory, graph.nodes[0].settings['db-path']);
    const otherFile = join(directory, graph.nodes[1].settings['db-path']);
    const initialGraph = await readFile(graphFile, 'utf8');
    const access = snapshot => snapshot.data.server.core.configs.find(config => config.id === profile.id);
    const scope = snapshot => {
        const { source, run_id, database_generation } = access(snapshot).users_access.staff;
        return { config_id: profile.id, node: 'staff', source, run_id, database_generation };
    };
    const original = scope(saved);
    const users = Array.from({ length: 1000 }, (_, index) => ({
        id: String(index + 1), name: `Account ${index + 1}`, password: `saved-entry-password-${index + 1}`,
    }));
    const writeRoster = () => writeFile(staffFile, JSON.stringify({ users }));
    await writeRoster();
    await writeFile(otherFile, JSON.stringify({ users: [{ id: '1', name: 'Other account', password: 'other-entry-password' }] }));

    const a = await openPreview(t, fixture.origin, firstCookie);
    const b = await openPreview(t, fixture.origin, secondCookie);
    const first = watch(a.socket, a.schematicRevision);
    const second = watch(b.socket, b.schematicRevision);
    const state = (viewer, subscription, value, after = 0) => viewer.wait(after,
        frame => frame.type === rosterType && frame.subscription === subscription &&
            frame.event === 'state' && frame.state === value);
    const noData = (viewer, subscription, after) => assert.ok(viewer.frames.slice(after).every(({ frame }) =>
        frame.subscription !== subscription || (frame.event !== 'begin' && frame.event !== 'chunk')));

    first.subscribe('initial', original);
    const baseline = await transfer(first, 'initial');
    assert.equal(baseline.total, 1000);
    users[0].name = 'Externally updated name';
    await writeRoster();
    const editAt = first.frames.length;
    second.subscribe('first-entry', original);
    const [diff, fromDisk] = await Promise.all([transfer(first, 'initial', editAt), transfer(second, 'first-entry')]);
    assert.equal(diff.base, baseline.sequence);
    assert.deepEqual(diff.rows.map(row => [row.id, row.name]), [['1', users[0].name]]);
    assert.equal(fromDisk.base, undefined);
    assert.equal(fromDisk.total, 1000);
    assert.equal(fromDisk.rows.find(row => row.id === '1').name, users[0].name);

    // Reset atime to distinguish an on-entry read from an unchanged cached snapshot.
    await utimes(staffFile, new Date('2020-01-01T00:00:00Z'), new Date());
    const idleAtime = (await stat(staffFile)).atimeMs;
    const unchangedAt = first.frames.length;
    second.subscribe('first-entry', original);
    await delay(1125);
    assert.equal((await stat(staffFile)).atimeMs, idleAtime, 'Duplicate entry and idle viewers must not read the file.');
    second.send('users_roster_unsubscribe', { subscription: 'first-entry' });
    second.subscribe('unchanged-entry', original);
    const unchanged = await transfer(second, 'unchanged-entry');
    assert.deepEqual(unchanged.rows, fromDisk.rows);
    assert.ok((await stat(staffFile)).atimeMs > idleAtime, 'A genuine re-entry must read even the unchanged file.');
    noData(first, 'initial', unchangedAt);

    // Wait until a refresh has opened its file snapshot, then give a later entry a new file to verify.
    const nextFile = staffFile + '.next-entry';
    const oldAtime = new Date('2020-01-01T00:00:00Z');
    await writeFile(nextFile, JSON.stringify({ users }), { mode: 0o600 });
    await utimes(nextFile, oldAtime, new Date());
    await writeFile(staffFile, JSON.stringify({ users, padding: 'x'.repeat(15 * 1024 * 1024) }));
    await utimes(staffFile, oldAtime, new Date());
    first.send('users_roster_refresh', { subscription: 'initial' });
    const readDeadline = performance.now() + timeout;
    while ((await stat(staffFile)).atimeMs === oldAtime.getTime() && performance.now() < readDeadline) {
        await delay(5);
    }
    assert.ok((await stat(staffFile)).atimeMs > oldAtime.getTime());
    await rename(nextFile, staffFile);
    second.subscribe('later-entry', original);
    assert.deepEqual((await transfer(second, 'later-entry')).rows, fromDisk.rows);
    assert.ok((await stat(staffFile)).atimeMs > oldAtime.getTime(), 'The later entry must validate the new file.');

    await writeFile(staffFile, '{"users":');
    second.send('users_roster_unsubscribe', { subscription: 'later-entry' });
    const invalidAt = second.frames.length;
    second.subscribe('invalid-document', original);
    second.send('users_roster_refresh', { subscription: 'invalid-document', resync: true });
    await Promise.all([state(first, 'initial', 'stale', unchangedAt), state(second, 'invalid-document', 'error', invalidAt)]);
    noData(second, 'invalid-document', invalidAt);
    await writeRoster();

    // A missing file invalidates an existing viewer and a newcomer before any cached rows are emitted.
    const moved = staffFile + '.test-backup';
    await rename(staffFile, moved);
    const missingAt = [first.frames.length, second.frames.length];
    second.subscribe('missing-file', original);
    const missing = await Promise.all([
        state(first, 'initial', 'lost', missingAt[0]), state(second, 'missing-file', 'lost', missingAt[1]),
    ]);
    assert.ok(missing.every(({ frame }) => frame.cause === 'identity'));
    noData(second, 'missing-file', missingAt[1]);
    second.subscribe('still-missing', original);
    await state(second, 'still-missing', 'lost');
    noData(second, 'still-missing', missingAt[1]);

    await rename(moved, staffFile);
    const restoredAt = first.frames.length;
    first.subscribe('single-retry', original);
    const recovered = await transfer(first, 'single-retry', restoredAt);
    assert.equal(recovered.base, undefined);
    assert.deepEqual(recovered.rows, fromDisk.rows);
    assert.equal(first.frames.slice(restoredAt).some(({ frame }) =>
        frame.subscription === 'single-retry' && frame.state === 'lost'), false);
    second.subscribe('current', original);
    await transfer(second, 'current');

    // A temporary graph error keeps the validated tabs and binding identity visible while blocking actions.
    const graphErrorAt = [first.frames.length, second.frames.length];
    await writeFile(graphFile, '{"nodes":');
    second.send('users_roster_refresh', { subscription: 'current' });
    const graphLoss = await Promise.all([
        state(first, 'single-retry', 'lost', graphErrorAt[0]), state(second, 'current', 'lost', graphErrorAt[1]),
    ]);
    assert.ok(graphLoss.every(({ frame }) => frame.cause === 'config_unverified' && /valid JSON/.test(frame.message)));
    const blocked = await requestPreview(observer, 'edit_config_user', {
        ...original, roster: true, user_id: '1', user: { name: 'Unsafe edit' },
    });
    assert.equal(blocked.ok, false);
    assert.match(blocked.feedback, /valid JSON/);
    assert.deepEqual(scope(blocked), original);
    assert.deepEqual(access(blocked).auth_nodes, ['staff', 'other']);
    assert.equal(access(blocked).users_access.staff.binding_error, blocked.feedback);
    assert.equal(JSON.parse(await readFile(staffFile, 'utf8')).users[0].name, users[0].name);
    first.send('users_roster_refresh', { subscription: 'single-retry', resync: true });
    await delay(175);
    noData(first, 'single-retry', graphErrorAt[0]);

    await writeFile(graphFile, initialGraph);
    first.subscribe('repaired-graph', original);
    const repaired = await transfer(first, 'repaired-graph');
    assert.deepEqual(repaired.rows, fromDisk.rows);
    const healthy = await requestPreview(observer, 'get_core_config', { id: profile.id });
    assert.equal(healthy.ok, true, healthy.feedback);
    assert.deepEqual(scope(healthy), original);
    assert.equal(access(healthy).users_access.staff.binding_error, undefined);

    // A repair to a different file must reject the editor's original scope.
    await writeFile(graphFile, '{"nodes":');
    first.send('users_roster_refresh', { subscription: 'repaired-graph' });
    await state(first, 'repaired-graph', 'lost');
    graph.nodes[0].settings['db-path'] = graph.nodes[1].settings['db-path'];
    await writeFile(graphFile, JSON.stringify(graph));
    first.subscribe('obsolete-retry', original);
    const obsolete = await state(first, 'obsolete-retry', 'lost');
    assert.equal(obsolete.frame.cause, 'identity');
    const remap = await requestPreview(observer, 'edit_config_user', {
        ...original, roster: true, user_id: '1', user: { name: 'Old draft' },
    });
    assert.equal(remap.ok, false);
    assert.match(remap.feedback, /scope changed/);
    const current = scope(remap);
    assert.notEqual(current.database_generation, original.database_generation);
    first.subscribe('remapped', current);
    assert.deepEqual((await transfer(first, 'remapped')).rows.map(row => row.name), ['Other account']);
    noData(first, 'obsolete-retry', 0);
    t.diagnostic('Two sign-ins verified external rename on entry, unchanged re-entry, single-subscribe recovery and remap isolation.');
});

test('keeps a saved binding through a malformed node list and removes it for a valid empty list', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const { socket, schematicRevision } = await openPreview(t, fixture.origin, await fixtureSession(fixture));
    const viewer = watch(socket, schematicRevision);
    const name = 'Node shape fixture';
    const created = await requestPreview(socket, 'create_core_config', { name });
    const profile = reply => reply.data.server.core.configs.find(row => row.name === name);
    const id = profile(created).id;
    const graphFile = join(dirname(fixture.backend), 'configs', name, 'config.json');
    const open = () => requestPreview(socket, 'get_core_config', { id });
    const save = (config, revision) => requestPreview(socket, 'save_core_config', { id, revision, config });
    const { result } = await open();
    const saved = await save({ ...result.config, nodes: [{ name: 'staff', type: 'AuthenticationServer' }] }, result.revision);
    assert.equal(saved.ok, true, saved.feedback);
    const graph = saved.result.config;
    const [staff] = graph.nodes;
    const { run_id, database_generation } = profile(saved).users_access.staff;
    const scope = { config_id: id, node: 'staff', source: 'offline', run_id, database_generation };
    await viewer.action('create_config_user', { ...scope, roster: true, user: { id: '42', password: 'shape-user' } });
    viewer.subscribe('original', scope);
    assert.deepEqual((await transfer(viewer, 'original')).rows.map(row => row.id), ['42']);

    let lastRevision;
    for (const [invalid, message] of [
        [null, /nodes array/], [{}, /nodes array/], [[staff, {}], /name and type/],
        [[staff, staff], /Duplicate node name/], [[{ ...staff, settings: null }], /must be an object/],
        [[{ ...staff, settings: { 'db-path': 42 } }], /database path/],
    ]) {
        await writeFile(graphFile, JSON.stringify({ ...graph, nodes: invalid }));
        if (invalid === null) {
            viewer.send('users_roster_refresh', { subscription: 'original' });
            await viewer.wait(0, frame => frame.subscription === 'original' && frame.cause === 'config_unverified');
        }
        const parsed = await open();
        assert.equal(parsed.ok, true, parsed.feedback);
        assert.deepEqual(parsed.result.config.nodes, invalid);
        lastRevision = parsed.result.revision;
        assert.deepEqual(profile(parsed).auth_nodes, ['staff']);
        assert.equal(profile(parsed).users_access.staff.database_generation, database_generation);
        assert.match(profile(parsed).users_access.staff.binding_error, message);
        for (const action of ['get_config_user', 'get_config_user_password', 'delete_config_user']) {
            const blocked = await requestPreview(socket, action, { ...scope, user_id: '42' });
            assert.equal(blocked.ok, false, action);
            assert.match(blocked.feedback, message);
        }
    }

    const repaired = await save(graph, lastRevision);
    assert.equal(repaired.ok, true, repaired.feedback);
    assert.equal(repaired.result.config.nodes[0].settings['db-path'], staff.settings['db-path']);
    assert.equal(profile(repaired).users_access.staff.database_generation, database_generation);
    assert.equal(profile(repaired).users_access.staff.binding_error, undefined);
    viewer.subscribe('repaired', scope);
    assert.deepEqual((await transfer(viewer, 'repaired')).rows.map(row => row.id), ['42']);

    await writeFile(graphFile, JSON.stringify({ ...graph, nodes: [] }));
    const empty = await open();
    assert.equal(empty.ok, true, empty.feedback);
    assert.equal(profile(empty).auth_nodes, undefined);
    await viewer.wait(0, frame => frame.subscription === 'repaired' && frame.cause === 'identity');
    const removed = await requestPreview(socket, 'get_config_user', { ...scope, user_id: '42' });
    assert.equal(removed.ok, false);
});

test('delivers only changed saved rows across bursts and pinned roster transfers', async t => {
    const fixture = await authFixture(t, undefined, { encrypted: false });
    await fixture.start();
    const firstCookie = await fixtureSession(fixture);
    const secondCookie = await fixtureSession(fixture, { setup: false });
    const connections = await Promise.all([firstCookie, secondCookie].map(cookie => openPreview(t, fixture.origin, cookie)));
    const [first, second] = connections.map(({ socket, schematicRevision }) => watch(socket, schematicRevision));
    const controls = connections[0].socket;
    const name = 'Burst roster fixture';
    const created = await requestPreview(controls, 'create_core_config', { name });
    assert.equal(created.ok, true, created.feedback);
    const profile = created.data.server.core.configs.find(config => config.name === name);
    const opened = await requestPreview(controls, 'get_core_config', { id: profile.id });
    const saved = await requestPreview(controls, 'save_core_config', {
        id: profile.id, revision: opened.result.revision,
        config: { ...opened.result.config, nodes: [{ name: 'staff', type: 'AuthenticationServer' }] },
    });
    assert.equal(saved.ok, true, saved.feedback);
    const access = saved.data.server.core.configs.find(config => config.id === profile.id).users_access.staff;
    const scope = { config_id: profile.id, node: 'staff', source: 'offline', run_id: access.run_id,
        database_generation: access.database_generation };
    const file = join(dirname(fixture.backend), saved.result.config.nodes[0].settings['db-path']);
    const accounts = Array.from({ length: 1000 }, (_, index) => ({
        id: String(index + 1), name: `Account ${index + 1}`, password: `unique fixture credential ${index + 1}`,
    }));
    await writeFile(file, JSON.stringify({ users: accounts }));
    const subscriptions = ['burst-first', 'burst-second'];
    const viewers = [first, second];
    viewers.forEach((viewer, index) => viewer.subscribe(subscriptions[index], scope));
    const initial = await Promise.all(viewers.map((viewer, index) => transfer(viewer, subscriptions[index])));
    initial.forEach(result => assert.equal(result.rows.length, 1000));
    assert.equal(initial[0].sequence, initial[1].sequence);

    const edit = (id, name) => first.action('edit_config_user',
        { ...scope, roster: true, user_id: id, user: { name } });
    async function catchUp(viewer, subscription, offset, base, expectedSequence) {
        const transfers = [];
        let sequence = BigInt(base);
        let nextOffset = offset;
        while (sequence < expectedSequence) {
            const delta = await transfer(viewer, subscription, nextOffset);
            assert.equal(delta.base, String(sequence), 'catch-up uses the last delivered version');
            assert.ok(BigInt(delta.sequence) > sequence);
            sequence = BigInt(delta.sequence);
            nextOffset = delta.nextOffset;
            transfers.push(delta);
        }
        assert.equal(sequence, expectedSequence);
        const frames = viewer.frames.slice(offset, nextOffset)
            .filter(({ frame }) => frame.type === rosterType && frame.subscription === subscription);
        return {
            rows: transfers.flatMap(delta => delta.rows),
            removed: transfers.flatMap(delta => delta.removed),
            bytes: frames.reduce((sum, entry) => sum + entry.bytes, 0),
            deltas: transfers.length,
        };
    }

    let sequence = BigInt(initial[0].sequence);
    for (const ids of [['1', '2'], ['3', '4', '5', '6', '7']]) {
        const position = viewers.map(viewer => viewer.frames.length);
        await Promise.all(ids.map(id => edit(id, `Burst account ${id}`)));
        const expectedSequence = sequence + BigInt(ids.length);
        const delivered = await Promise.all(viewers.map((viewer, index) =>
            catchUp(viewer, subscriptions[index], position[index], sequence, expectedSequence)));
        delivered.forEach(result => {
            assert.deepEqual(result.rows.map(row => row.id).sort(), ids);
            assert.deepEqual(result.removed, []);
            assert.ok(result.bytes < 8 * 1024, 'a small burst fits in small deltas, without a full snapshot');
        });
        t.diagnostic(`${ids.length} edited IDs, no full snapshots. Per viewer ${delivered.map(result =>
            `${result.rows.length} rows in ${result.deltas} deltas and ${result.bytes} payload bytes`).join(' / ')}.`);
        sequence = expectedSequence;
    }

    const addTransient = () => first.action('create_config_user',
        { ...scope, roster: true, user: { id: '1001', name: 'Transient', password: 'transient fixture credential' } });
    const deleteTransient = () => first.action('delete_config_user', { ...scope, roster: true, user_id: '1001' });
    for (const keepEdit of [true, false]) {
        const label = keepEdit ? 'Pinned' : 'Reverted';
        await writeFile(file, JSON.stringify({ users: accounts.map(user => ({ ...user, name: `${label} ${user.id}` })) }));
        const position = viewers.map(viewer => viewer.frames.length);
        first.send('users_roster_refresh', { subscription: subscriptions[0] });
        const pinnedBegin = await second.wait(position[1], frame => frame.type === rosterType &&
            frame.subscription === subscriptions[1] && frame.event === 'begin');
        assert.equal(pinnedBegin.frame.base_sequence, String(sequence));
        const writes = [addTransient(), edit('1', `${label} temporary`), edit('1', `${label} 1`)];
        if (keepEdit) {
            writes.push(edit('2', `${label} intermediate`), edit('2', `${label} final`));
        }
        writes.push(deleteTransient());
        await Promise.all(writes);
        const pinned = await transfer(second, subscriptions[1], position[1]);
        assert.equal(pinned.rows.length, 1000);
        assert.ok(pinned.chunks > 1);
        assert.equal(pinned.rows.find(row => row.id === '2').name, `${label} 2`);
        const endSequence = BigInt(pinned.sequence) + BigInt(writes.length);
        const [otherViewer, delayed] = await Promise.all([
            catchUp(first, subscriptions[0], position[0], sequence, endSequence),
            catchUp(second, subscriptions[1], pinned.nextOffset, pinned.sequence, endSequence),
        ]);
        assert.ok(otherViewer.rows.length >= 1000);
        assert.deepEqual(delayed.rows.map(row => [row.id, row.name]), keepEdit ? [['2', `${label} final`]] : []);
        assert.deepEqual(delayed.removed, []);
        assert.equal(delayed.deltas, 1);
        t.diagnostic(`After a pinned ${pinned.chunks}-chunk delta, ${writes.length} writes coalesced to ` +
            `${delayed.rows.length} changed IDs, no removals and ${delayed.bytes} payload bytes.`);
        sequence = endSequence;
    }
    await recordMemory(t, fixture, 'Two live saved subscriptions across the bursts and pinned transfers');
});
