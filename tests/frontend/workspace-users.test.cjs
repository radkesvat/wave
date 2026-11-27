const assert = require('node:assert/strict');
const { test } = require('node:test');
const {
    setupWorkspace,
    desktop,
    coreDesktop,
    portrait,
    donationSlotHeight,
    timeout,
    fixture,
    status,
    expectLoggedOnce,
    settleCanvas,
    click,
    replaceText,
    assertRequest,
    verticalRuns,
    rectangleContainsColor,
    graphMenuItem,
    selectConfigTab,
} = require('./workspace_fixture.cjs');

const workspace = setupWorkspace();
const { mockPreview } = workspace;

async function readUserField(page, x, y) {
    await page.evaluate(() => navigator.clipboard.writeText(''));
    await click(page, x, y);
    await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
    await page.keyboard.press('Control+c');
    await page.waitForFunction(async () => Boolean(await navigator.clipboard.readText()), undefined, { timeout });
    const value = await page.evaluate(() => navigator.clipboard.readText());
    await page.evaluate(() => navigator.clipboard.writeText(''));
    return value;
}

test('edits WaterWall user access and limits and creates a user with an exact ID while offline', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'Offline fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [
        { id: 11, name: config.name, enabled: true, active: false, auth_nodes: ['staff'] },
    ];
    const user = {
        id: '9007199254740993', name: 'Fixture account', enabled: false, gid: '4', 'wireguard-allowed-ips': '',
        time: { 'created-at-ms': '1920000000000', 'expire-at-ms': '1924992000000' },
        limit: { traffic: { up: '9875654321', down: '1000', total: '18446744073709551615' } },
        stats: { traffic: { up: '20', down: '30' } },
    };
    const preview = await mockPreview(page, data);
    const selector = { config_id: 11, node: 'staff', page: 0 };
    const passwordSelector = { action: 'get_config_user_password', config_id: 11, node: 'staff', user_id: user.id };
    const storedPassword = 'fixture-saved-credential';
    const listing = record => ({ users: [record], editable: true, page: 0, page_count: 1, total: 1 });
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 11 });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });

        await selectConfigTab(page, 'staff');
        const read = await preview.action(1);
        assertRequest(read.request, { action: 'get_config_users', ...selector });
        read.reply({ result: listing(user) });
        await status(page, /staff tab\. AuthenticationServer users\..*1 users?\./);

        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const passwordRead = await preview.action(2);
        assertRequest(passwordRead.request, passwordSelector);
        await status(page, /Loading saved password\./);
        await selectConfigTab(page, 'Nodes');
        await selectConfigTab(page, 'staff');
        await status(page, /Edit user\. Account section\. Loading saved password\./);
        assert.equal(preview.actions.length, 3, 'Returning to the editor keeps its pending password lookup.');
        passwordRead.reply({ result: { password: storedPassword } });
        assert.ok(await readUserField(page, 300, 317) === storedPassword, 'The saved password is visible in the editor.');
        assert.ok(!(await page.locator('#canvas').getAttribute('aria-label')).includes(storedPassword));
        await click(page, 237, 747);
        assert.equal(preview.actions.length, 3, 'The saved password alone leaves the editor unchanged.');
        await replaceText(page, 300, 317, '');
        await click(page, 306, 166);
        await status(page, /Edit user\. Access section\./);
        await replaceText(page, 300, 279, '18446744073709551616');
        await status(page, /Group ID requires a whole number/);
        await click(page, 237, 747);
        assert.equal(preview.actions.length, 3);
        const gid = '18446744073709551615';
        const allowedIps = '192.0.2.15/32';
        await replaceText(page, 300, 279, gid);
        await replaceText(page, 820, 279, allowedIps);
        await replaceText(page, 820, 350, '');

        await click(page, 370, 166);
        await status(page, /Edit user\. Limits section\./);
        assert.equal(await readUserField(page, 300, 298), '9.875');
        assert.equal(await readUserField(page, 820, 298), '0.000');
        for (const invalid of ['10.5312', '18446744073.710']) {
            await replaceText(page, 300, 298, invalid);
            await status(page, /Upload traffic \(GB\).*three decimal places/);
            await click(page, 237, 747);
            assert.equal(preview.actions.length, 3, 'Invalid traffic quotas cannot be saved.');
        }
        await replaceText(page, 300, 298, '10.531');
        await replaceText(page, 820, 298, '');
        await click(page, 433, 166);
        await status(page, /Edit user\. Activity section\./);
        await click(page, 237, 747);
        const save = await preview.action(3);
        assertRequest(save.request, {
            action: 'edit_config_user', ...selector, user_id: user.id,
            user: { gid, 'wireguard-allowed-ips': allowedIps, time: { 'expire-at-ms': null },
                limit: { traffic: { up: '10531000000', down: null } } },
        });
        const updated = { ...user, gid, 'wireguard-allowed-ips': allowedIps,
            time: { 'created-at-ms': user.time['created-at-ms'] },
            limit: { traffic: { up: '10531000000', total: user.limit.traffic.total } } };
        await selectConfigTab(page, 'Nodes');
        save.reply({ feedback: 'Saved fixture user.', feedback_in_logs_only: true, result: listing(updated) });
        await selectConfigTab(page, 'staff');
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);
        await expectLoggedOnce(page, 'Saved fixture user.');
        assert.equal(preview.actions.length, 4, 'Returning to the tab preserves the pending save result.');

        await page.setViewportSize({ width: 1600, height: 900 });
        await settleCanvas(page);
        const cellText = [181, 180, 197];
        for (const [label, left, lastGlyph] of [['Group ID', 1152, 1341], ['Disabled state', 1358, 1428]]) {
            assert.ok(await rectangleContainsColor(page, {
                x: lastGlyph, y: 226, width: 1, height: 18,
            }, cellText), `${label} displays its final glyph on the first line.`);
            assert.equal(await rectangleContainsColor(page, {
                x: left, y: 248, width: lastGlyph - left + 2, height: 80,
            }, cellText), false, `${label} stays on one line.`);
        }
        await page.setViewportSize(coreDesktop);
        await settleCanvas(page);

        const replacementPassword = 'fixture-replacement-credential';
        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const previousPassword = await preview.action(4);
        assertRequest(previousPassword.request, passwordSelector);
        previousPassword.reply({ result: { password: storedPassword } });
        await replaceText(page, 300, 317, replacementPassword);
        await click(page, 237, 747);
        const replacement = await preview.action(5);
        const passwordChange = {
            action: 'edit_config_user', ...selector, user_id: user.id, user: { password: replacementPassword },
        };
        assertRequest(replacement.request, passwordChange);
        replacement.reply({ ok: false, feedback: 'Fixture password save failed.', feedback_in_logs_only: true });
        await expectLoggedOnce(page, 'Fixture password save failed.');
        assert.ok(await readUserField(page, 300, 317) === replacementPassword,
            'A rejected save leaves the replacement available to retry.');
        await click(page, 237, 747);
        const retryPassword = await preview.action(6);
        assertRequest(retryPassword.request, passwordChange);
        retryPassword.reply({ result: listing(updated) });
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);

        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const savedPassword = await preview.action(7);
        assertRequest(savedPassword.request, passwordSelector);
        savedPassword.reply({ result: { password: replacementPassword } });
        assert.ok(await readUserField(page, 300, 317) === replacementPassword,
            'Reopening the editor shows the replacement password.');
        await click(page, 344, 747);
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-new-credential';
        const id = '9007199254740995';
        await replaceText(page, 300, 317, password);
        await replaceText(page, 820, 317, id);
        await click(page, 242, 747);
        const create = await preview.action(8);
        assertRequest(create.request, {
            action: 'create_config_user', ...selector, user: { id, password, enabled: true },
        });
        create.reply({ result: { ...listing(updated), users: [updated, { id, enabled: true }], total: 2 } });
        await status(page, /staff tab\. AuthenticationServer users\. 2 users\./);

        await page.setViewportSize(portrait);
        await settleCanvas(page);
        await click(page, 326, 420 + donationSlotHeight);
        await status(page, /Delete user\./);
        assert.equal(preview.actions.length, 9, 'The mobile Delete action opens its confirmation without deleting the user.');
    } finally {
        await page.close();
    }
});

test('shows offline account state and group when the schematic omits those controls', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const config = { name: 'Offline display fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    const id = 11;
    data.server.core.configs = [{ id, name: config.name, enabled: true, active: false, auth_nodes: ['staff'] }];
    const schematic = structuredClone(workspace.waterwallSchematic);
    const { id: idField, password } = schematic.ui.users.fields;
    schematic.ui.users.fields = { id: idField, password };
    delete schematic.ui.users.access_fields;
    delete schematic.ui.users.limit_groups;
    delete schematic.ui.users.activity_groups;
    const preview = await mockPreview(page, data, Promise.resolve(), schematic);
    const enabled = [163, 211, 83];
    const secondary = [181, 180, 197];
    const inset = [13, 11, 14];
    const hasText = (x, y, width, color) => rectangleContainsColor(page, { x, y, width, height: 18 }, color);
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });
        await selectConfigTab(page, 'staff');
        const read = await preview.action(1);
        assertRequest(read.request, { action: 'get_config_users', config_id: id, node: 'staff', page: 0 });
        read.reply({ result: {
            users: [{ id: '1', gid: '4', enabled: true }, { id: '2', enabled: false }],
            editable: true, page: 0, page_count: 1, total: 2,
        } });
        await status(page, /staff tab\. AuthenticationServer users\..*2 users\./);
        await settleCanvas(page);

        assert.ok(await hasText(1072, 198, 56, enabled), 'The assigned account shows Enabled.');
        assert.ok(await hasText(1072, 226, 56, secondary), 'The unassigned account shows Disabled.');
        assert.ok(await hasText(1010, 198, 11, secondary), 'The assigned account shows group 4.');
        assert.equal(await hasText(1021, 198, 38, secondary), false,
            'The assigned account has a single-digit group label.');
        assert.ok(await hasText(1030, 226, 29, secondary), 'The unassigned group cell shows Default.');

        await page.setViewportSize(portrait);
        await settleCanvas(page);
        assert.ok(await hasText(32, 474, 58, enabled), 'The compact assigned row shows Enabled.');
        assert.ok(await hasText(32, 497, 58, secondary), 'The compact assigned row shows Group 4.');
        assert.ok(await hasText(32, 548, 58, secondary), 'The compact unassigned row shows Disabled.');
        assert.equal(await hasText(32, 571, 58, secondary), false, 'The compact unassigned row omits its group.');

        await page.setViewportSize(coreDesktop);
        await settleCanvas(page);
        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const savedPassword = await preview.action(2);
        assertRequest(savedPassword.request, {
            action: 'get_config_user_password', config_id: id, node: 'staff', user_id: '1',
        });
        savedPassword.reply({ result: { password: 'fixture-saved-credential' } });
        await settleCanvas(page);
        assert.equal((await verticalRuns(page, 420, 190, 435, inset, 15)).length, 1,
            'The password is the only input in Account.');
        assert.deepEqual(await verticalRuns(page, 925, 190, 435, inset, 15), []);
        await click(page, 306, 166);
        await status(page, /Edit user\. Access section\./);
        assert.equal(await rectangleContainsColor(page, {
            x: 209, y: 190, width: 1040, height: 250,
        }, [234, 235, 255]), false, 'Access shows no field labels for the omitted controls.');
        assert.deepEqual(await verticalRuns(page, 420, 190, 435, inset, 15), []);
    } finally {
        await page.close();
    }
});

test('lists and refreshes AuthenticationServer live users while offline editing is locked', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const config = { name: 'Live fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    const id = 2147483646;
    data.server.core.configs = [
        { id, name: config.name, enabled: true, active: false, auth_nodes: ['staff'], live_users: true },
    ];
    const preview = await mockPreview(page, data);
    const listRequest = { action: 'get_live_config_users', config_id: id, node: 'staff' };
    const cursor = '12'.repeat(32);
    const user = { id: '9007199254740993', name: 'Fixture account', enabled: true, usage: { u: '2048', d: '4096' } };
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);
        data.server.core.state = 'running';
        data.server.core.configs[0].active = true;
        data.logs[0].message = 'Fixture config started.';
        await preview.broadcast();
        await status(page, /Latest log: Fixture config started\./);
        await graphMenuItem(page, 'nodes', 0);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Add node\./,
            'An active config with a large ID locks its graph.');

        await selectConfigTab(page, 'staff');
        await status(page, /staff tab\. AuthenticationServer users\..*Stop WaterWall to access this users database\./);
        assert.equal(preview.actions.length, 1, 'An active process blocks offline user access.');

        await selectConfigTab(page, 'staff Live');
        const first = await preview.action(1);
        assertRequest(first.request, listRequest);
        first.reply({ result: { available: true, total: 2, users: [user], next_cursor: cursor } });
        await status(page, /staff Live tab\. AuthenticationServer live users\..*2 users\..*Page 1\./);

        await click(page, 300, 137);
        const next = await preview.action(2);
        assertRequest(next.request, { ...listRequest, cursor });
        next.reply({ result: { available: true, total: 2, users: [{ ...user, id: '2' }], next_cursor: null } });
        await status(page, /staff Live tab\..*2 users\..*Page 2\./);
        const refresh = await preview.action(3);
        assertRequest(refresh.request, { ...listRequest, cursor });
        refresh.reply({ ok: false, feedback: 'The live cursor is no longer current.', result: { reset_cursor: true } });

        const restarted = await preview.action(4);
        assertRequest(restarted.request, listRequest);
        restarted.reply({ result: { available: true, total: 1, users: [user], next_cursor: null } });
        await status(page, /staff Live tab\..*1 users?\./);
    } finally {
        await page.close();
    }
});

test('uses adapter ranges, automatic IDs, and whole-megabyte quota bounds', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const config = { name: 'User ranges fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [{ id: 12, name: config.name, enabled: true, active: false, auth_nodes: ['staff'] }];
    const schematic = structuredClone(workspace.waterwallSchematic);
    const controls = schematic.ui.users;
    delete controls.fields.id.minimum;
    delete controls.access_fields.find(field => field.path === '/record-stat-interval-ms').maximum;
    const uploadQuota = controls.limit_groups[0].fields.find(field => field.path === '/limit/traffic/up');
    Object.assign(uploadQuota, { minimum: 500000, maximum: 2500000 });
    const preview = await mockPreview(page, data, Promise.resolve(), schematic);
    const selector = { config_id: 12, node: 'staff', page: 0 };
    const listing = users => ({ users, editable: true, page: 0, page_count: 1, total: users.length });
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 12 });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });
        await selectConfigTab(page, 'staff');
        const read = await preview.action(1);
        assertRequest(read.request, { action: 'get_config_users', ...selector });
        read.reply({ result: listing([]) });
        await status(page, /staff tab\. AuthenticationServer users\. 0 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-explicit-id';
        await replaceText(page, 300, 317, password);
        await replaceText(page, 820, 317, '0');
        await status(page, /User ID requires a whole number from 1 to 18446744073709551615\./);
        await click(page, 242, 747);
        assert.equal(preview.actions.length, 2, 'Explicit ID 0 cannot be submitted.');

        await replaceText(page, 820, 317, '1');
        await click(page, 306, 166);
        await status(page, /Add user\. Access section\./);
        await replaceText(page, 300, 350, '2147483648');
        await status(page, /Record stats interval \(ms\) requires a whole number from 0 to 2147483647\./);
        await click(page, 242, 747);
        assert.equal(preview.actions.length, 2, 'An interval above INT_MAX cannot be submitted.');
        const interval = '2147483647';
        await replaceText(page, 300, 350, interval);
        await click(page, 370, 166);
        await status(page, /Add user\. Limits section\./);
        for (const outside of ['0', '0.003']) {
            await replaceText(page, 300, 298, outside);
            await status(page, /Upload traffic \(GB\) must be between 0\.001 and 0\.002/);
            await click(page, 242, 747);
            assert.equal(preview.actions.length, 2, 'A quota outside the whole-megabyte range cannot be submitted.');
        }
        await replaceText(page, 300, 298, '0.001');
        await click(page, 242, 747);
        const explicit = await preview.action(2);
        const minimumQuota = { traffic: { up: '1000000' } };
        assertRequest(explicit.request, {
            action: 'create_config_user', ...selector,
            user: { id: '1', password, enabled: true, 'record-stat-interval-ms': interval, limit: minimumQuota },
        });
        const created = { id: '1', enabled: true, 'record-stat-interval-ms': interval, limit: minimumQuota };
        explicit.reply({ result: listing([created]) });
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const automaticPassword = 'fixture-automatic-id';
        await replaceText(page, 300, 317, automaticPassword);
        await click(page, 370, 166);
        await status(page, /Add user\. Limits section\./);
        await replaceText(page, 300, 298, '0.002');
        await replaceText(page, 820, 298, '18446744073.709');
        await click(page, 242, 747);
        const automatic = await preview.action(3);
        assertRequest(automatic.request, {
            action: 'create_config_user', ...selector,
            user: { password: automaticPassword, enabled: true,
                limit: { traffic: { up: '2000000', down: '18446744073709000000' } } },
        });
        automatic.reply({ result: listing([created, { id: '9007199254740993', enabled: true }]) });
        await status(page, /staff tab\. AuthenticationServer users\. 2 users\./);
    } finally {
        await page.close();
    }
});
