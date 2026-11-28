const assert = require('node:assert/strict');
const { test } = require('node:test');
const { assertRequest, click, coreDesktop, rosterColumnX, expectLoggedOnce, fixture, replaceText, selectConfigTab,
    settleCanvas, setupWorkspace, status, verticalRuns } = require('./workspace_fixture.cjs');

const workspace = setupWorkspace();
const management = ['status', 'list-users', 'get-user', 'create-user', 'update-user', 'change-user-password'];
const appliedMessage = 'Applied to the running core. WaterWall saves it on its normal schedule.';

async function options(page, y = 226, item = 0) {
    await click(page, rosterColumnX.options, y);
    await click(page, rosterColumnX.options + 40, y + 18 + 23 * item);
}

async function passwordAndIdRow(page) {
    // A notice above the editor can move the two rows of account inputs.
    const runs = await verticalRuns(page, 625, 215, 490, [13, 11, 14], 14);
    const fields = runs.filter(([top, bottom]) => bottom - top <= 26);
    assert.equal(fields.length, 2, 'The account form shows a name row and a password/ID row.');
    const [top, bottom] = fields[1];
    return (top + bottom) / 2;
}

async function liveWorkspace(page, operations = management, schematic = workspace.waterwallSchematic) {
    const config = { name: 'Live management fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    const configId = 31;
    const run = { config_id: configId, node: 'staff', source: 'live', run_id: 'staff-run-1' };
    const data = fixture('running');
    data.server.core.configs = [{ id: configId, name: config.name, enabled: true, active: true,
        auth_nodes: ['staff'], users_access: { staff: { source: run.source, run_id: run.run_id, reason: '',
            database_generation: 'staff-database', operations } } }];
    const user = { id: '18446744073709551613', name: 'Live account', enabled: true, gid: '0',
        stats: { traffic: { up: '0', down: '1' } } };
    const preview = await workspace.mockPreview(page, data, Promise.resolve(), schematic);
    let index = 0;
    let subscription;
    let sequence = 1;
    const delta = (rows, { total = 1, removed = [] } = {}) => {
        const base_sequence = String(sequence++);
        return preview.roster(subscription, rows, { sequence: String(sequence), base_sequence, removed, total });
    };
    const streamState = (state, message = '', cause) =>
        preview.rosterFrame(subscription, { event: 'state', state, message, cause });
    const next = async (action, fields) => {
        const request = await preview.action(index++);
        assert.equal(request.request.action, action);
        if (fields) assertRequest(request.request, { action, ...fields });
        return request;
    };
    await page.goto(workspace.url);
    await page.getByRole('link', { name: config.name, exact: true }).focus();
    await page.keyboard.press('Enter');
    const nodes = await next('get_core_config', { id: configId });
    nodes.reply({ result: { config, revision: 'fixture', editable: false } });
    await selectConfigTab(page, 'staff');
    if (operations.includes('list-users')) {
        const { subscription: id, ...target } = await preview.rosterAction(0);
        assert.deepEqual(target, { action: 'users_roster_subscribe', ...run, database_generation: 'staff-database' });
        subscription = id;
        await preview.roster(subscription, [user]);
        await status(page, /staff tab\. AuthenticationServer users\..*1 users\./);
    } else if (operations.includes('create-user')) {
        await status(page, /staff tab\. AuthenticationServer users\..*create accounts/);
    } else {
        await status(page, /Exact-ID lookup/);
    }
    return { next, run, user, delta, streamState, preview, data, config, subscription, actions: preview.actions };
}

test('uses fresh account details for inline enable and preserves one ID after an unknown create outcome', async () => {
    const token = '31'.repeat(24);
    const nextToken = '32'.repeat(24);
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, delta, actions } = await liveWorkspace(page);
        await click(page, rosterColumnX.enable, 226);
        const check = await next('get_live_config_user', { ...run, user_id: user.id });
        check.reply({ result: { user: { ...user, enabled: false }, if_match: token } });
        await status(page, /The enabled state or access changed/);
        assert.equal(actions.filter(({ request }) => request.action === 'edit_live_config_user').length, 0);

        await click(page, rosterColumnX.enable, 226);
        const matched = await next('get_live_config_user', { ...run, user_id: user.id });
        matched.reply({ result: { user, if_match: nextToken } });
        const write = await next('edit_live_config_user', {
            ...run, user_id: user.id, if_match: nextToken, user: { enabled: false },
        });
        write.reply({ result: { outcome: 'applied', user_id: user.id } });
        await delta([{ ...user, enabled: false }]);
        await expectLoggedOnce(page, appliedMessage);

        await click(page, 240, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'throwaway-live-password';
        await replaceText(page, 600, 317, password);
        await click(page, 245, 747);
        const created = await next('create_live_config_user');
        const assigned = '18446744073709551614';
        assertRequest(created.request, { action: 'create_live_config_user', ...run, user_id: assigned,
            user: { id: assigned, password, enabled: true } });
        created.reply({ ok: false, result: { outcome: 'unknown', user_id: assigned } });
        await status(page, new RegExp(`Outcome unknown for account ${assigned}`));
        const reconcile = await next('get_live_config_user', { ...run, user_id: assigned });
        reconcile.reply({ result: { user: { id: assigned, enabled: true }, if_match: token } });
        await status(page, new RegExp(`Account ${assigned} exists`));
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);
        assert.ok(!(await page.locator('#canvas').getAttribute('aria-label')).includes(password));
    } finally {
        await page.close();
    }
});

test('keeps the original sequential ID on a deliberate retry even after the roster advances', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const schematic = structuredClone(workspace.waterwallSchematic);
        Object.assign(schematic.ui.users.fields.id, { minimum: 7, maximum: 10 });
        const { next, run, delta, actions } = await liveWorkspace(page, management, schematic);
        await delta([{ id: '8', enabled: true }], { total: 2 });
        await click(page, 240, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-range-password';
        await replaceText(page, 600, 317, password);
        await click(page, 245, 747);
        const create = await next('create_live_config_user');
        const id = '9';
        assert.equal(create.request.user_id, id);
        assert.equal(create.request.user.id, id);
        create.reply({ ok: false, result: { outcome: 'unknown', user_id: id } });
        const lookup = await next('get_live_config_user', { ...run, user_id: id });
        await delta([{ id: '10', enabled: true }], { total: 3 });
        lookup.reply({ ok: false, result: { code: 'not_found' } });
        await status(page, new RegExp(`Account ${id} could not be found`));
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);

        const [review] = await verticalRuns(page, 206, 165, 287, [69, 64, 79], 6);
        assert.ok(review, 'The operator can review the absent account and keep its assigned ID.');
        await click(page, 280, (review[0] + review[1]) / 2);
        await replaceText(page, 600, 317, password);
        await click(page, 245, 747);
        const retry = await next('create_live_config_user', {
            ...run, user_id: id, user: { id, password, enabled: true },
        });
        retry.reply({ result: { outcome: 'applied', user_id: id } });
        await status(page, `Latest log: WaterWall confirmed creation of account ${id} in config ${run.config_id}`);
    } finally {
        await page.close();
    }
});

test('starts at one and proposes the refreshed next ID after a concurrent create takes it', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, delta, streamState, actions, preview, subscription } = await liveWorkspace(page);
        await delta([], { total: 0, removed: [user.id] });
        await status(page, /0 users/);
        await click(page, 240, 137);
        await status(page, /Add user\. Account section\. Next ID 1\./);
        const password = 'fixture-concurrent-create';
        await replaceText(page, 600, await passwordAndIdRow(page), password);
        await click(page, 245, 747);
        const create = await next('create_live_config_user', {
            ...run, user_id: '1', user: { id: '1', password, enabled: true },
        });
        create.reply({ ok: false, feedback: 'An account with that ID already exists.',
            result: { outcome: 'rejected', code: 'id_exists', user_id: '1' } });
        const refresh = await preview.rosterAction(1);
        assert.equal(refresh.action, 'users_roster_refresh');
        assert.equal(refresh.subscription, subscription);
        // A concurrent create can reach WaterWall before the roster stream catches up.
        await streamState('ready');
        await status(page, /Next ID 2\./);
        await delta([{ id: '1', enabled: true }, { id: '5', enabled: true }], { total: 2 });
        await status(page, /Add user\. Account section\. Next ID 6\./);
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);
        await replaceText(page, 600, await passwordAndIdRow(page), password);
        await click(page, 245, 747);
        const retry = await next('create_live_config_user', {
            ...run, user_id: '6', user: { id: '6', password, enabled: true },
        });
        retry.reply({ result: { outcome: 'applied', user_id: '6' } });
        await status(page, /Latest log: WaterWall confirmed creation of account 6 in config 31/);
        await click(page, 240, 137);
        await status(page, /Add user\. Account section\. Next ID 7\./);
    } finally {
        await page.close();
    }
});

test('requires an explicit unused ID when the live sequence reaches the upper bound', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, delta, actions } = await liveWorkspace(page);
        await delta([{ ...user, id: '18446744073709551615' }], { removed: [user.id] });
        await click(page, 240, 137);
        await status(page, /Add user\. Account section\. No higher account ID/);
        const password = 'fixture-end-of-sequence';
        await replaceText(page, 600, 317, password);
        await click(page, 245, 747);
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 0);
        await replaceText(page, 820, await passwordAndIdRow(page), '42');
        await click(page, 245, 747);
        const create = await next('create_live_config_user', {
            ...run, user_id: '42', user: { id: '42', password, enabled: true },
        });
        create.reply({ result: { outcome: 'applied', user_id: '42' } });
        await status(page, /Latest log: WaterWall confirmed creation of account 42 in config 31/);
    } finally {
        await page.close();
    }
});

test('retains an exact quota draft and original edit token through roster changes and conflict review', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, delta, streamState, actions } = await liveWorkspace(page);
        const original = '41'.repeat(24);
        const newToken = '42'.repeat(24);
        const account = { ...user, limit: { traffic: { up: '2000000000', down: '9875654321' } } };
        const current = { ...account, limit: { traffic: { ...account.limit.traffic, up: '1000000001' } } };
        const patch = { limit: { traffic: { up: '1000000000' } } };
        await options(page);
        const details = await next('get_live_config_user', { ...run, user_id: user.id });
        details.reply({ result: { user: account, if_match: original } });
        await status(page, /Edit user\. Account section\./);
        await click(page, 370, 166);
        await status(page, /Edit user\. Limits section\./);
        await replaceText(page, 300, 298, '1.000');
        await delta([{ ...user, stats: { traffic: { up: '987', down: '654' } } }]);
        await streamState('stale', 'The list listener is unavailable.');
        await status(page, /list listener is unavailable.*Showing stale accounts/);
        await click(page, 234, 747);
        const save = await next('edit_live_config_user', {
            ...run, user_id: user.id, if_match: original, user: patch,
        });
        save.reply({ ok: false, result: { outcome: 'conflict', user_id: user.id } });
        const reconcile = await next('get_live_config_user', { ...run, user_id: user.id });
        reconcile.reply({ result: { user: current, if_match: newToken } });
        await streamState('ready');
        await status(page, /Review current account details before saving\. Upload traffic \(GB\) changed\./);
        await click(page, 234, 747);
        assert.equal(actions.filter(({ request }) => request.action === 'edit_live_config_user').length, 1);
        await click(page, 337, 236);
        await click(page, 234, 747);
        const resubmitted = await next('edit_live_config_user', {
            ...run, user_id: user.id, if_match: newToken, user: patch,
        });
        resubmitted.reply({ result: { outcome: 'applied', user_id: user.id } });
        await expectLoggedOnce(page, appliedMessage);
        await status(page, new RegExp(`Selected account ${user.id} is enabled`));

        await options(page);
        const obsoleteDetail = await next('get_live_config_user', { ...run, user_id: user.id });
        await delta([], { total: 0, removed: [user.id] });
        await status(page, /0 users.*No users\./);
        obsoleteDetail.reply({ result: { user: account, if_match: original } });
        await page.waitForFunction(() => !document.querySelector('#canvas').ariaLabel.includes('Loading account details.'));
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Edit user|Selected account/);
    } finally {
        await page.close();
    }
});

test('creates during a listing outage while the stale row cannot toggle', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, delta, streamState, actions, preview } = await liveWorkspace(page);
        await streamState('stale', 'The list listener is unavailable.');
        await status(page, /list listener is unavailable.*Showing stale accounts/);
        await click(page, rosterColumnX.enable, 282);
        assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user' ||
            request.action === 'edit_live_config_user').length, 0);

        await click(page, 240, 160);
        await status(page, /Add user\. Account section\./);
        const password = 'temporary-outage-password';
        await replaceText(page, 600, 373, password);
        await status(page, /Wait for the account list to refresh, or enter an unused ID/);
        await click(page, 245, 747);
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 0,
            'A stale list cannot supply the next ID.');
        const assigned = '42';
        await replaceText(page, 820, await passwordAndIdRow(page), assigned);
        await click(page, 245, 747);
        const create = await next('create_live_config_user');
        assertRequest(create.request, { action: 'create_live_config_user', ...run, user_id: assigned,
            user: { id: assigned, password, enabled: true } });
        create.reply({ result: { outcome: 'applied', user_id: assigned } });
        await delta([{ id: assigned, enabled: true }], { total: 2 });
        await status(page, `Latest log: WaterWall confirmed creation of account ${assigned} in config ${run.config_id}`);
        await status(page, `Selected account ${assigned} is enabled`);
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);

        await click(page, 240, 137);
        await status(page, /Add user\. Account section\./);
        await replaceText(page, 600, 317, password);
        await preview.disconnect();
        await status(page, /Panel connection unavailable\./);
        await click(page, 245, 747);
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);
    } finally {
        await page.close();
    }
});

for (const duringLoss of [false, true]) {
    test(`opens exact account details with password access ${duringLoss ? 'across' : 'after'} a list denial`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { next, run, user, streamState, preview, actions } = await liveWorkspace(page);
            await streamState(duringLoss ? 'stale' : 'lost', 'The list listener is unavailable.',
                duringLoss ? undefined : 'permission');
            await status(page, /The list listener is unavailable\./);
            await replaceText(page, 310, 193, `  00${user.id}  `);
            await page.keyboard.press('Enter');
            const lookup = await next('get_live_config_user', { ...run, user_id: user.id });
            if (duringLoss) {
                await streamState('lost', 'Listing was denied.', 'permission');
                await status(page, /Listing was denied\./);
            }
            const wall = 1795000000000n;
            const accounting = 1794000000000n;
            const day = 86400000n;
            const current = { ...user, enabled: false, time: {
                'expire-at-ms': duringLoss ? '0' : String(wall + day),
                'expire-after-first-usage-ms': duringLoss ? String(day) : '0',
                'first-usage-at-ms': duringLoss ? String(accounting) : '0',
            } };
            const token = '57'.repeat(24);
            lookup.reply({ result: { user: current, if_match: token,
                host_time_ms: String(wall), accounting_time_ms: String(accounting) } });
            await status(page, new RegExp(`Edit user\\. Account section\\..*Account ${user.id} is disabled\\..*Days Left 1 day`));
            assert.equal(preview.rosterActions.length, 2);
            await click(page, 428, 747);
            await status(page, /Change password\./);
            const password = 'fixture-lookup-password';
            await replaceText(page, 300, 278, password);
            await click(page, 266, 306);
            const write = await next('change_live_config_user_password', { ...run, user_id: user.id, if_match: token, password });
            write.reply({ result: { outcome: 'applied', user_id: user.id } });
            await expectLoggedOnce(page, appliedMessage);
            assert.equal(actions.filter(entry => entry.request.action === 'get_live_config_user').length, 1);
        } finally {
            await page.close();
        }
    });
}

test('opens a read-only detail with its exact result clock and clears the account on close or a missing lookup', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, actions, preview } = await liveWorkspace(page, ['get-user']);
        const find = async () => {
            await replaceText(page, 310, 160, '  00042  ');
            await page.keyboard.press('Enter');
            return next('get_live_config_user', { ...run, user_id: '42' });
        };
        const lookup = await find();
        const wall = 1795000000000n;
        const accounting = 1794000000000n;
        const day = 86400000n;
        const detail = { ...user, id: '42', time: { 'expire-at-ms': String(wall + day),
            'expire-after-first-usage-ms': String(day), 'first-usage-at-ms': String(accounting) } };
        lookup.reply({ result: { user: detail, if_match: '59'.repeat(24),
            host_time_ms: String(wall), accounting_time_ms: String(accounting) } });
        await status(page, /Account details\. Account section\..*Account 42 .*Days Left 1 day/);
        await click(page, 224, 747);
        const repeat = await find();
        repeat.reply({ result: { user: detail, if_match: '60'.repeat(24), host_time_ms: String(wall) } });
        await status(page, /Account details\. Account section\..*Account 42 .*Days Left Unavailable/);
        await click(page, 224, 747);
        const missing = await find();
        missing.reply({ ok: false, result: { code: 'not_found' } });
        await status(page, /Account 42 could not be found\./);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Selected account|Account section/);
        assert.equal(preview.rosterActions.length, 0);
        assert.deepEqual(actions.slice(1).map(entry => entry.request.action), Array(3).fill('get_live_config_user'));
    } finally {
        await page.close();
    }
});

test('changes a password from a looked-up account with only the detail and password grants', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, actions } = await liveWorkspace(page, ['get-user', 'change-user-password']);
        await replaceText(page, 310, 160, user.id);
        await page.keyboard.press('Enter');
        const lookup = await next('get_live_config_user', { ...run, user_id: user.id });
        const token = '63'.repeat(24);
        lookup.reply({ result: { user, if_match: token } });
        await status(page, /Account details\. Account section/);
        await click(page, 310, 747);
        await status(page, /Change password\./);
        const password = 'fixture-password-only';
        await replaceText(page, 300, 245, password);
        await click(page, 266, 272);
        const write = await next('change_live_config_user_password', { ...run, user_id: user.id, if_match: token, password });
        write.reply({ result: { outcome: 'applied', user_id: user.id } });
        await expectLoggedOnce(page, appliedMessage);
        assert.deepEqual(actions.map(({ request }) => request.action), [
            'get_core_config', 'get_live_config_user', 'change_live_config_user_password',
        ]);
    } finally {
        await page.close();
    }
});

test('closing explicit details releases the account before the next roster write', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, actions, streamState, delta } = await liveWorkspace(page);
        await streamState('stale', 'The list listener is unavailable.');
        await status(page, /The list listener is unavailable\./);
        await replaceText(page, 310, 193, user.id);
        await page.keyboard.press('Enter');
        const opened = await next('get_live_config_user', { ...run, user_id: user.id });
        opened.reply({ result: { user, if_match: '61'.repeat(24) } });
        await status(page, /Edit user\. Account section/);
        await streamState('ready');
        const neighbor = { ...user, id: '61', name: 'Neighbor' };
        await delta([neighbor], { total: 2 });
        await click(page, 312, 747);
        await status(page, /Live · 2 users/);
        await click(page, rosterColumnX.enable, 264);
        const check = await next('get_live_config_user', { ...run, user_id: user.id });
        check.reply({ result: { user, if_match: '62'.repeat(24) } });
        const write = await next('edit_live_config_user', {
            ...run, user_id: user.id, if_match: '62'.repeat(24), user: { enabled: false },
        });
        await click(page, 320, 226);
        await status(page, /Selected account 61 is enabled/);
        write.reply({ result: { outcome: 'applied', user_id: user.id } });
        await delta([{ ...user, enabled: false }], { total: 2 });
        await expectLoggedOnce(page, appliedMessage);
        await status(page, /Selected account 61 is enabled/);
        assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length, 2);
    } finally {
        await page.close();
    }
});

for (const afterWrite of [false, true]) {
    test(`clears a toggled account removed ${afterWrite ? 'after dispatch' : 'before its detail reply'}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { next, run, user, delta, actions } = await liveWorkspace(page);
            const neighbor = { ...user, id: '61', name: 'Unaffected neighbor' };
            if (afterWrite) {
                await delta([neighbor], { total: 2 });
                await status(page, /Live · 2 users/);
            }
            await click(page, rosterColumnX.enable, afterWrite ? 264 : 226);
            const detail = await next('get_live_config_user', { ...run, user_id: user.id });
            await status(page, new RegExp(`Selected account ${user.id}`));
            const token = '60'.repeat(24);
            let write;
            if (afterWrite) {
                detail.reply({ result: { user, if_match: token } });
                write = await next('edit_live_config_user', {
                    ...run, user_id: user.id, if_match: token, user: { enabled: false },
                });
            }
            await delta([], { total: afterWrite ? 1 : 0, removed: [user.id] });
            await status(page, afterWrite ? /Live · 1 users/ : /No users\./);
            if (write) {
                write.reply({ result: { outcome: 'applied', user_id: user.id } });
                await expectLoggedOnce(page, appliedMessage);
            } else {
                detail.reply({ result: { user, if_match: token } });
                await settleCanvas(page);
            }
            assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Selected account/);
            assert.equal(actions.filter(entry => entry.request.action === 'edit_live_config_user').length,
                afterWrite ? 1 : 0);
            if (afterWrite) {
                await click(page, 320, 226);
                await status(page, /Selected account 61 is enabled/);
            }
        } finally {
            await page.close();
        }
    });
}

test('keeps account options bound to a reordered row and prevents saving a removed draft', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, delta, actions } = await liveWorkspace(page);
        const neighbor = { id: '61', name: 'Neighbor', enabled: true, stats: { traffic: { up: '0', down: '100' } } };
        await delta([neighbor], { total: 2 });
        await status(page, /Live · 2 users/);
        await click(page, rosterColumnX.trafficUsage, 193);
        await status(page, /Sorted by Traffic usage ascending/);

        await click(page, rosterColumnX.options, 226);
        await status(page, new RegExp(`Selected account ${user.id} is enabled`));
        await delta([{ ...user, stats: { traffic: { up: '300', down: '700' } } }], { total: 2 });
        await status(page, new RegExp(`Selected account ${user.id} is enabled\\..*Traffic usage 1000 B`));
        await click(page, rosterColumnX.options + 40, 244);
        const details = await next('get_live_config_user', { ...run, user_id: user.id });
        details.reply({ result: { user, if_match: '59'.repeat(24) } });
        await status(page, /Edit user\. Account section\./);
        await replaceText(page, 405, 245, 'Unsaved draft');
        await delta([], { total: 1, removed: [user.id] });
        await status(page, /This account was removed\. Its draft can no longer be saved\./);
        await click(page, 234, 747);
        assert.equal(actions.filter(({ request }) => request.action === 'edit_live_config_user').length, 0);
        await status(page, /Edit user\. Account section\./);
    } finally {
        await page.close();
    }
});

for (const operations of [management, ['create-user', 'get-user'], ['create-user']]) {
    const canList = operations.includes('list-users');
    const canLookup = operations.includes('get-user');
    const access = canList ? 'a roster creation' : canLookup ? 'a create and lookup grant' : 'a create-only grant';
    test(`shows the created account ID after ${access}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const schematic = structuredClone(workspace.waterwallSchematic);
            if (!canLookup) {
                Object.assign(schematic.ui.users.fields.id, { minimum: 42, maximum: 42 });
            }
            const { next, run, delta, actions, preview } = await liveWorkspace(page, operations, schematic);
            await click(page, 240, canList ? 137 : 160);
            await status(page, /Add user\. Account section\./);
            const password = 'fixture-create-password';
            const account = { id: '42', name: 'Created account', enabled: true };
            const y = canList ? 317 : 340;
            await replaceText(page, 600, y, password);
            if (canLookup) await replaceText(page, 820, y, account.id);
            await click(page, 245, 747);
            const create = await next('create_live_config_user', { ...run, user_id: account.id,
                user: { id: account.id, password, enabled: true } });
            create.reply({ result: { outcome: 'applied', user_id: account.id } });
            assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length, 0,
                'Acknowledging a create does not trigger a detail read.');
            await status(page, `Latest log: WaterWall confirmed creation of account ${account.id} in config ${run.config_id}`);
            if (canList) {
                await delta([account], { total: 2 });
                await status(page, /Selected account 42 is enabled\./);
                return;
            }
            await status(page, /Selected account 42\./);
            assert.equal(preview.rosterActions.length, 0, 'Creating without list permission does not read the roster.');
            if (!canLookup) {
                assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Exact-ID lookup/);
                return;
            }
            await status(page, /Exact-ID lookup/);
            await replaceText(page, 400, 160, account.id);
            await page.keyboard.press('Enter');
            const find = await next('get_live_config_user', { ...run, user_id: account.id });
            find.reply({ result: { user: account, if_match: '77'.repeat(24) } });
            await status(page, /Account details\. Account section\..*Account 42 is enabled\./);
        } finally {
            await page.close();
        }
    });
}

test('reconciles an uncertain password change without claiming to verify the credential', async () => {
    const token = '51'.repeat(24);
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, actions } = await liveWorkspace(page);
        await options(page, 226, 1);
        const details = await next('get_live_config_user', { ...run, user_id: user.id });
        details.reply({ result: { user, if_match: token } });
        await status(page, /Change password\./);
        const password = 'disposable-password-change';
        await replaceText(page, 300, 222, password);
        await click(page, 266, 255);
        const change = await next('change_live_config_user_password', { ...run, user_id: user.id,
            if_match: token, password });
        change.reply({ ok: false, result: { outcome: 'unknown', user_id: user.id } });
        const lookup = await next('get_live_config_user', { ...run, user_id: user.id });
        lookup.reply({ result: { user, if_match: '52'.repeat(24) } });
        await status(page, /Live details cannot verify the password\. Verify the credential separately\./);
        assert.equal(actions.filter(({ request }) => request.action === 'change_live_config_user_password').length, 1);
        assert.ok(!(await page.locator('#canvas').getAttribute('aria-label')).includes(password));
    } finally {
        await page.close();
    }
});

test('retains a lost create receipt through reconnect and remap only for the original sign-in', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, config, data, preview, actions } =
            await liveWorkspace(page, ['create-user', 'get-user']);
        await click(page, 240, 160);
        await status(page, /Add user\. Account section\./);
        const password = 'temporary-receipt-passphrase';
        await replaceText(page, 600, 340, password);
        await click(page, 245, 747);
        const create = await next('create_live_config_user');
        const id = create.request.user_id;
        assert.match(id, /^[1-9][0-9]*$/);

        const reconnect = async sessionId => {
            await preview.disconnectBoth(sessionId);
            await status(page, /Checking the session/);
            await page.getByRole('link', { name: config.name, exact: true }).focus();
            await page.keyboard.press('Enter');
            await selectConfigTab(page, 'staff');
        };
        let index = actions.indexOf(create) + 1;
        const afterReconnect = async (action, fields) => {
            let pending = await preview.action(index++);
            if (pending.request.action === 'get_core_config') {
                pending.reply({ result: { config, revision: 'fixture', editable: false } });
                pending = await preview.action(index++);
            }
            assertRequest(pending.request, { action, ...fields });
            return pending;
        };
        await reconnect();
        const lookup = await afterReconnect('get_live_config_user', { ...run, user_id: id });
        await status(page, new RegExp(`Outcome unknown for account ${id}`));
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);

        const offline = { source: 'offline', run_id: 'staff-stopped-replacement' };
        data.server.core.state = 'stopped';
        data.server.core.configs[0].active = false;
        data.server.core.configs[0].users_access.staff = { ...offline, database_generation: 'replacement-db' };
        await preview.broadcast();
        const savedList = async rosterIndex => {
            const { subscription, ...target } = await preview.rosterAction(rosterIndex);
            assert.deepEqual(target, { action: 'users_roster_subscribe', config_id: run.config_id, node: run.node,
                ...offline, database_generation: 'replacement-db' });
            await preview.roster(subscription, []);
        };
        await savedList(0);
        lookup.reply({ result: { user: { id, name: 'Obsolete account' }, if_match: '31'.repeat(24) } });
        await status(page, new RegExp(`Check the original database.*Outcome unknown for account ${id}`));
        const originalTab = await page.locator('#canvas').getAttribute('aria-label');
        assert.ok(!originalTab.includes(password) && !originalTab.includes('Obsolete account'));
        assert.equal(actions.filter(({ request }) => request.action === 'get_config_users').length, 0);

        await reconnect('different-sign-in-for-same-username');
        await savedList(1);
        await status(page, /No users\./);
        assert.ok(!(await page.locator('#canvas').getAttribute('aria-label')).includes(id));
        assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length, 1);
    } finally {
        await page.close();
    }
});

for (const acknowledged of [true, false]) {
    const outcome = acknowledged ? 'confirmed' : 'unconfirmed';
    test(`keeps an accurately labelled ${outcome} create receipt when its node disappears`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { next, data, preview } = await liveWorkspace(page, ['create-user']);
            await click(page, 240, 160);
            await status(page, /Add user\. Account section\./);
            await replaceText(page, 600, 340, 'temporary-removed-node-password');
            await click(page, 245, 747);
            const create = await next('create_live_config_user');
            const id = create.request.user_id;
            data.server.core.configs[0].auth_nodes = [];
            data.server.core.configs[0].users_access = {};
            if (acknowledged) {
                create.reply({ result: { outcome: 'applied', user_id: id } });
            } else {
                await preview.broadcast();
            }
            const label = acknowledged ? 'WaterWall confirmed creation of' : 'Outcome unknown for';
            await status(page, new RegExp(`Latest log: ${label} account ${id} in config 31`));
            const description = await page.locator('#canvas').getAttribute('aria-label');
            assert.ok(description.includes('(node staff)') && !description.includes('temporary-removed-node-password'));
            assert.equal(description.split(id).length - 1, 1);
        } finally {
            await page.close();
        }
    });
}

test('reviews open edit details once after list permission loss and saves the intact draft', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, streamState, actions } = await liveWorkspace(page);
        await options(page);
        const opened = await next('get_live_config_user', { ...run, user_id: user.id });
        opened.reply({ result: { user, if_match: '81'.repeat(24) } });
        await status(page, /Edit user\. Account section\./);
        await replaceText(page, 405, 245, 'Draft survives list loss');

        await streamState('lost', 'Listing was denied.', 'permission');
        const review = await next('get_live_config_user', { ...run, user_id: user.id });
        const freshToken = '82'.repeat(24);
        review.reply({ result: { user: { ...user, name: 'Updated by another operator' }, if_match: freshToken } });
        await status(page, /Listing was denied\..*Review current account details before saving\. Name changed\./);
        const [reviewButton] = await verticalRuns(page, 206, 233, 330, [69, 64, 79], 6);
        assert.ok(reviewButton, 'Review is available after independent detail access succeeds.');
        await click(page, 332, (reviewButton[0] + reviewButton[1]) / 2);
        await page.waitForFunction(() => !document.querySelector('#canvas').ariaLabel.includes(
            'Review current account details before saving.'));
        await click(page, 234, 747);
        const saved = await next('edit_live_config_user', {
            ...run, user_id: user.id, if_match: freshToken, user: { name: 'Draft survives list loss' },
        });
        saved.reply({ result: { outcome: 'applied', user_id: user.id } });
        await expectLoggedOnce(page, appliedMessage);
        assert.deepEqual(actions.map(({ request }) => request.action), [
            'get_core_config', 'get_live_config_user', 'get_live_config_user', 'edit_live_config_user',
        ]);
    } finally {
        await page.close();
    }
});

test('updates the open password form token after list denial and waits for an explicit write', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, streamState, actions } = await liveWorkspace(page);
        await options(page, 226, 1);
        const opened = await next('get_live_config_user', { ...run, user_id: user.id });
        opened.reply({ result: { user, if_match: '83'.repeat(24) } });
        await status(page, /Change password\./);
        const password = 'fixture-explicit-password';
        await replaceText(page, 300, 222, password);

        await streamState('lost', 'Listing was denied.', 'permission');
        const review = await next('get_live_config_user', { ...run, user_id: user.id });
        const freshToken = '84'.repeat(24);
        review.reply({ result: { user, if_match: freshToken } });
        await page.waitForFunction(() => {
            const description = document.querySelector('#canvas').ariaLabel;
            return description.includes('Listing was denied.') && !description.includes('before saving.');
        });
        assert.equal(actions.filter(({ request }) => request.action === 'change_live_config_user_password').length, 0);
        await click(page, 266, 306);
        const saved = await next('change_live_config_user_password', {
            ...run, user_id: user.id, if_match: freshToken, password,
        });
        saved.reply({ result: { outcome: 'applied', user_id: user.id } });
        await expectLoggedOnce(page, appliedMessage);
        assert.deepEqual(actions.map(({ request }) => request.action), [
            'get_core_config', 'get_live_config_user', 'get_live_config_user', 'change_live_config_user_password',
        ]);
    } finally {
        await page.close();
    }
});

for (const failure of ['detail rejection', 'write grant loss', 'unverified target', 'replacement binding']) {
    test(`blocks an open draft after list loss and ${failure}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { next, run, user, streamState, preview, data, actions } = await liveWorkspace(page);
            await options(page);
            const opened = await next('get_live_config_user', { ...run, user_id: user.id });
            opened.reply({ result: { user, if_match: '85'.repeat(24) } });
            await status(page, /Edit user\. Account section\./);
            await replaceText(page, 405, 245, 'Never send this existing draft');
            const access = data.server.core.configs[0].users_access.staff;
            if (failure === 'write grant loss') {
                access.operations = management.filter(operation => operation !== 'update-user');
                await preview.broadcast();
            }
            const unverifiedTarget = failure === 'unverified target';
            await streamState('lost', 'Listing is unavailable.', unverifiedTarget ? 'identity' : 'permission');
            await status(page, /Listing is unavailable\./);
            if (unverifiedTarget) {
                await status(page, /The account source could not be verified\. Reopen the account/);
            } else if (failure !== 'write grant loss') {
                const review = await next('get_live_config_user', { ...run, user_id: user.id });
                if (failure === 'detail rejection') {
                    review.reply({ ok: false, result: { code: 'not_found' } });
                    await status(page, /This account was removed.*could not be found/);
                } else {
                    access.run_id = 'new-live-run';
                    access.database_generation = 'new-live-database';
                    await preview.broadcast();
                    await status(page, /node's database changed/);
                    review.reply({ result: { user, if_match: '86'.repeat(24) } });
                }
            } else {
                await status(page, /Access to save this account is unavailable/);
            }
            await click(page, 234, 747);
            assert.equal(actions.filter(({ request }) => request.action === 'edit_live_config_user').length, 0);
            if (failure === 'write grant loss' || unverifiedTarget) {
                assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length, 1);
            }
        } finally {
            await page.close();
        }
    });
}

test('does not re-arm an uncertain password write when only list access is lost', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { next, run, user, streamState, actions } = await liveWorkspace(page);
        await options(page, 226, 1);
        const opened = await next('get_live_config_user', { ...run, user_id: user.id });
        const token = '87'.repeat(24);
        opened.reply({ result: { user, if_match: token } });
        await status(page, /Change password\./);
        const password = 'fixture-password-uncertain';
        await replaceText(page, 300, 222, password);
        await click(page, 266, 255);
        const change = await next('change_live_config_user_password', { ...run, user_id: user.id, if_match: token,
            password });
        change.reply({ ok: false, result: { outcome: 'unknown', user_id: user.id } });
        const obsolete = await next('get_live_config_user', { ...run, user_id: user.id });
        await streamState('lost', 'Listing was denied.', 'permission');
        const current = await next('get_live_config_user', { ...run, user_id: user.id });
        obsolete.reply({ result: { user, if_match: '88'.repeat(24) } });
        current.reply({ result: { user, if_match: '89'.repeat(24) } });
        await status(page, /Live details cannot verify the password.*Outcome unknown/);
        await click(page, 266, 306);
        assert.equal(actions.filter(({ request }) => request.action === 'change_live_config_user_password').length, 1);
        assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length, 3);
    } finally {
        await page.close();
    }
});

test('recovers live listing with one fresh subscribe and ignores deliveries to lost IDs', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { run, user, streamState, preview, subscription } = await liveWorkspace(page);
        await streamState('lost', 'Listing was denied.', 'permission');
        await status(page, /Listing was denied\./);
        assert.deepEqual(await preview.rosterAction(1), { action: 'users_roster_unsubscribe', subscription });
        await streamState('lost', 'Late loss frame.', 'permission');
        await preview.roster(subscription, [{ ...user, name: 'Obsolete name' }], { sequence: '2' });
        await status(page, /Listing was denied\./);
        await click(page, 726, 160);
        const denied = await preview.rosterAction(2);
        assert.deepEqual(denied, { action: 'users_roster_subscribe', ...run,
            database_generation: 'staff-database', subscription: denied.subscription });
        assert.notEqual(denied.subscription, subscription);
        await preview.rosterFrame(denied.subscription, { event: 'state', state: 'lost',
            message: 'Listing was denied again.', cause: 'permission' });
        await status(page, /Listing was denied again\./);
        assert.deepEqual(await preview.rosterAction(3),
            { action: 'users_roster_unsubscribe', subscription: denied.subscription });

        await click(page, 726, 160);
        const recovered = await preview.rosterAction(4);
        assert.deepEqual(recovered, { action: 'users_roster_subscribe', ...run,
            database_generation: 'staff-database', subscription: recovered.subscription });
        assert.notEqual(recovered.subscription, denied.subscription);
        await preview.roster(denied.subscription, [user, { id: '5', enabled: true }]);
        await preview.roster(recovered.subscription, [user]);
        await status(page, /Live · 1 users\./);
        assert.equal(preview.rosterActions.length, 5);
    } finally {
        await page.close();
    }
});

for (const timing of ['before an unknown reply', 'after an absent result', 'before a confirmed reply', 'after stopping WaterWall']) {
    test(`retires a create draft when its database identity is lost ${timing}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { next, run, config, data, actions, preview, streamState } = await liveWorkspace(page);
            await click(page, 240, 137);
            await status(page, /Add user\. Account section/);
            const password = 'temporary-create-identity-password';
            await replaceText(page, 600, 317, password);
            await click(page, 245, 747);
            const create = await next('create_live_config_user');
            const id = create.request.user_id;
            const unknown = { ok: false, result: { outcome: 'unknown', user_id: id } };
            const stopped = timing === 'after stopping WaterWall';
            const alreadyLookedUp = timing === 'after an absent result' || stopped;
            if (alreadyLookedUp) {
                create.reply(unknown);
                const lookup = await next('get_live_config_user', { ...run, user_id: id });
                lookup.reply({ ok: false, result: { code: 'not_found' } });
                await status(page, new RegExp(`Account ${id} could not be found`));
            }

            if (stopped) {
                data.server.core.state = 'stopped';
                const entry = data.server.core.configs[0];
                entry.active = false;
                Object.assign(entry.users_access.staff, { source: 'offline', run_id: 'saved-run', operations: [] });
                await preview.broadcast();
                const saved = await preview.rosterAction(2); // live unsubscribe, then saved subscribe
                assert.equal(saved.source, 'offline');
                await preview.rosterFrame(saved.subscription, { event: 'state', state: 'lost', cause: 'identity',
                    message: 'The saved database is missing.' });
            } else {
                await streamState('lost', 'The original database identity was lost.', 'identity');
            }
            await status(page, new RegExp(`Check account ${id} in the original database`));
            if (!alreadyLookedUp) {
                create.reply(timing === 'before a confirmed reply'
                    ? { result: { outcome: 'applied', user_id: id } } : unknown);
            }
            const receipt = timing === 'before a confirmed reply' ? 'WaterWall confirmed creation of' : 'Outcome unknown for';
            await status(page, new RegExp(`Latest log: ${receipt} account ${id} in config 31`));
            const label = await page.locator('#canvas').getAttribute('aria-label');
            assert.doesNotMatch(label, /Add user\. Account section|Loading account details|Selected account/);
            assert.ok(!label.includes(password));
            assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);

            // The same operator can reconnect to the replacement using the same Wave run and binding names.
            if (timing === 'before an unknown reply' || stopped) {
                await preview.disconnectBoth();
                await status(page, /Checking the session/);
                await page.getByRole('link', { name: config.name, exact: true }).focus();
                await page.keyboard.press('Enter');
                await selectConfigTab(page, 'staff');
                await status(page, new RegExp(`Latest log: ${receipt} account ${id} in config 31`));
                await settleCanvas(page);
            }
            assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length,
                alreadyLookedUp ? 1 : 0);
            assert.equal(actions.filter(({ request }) => request.action === 'get_config_user').length, 0);
        } finally {
            await page.close();
        }
    });
}

test('remembers create target loss when both sockets close before the next render', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        // Socket callbacks keep running while a background browser delays the next animation frame.
        await page.addInitScript(() => {
            const requestFrame = window.requestAnimationFrame.bind(window);
            window.requestAnimationFrame = callback => requestFrame(time => {
                if (window.holdWaveFrame) window.resumeWaveFrame = () => requestFrame(callback);
                else callback(time);
            });
        });
        const { next, run, config, actions, preview, streamState, subscription } = await liveWorkspace(page);
        await click(page, 240, 137);
        await status(page, /Add user\. Account section/);
        await replaceText(page, 600, 317, 'temporary-create-disconnect-password');
        await click(page, 245, 747);
        const create = await next('create_live_config_user');

        await page.evaluate(() => { window.holdWaveFrame = true; });
        await page.waitForFunction(() => !!window.resumeWaveFrame, null, { polling: 5 });
        await streamState('lost', 'The original database identity was lost.', 'identity');
        assert.deepEqual(await preview.rosterAction(1), { action: 'users_roster_unsubscribe', subscription });
        await preview.disconnectBoth();
        await page.evaluate(() => { window.holdWaveFrame = false; window.resumeWaveFrame(); });

        await status(page, /Checking the session/);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        await selectConfigTab(page, 'staff');
        await status(page, new RegExp(`Latest log: Outcome unknown for account ${create.request.user_id} in config ${run.config_id}`));
        await settleCanvas(page);
        assert.equal(actions.filter(({ request }) => request.action === 'get_live_config_user').length, 0);
        assert.equal(actions.filter(({ request }) => request.action === 'create_live_config_user').length, 1);
    } finally {
        await page.close();
    }
});
