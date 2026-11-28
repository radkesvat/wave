const assert = require('node:assert/strict');
const { test } = require('node:test');
const {
    setupWorkspace,
    coreDesktop,
    rosterColumnX,
    portrait,
    timeout,
    fixture,
    status,
    expectLoggedOnce,
    settleCanvas,
    click,
    replaceText,
    assertRequest,
    selectConfigTab,
    horizontalRuns,
    verticalRuns,
    rectangleContainsColor,
} = require('./workspace_fixture.cjs');

const workspace = setupWorkspace();
const { mockPreview } = workspace;
const offlineScope = { source: 'offline', run_id: 'staff-stopped' };
const offlineAccess = { ...offlineScope, database_generation: 'staff-database-1' };

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

async function openOfflineEditor(page, y = 226, item = 1) {
    await click(page, rosterColumnX.options, y);
    await click(page, rosterColumnX.options + 40, y + 18 + 23 * item);
}

test('edits WaterWall user access and limits and creates a user with an exact ID while offline', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'Offline fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [
        { id: 11, name: config.name, enabled: true, active: false, auth_nodes: ['staff'],
            users_access: { staff: offlineAccess } },
    ];
    const user = {
        id: '9007199254740993', name: 'Fixture account', enabled: false, gid: '4', 'wireguard-allowed-ips': '',
        time: { 'created-at-ms': '1920000000000', 'expire-at-ms': '1924992000000' },
        limit: { traffic: { up: '9875654321', down: '1000', total: '18446744073709551615' } },
        stats: { traffic: { up: '20', down: '30' } },
    };
    const schematic = structuredClone(workspace.waterwallSchematic);
    const uploadBandwidth = schematic.ui.users.limit_groups.flatMap(group => group.fields)
        .find(field => field.path === '/limit/bandwidth/up');
    uploadBandwidth.help = 'Bandwidth limits are rates for each direction. Enter 1048576 for 1 MiB/s.';
    const preview = await mockPreview(page, data, Promise.resolve(), schematic);
    const selector = { config_id: 11, node: 'staff', ...offlineScope, roster: true };
    const passwordSelector = { action: 'get_config_user_password', config_id: 11, node: 'staff',
        user_id: user.id, ...offlineScope };
    const storedPassword = 'fixture-saved-credential';
    let actionIndex = 0;
    let rosterIndex = 0;
    let subscription;
    let sequence = 1;
    const load = async rows => {
        const action = await preview.rosterAction(rosterIndex++);
        assert.equal(action.action, 'users_roster_subscribe');
        assert.equal(action.database_generation, offlineAccess.database_generation);
        subscription = action.subscription;
        await preview.roster(subscription, rows, { sequence: String(sequence++) });
    };
    const leave = async () => {
        await selectConfigTab(page, 'Nodes');
        assert.deepEqual(await preview.rosterAction(rosterIndex++),
            { action: 'users_roster_unsubscribe', subscription });
    };
    const details = async record => {
        await openOfflineEditor(page);
        const detail = await preview.action(actionIndex++);
        assertRequest(detail.request, { ...passwordSelector, action: 'get_config_user' });
        detail.reply({ result: { user: record } });
        return preview.action(actionIndex++);
    };
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(actionIndex++);
        assertRequest(opened.request, { action: 'get_core_config', id: 11 });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });

        await selectConfigTab(page, 'staff');
        await load([user]);
        await status(page, /staff tab\. AuthenticationServer users\..*1 users?\./);

        const passwordRead = await details(user);
        await status(page, /Edit user\. Account section\./);
        assertRequest(passwordRead.request, passwordSelector);
        await status(page, /Loading saved password\./);
        await leave();
        await selectConfigTab(page, 'staff');
        await load([user]);
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
        await page.mouse.move(409, 441);
        await status(page, 'Bandwidth limits are rates for each direction. Enter 1000000 for 1 MB/s.');
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
        const save = await preview.action(actionIndex++);
        assertRequest(save.request, {
            action: 'edit_config_user', ...selector, user_id: user.id,
            user: { gid, 'wireguard-allowed-ips': allowedIps, time: { 'expire-at-ms': null },
                limit: { traffic: { up: '10531000000', down: null } } },
        });
        const updated = { ...user, gid, 'wireguard-allowed-ips': allowedIps,
            time: { 'created-at-ms': user.time['created-at-ms'] },
            limit: { traffic: { up: '10531000000', total: user.limit.traffic.total } } };
        await leave();
        save.reply({ feedback: 'Saved fixture user.', feedback_in_logs_only: true, result: { user_id: user.id } });
        await selectConfigTab(page, 'staff');
        await load([updated]);
        await status(page, /staff tab\. AuthenticationServer users\..*1 users\./);
        await expectLoggedOnce(page, 'Saved fixture user.');
        assert.equal(preview.actions.length, actionIndex, 'Returning to the tab preserves the pending save result.');

        await page.setViewportSize({ width: 1600, height: 900 });
        await settleCanvas(page);
        await click(page, 320, 226);
        await status(page, /Selected account 9007199254740993 is disabled\. Group 18446744073709551615\./);
        await page.setViewportSize(coreDesktop);
        await settleCanvas(page);

        const replacementPassword = 'fixture-replacement-credential';
        const previousPassword = await details(updated);
        await status(page, /Edit user\. Account section\./);
        assertRequest(previousPassword.request, passwordSelector);
        previousPassword.reply({ result: { password: storedPassword } });
        await replaceText(page, 300, 317, replacementPassword);
        await click(page, 237, 747);
        const replacement = await preview.action(actionIndex++);
        const passwordChange = {
            action: 'edit_config_user', ...selector, user_id: user.id, user: { password: replacementPassword },
        };
        assertRequest(replacement.request, passwordChange);
        replacement.reply({ ok: false, feedback: 'Fixture password save failed.', feedback_in_logs_only: true });
        await expectLoggedOnce(page, 'Fixture password save failed.');
        assert.ok(await readUserField(page, 300, 317) === replacementPassword,
            'A rejected save leaves the replacement available to retry.');
        await click(page, 237, 747);
        const retryPassword = await preview.action(actionIndex++);
        assertRequest(retryPassword.request, passwordChange);
        retryPassword.reply({ result: { user_id: user.id } });
        await status(page, /staff tab\. AuthenticationServer users\..*1 users\./);

        const savedPassword = await details(updated);
        await status(page, /Edit user\. Account section\./);
        assertRequest(savedPassword.request, passwordSelector);
        savedPassword.reply({ result: { password: replacementPassword } });
        assert.ok(await readUserField(page, 300, 317) === replacementPassword,
            'Reopening the editor shows the replacement password.');
        await click(page, 344, 747);
        await status(page, /staff tab\. AuthenticationServer users\..*1 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-new-credential';
        const id = '9007199254740995';
        await replaceText(page, 300, 317, password);
        await replaceText(page, 820, 317, id);
        await click(page, 242, 747);
        const create = await preview.action(actionIndex++);
        assertRequest(create.request, {
            action: 'create_config_user', ...selector, user: { id, password, enabled: true },
        });
        create.reply({ result: { user_id: id } });
        await preview.roster(subscription, [updated, { id, enabled: true }], { sequence: String(sequence++) });
        await status(page, /staff tab\. AuthenticationServer users\..*2 users\./);

        await page.setViewportSize(portrait);
        await settleCanvas(page);
        await replaceText(page, 150, 423, user.id);
        await status(page, /1 matches of 2 users/);
        await click(page, 302, 513);
        await click(page, 192, 577);
        await status(page, /Delete user\./);
        assert.equal(preview.actions.length, actionIndex, 'The compact Options menu waits for deletion confirmation.');
        await click(page, 64, 429);
        const deletion = await preview.action(actionIndex++);
        assertRequest(deletion.request, { action: 'delete_config_user', ...selector, user_id: user.id });
        deletion.reply({ result: { user_id: user.id } });
        await preview.roster(subscription, [{ id, enabled: true }], { sequence: String(sequence++) });
        await status(page, /1 users\./);
    } finally {
        await page.close();
    }
});

test('uses adapter ranges, automatic IDs, and whole-megabyte quota bounds', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const config = { name: 'User ranges fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [{ id: 12, name: config.name, enabled: true, active: false, auth_nodes: ['staff'],
        users_access: { staff: offlineAccess } }];
    const schematic = structuredClone(workspace.waterwallSchematic);
    const controls = schematic.ui.users;
    delete controls.fields.id.minimum;
    delete controls.access_fields.find(field => field.path === '/record-stat-interval-ms').maximum;
    const uploadQuota = controls.limit_groups[0].fields.find(field => field.path === '/limit/traffic/up');
    Object.assign(uploadQuota, { minimum: 500000, maximum: 2500000 });
    const preview = await mockPreview(page, data, Promise.resolve(), schematic);
    const selector = { config_id: 12, node: 'staff', ...offlineScope, roster: true };
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 12 });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });
        await selectConfigTab(page, 'staff');
        const { subscription } = await preview.rosterAction(0);
        await preview.roster(subscription, []);
        await status(page, /staff tab\. AuthenticationServer users\..*0 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-explicit-id';
        await replaceText(page, 300, 317, password);
        await replaceText(page, 820, 317, '0');
        await status(page, /User ID requires a whole number from 1 to 18446744073709551615\./);
        await click(page, 242, 747);
        assert.equal(preview.actions.length, 1, 'Explicit ID 0 cannot be submitted.');

        await replaceText(page, 820, 317, '1');
        await click(page, 306, 166);
        await status(page, /Add user\. Access section\./);
        await replaceText(page, 300, 350, '2147483648');
        await status(page, /Record stats interval \(ms\) requires a whole number from 0 to 2147483647\./);
        await click(page, 242, 747);
        assert.equal(preview.actions.length, 1, 'An interval above INT_MAX cannot be submitted.');
        const interval = '2147483647';
        await replaceText(page, 300, 350, interval);
        await click(page, 370, 166);
        await status(page, /Add user\. Limits section\./);
        for (const outside of ['0', '0.003']) {
            await replaceText(page, 300, 298, outside);
            await status(page, /Upload traffic \(GB\) must be between 0\.001 and 0\.002/);
            await click(page, 242, 747);
            assert.equal(preview.actions.length, 1, 'A quota outside the whole-megabyte range cannot be submitted.');
        }
        await replaceText(page, 300, 298, '0.001');
        await click(page, 242, 747);
        const explicit = await preview.action(1);
        const minimumQuota = { traffic: { up: '1000000' } };
        assertRequest(explicit.request, {
            action: 'create_config_user', ...selector,
            user: { id: '1', password, enabled: true, 'record-stat-interval-ms': interval, limit: minimumQuota },
        });
        const created = { id: '1', enabled: true, 'record-stat-interval-ms': interval, limit: minimumQuota };
        explicit.reply({ result: { user_id: created.id } });
        await preview.roster(subscription, [created], { sequence: '2' });
        await status(page, /staff tab\. AuthenticationServer users\..*1 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\. Next ID 2\./);
        const automaticPassword = 'fixture-automatic-id';
        await replaceText(page, 300, 317, automaticPassword);
        await click(page, 370, 166);
        await status(page, /Add user\. Limits section\./);
        await replaceText(page, 300, 298, '0.002');
        await replaceText(page, 820, 298, '18446744073.709');
        await click(page, 242, 747);
        const automatic = await preview.action(2);
        assertRequest(automatic.request, {
            action: 'create_config_user', ...selector,
            user: { password: automaticPassword, enabled: true,
                limit: { traffic: { up: '2000000', down: '18446744073709000000' } } },
        });
        // Another operator took ID 2 before the backend opened the saved database for this create.
        automatic.reply({ result: { user_id: '3' } });
        await preview.roster(subscription, [created, { id: '2', enabled: true }, { id: '3', enabled: true }],
            { sequence: '3' });
        await status(page, /staff tab\. AuthenticationServer users\..*3 users\./);
    } finally {
        await page.close();
    }
});

const liveOperations = ['list-users', 'get-user', 'status', 'create-user', 'update-user', 'change-user-password'];

async function openRosterFixture(page, source = 'offline', options = {}) {
    const node = 'staff';
    const config = { name: 'Roster fixture', nodes: [{ name: node, type: 'AuthenticationServer' }] };
    const configId = 11;
    const live = source === 'live';
    const access = { source, run_id: 'staff-run', database_generation: 'staff-db',
        ...(live ? { operations: liveOperations } : {}), ...options.access };
    const entry = { id: configId, name: config.name, enabled: true, active: live, auth_nodes: [node],
        users_access: { [node]: access } };
    const data = fixture(live ? 'running' : 'stopped');
    data.server.core.configs = [entry];
    const preview = await mockPreview(page, data, Promise.resolve(), options.schematic);
    await page.goto(workspace.url);
    await page.getByRole('link', { name: config.name, exact: true }).focus();
    await page.keyboard.press('Enter');
    const configRead = await preview.action(0);
    assertRequest(configRead.request, { action: 'get_core_config', id: configId });
    configRead.reply({ result: { config, revision: 'fixture', editable: !live } });
    await selectConfigTab(page, node);
    const scope = { config_id: configId, node, source, run_id: access.run_id };
    return { preview, data, entry, scope };
}

for (const [layout, viewport, tableStart, rowY] of [
    ['desktop', coreDesktop, 200, 226],
    ['compact', portrait, 22, 511],
]) {
    test(`fills the fixed-width Enable cell on ${layout}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { preview, scope } = await openRosterFixture(page);
            const { subscription } = await preview.rosterAction(0);
            const row = { id: '41', name: 'Alice', enabled: true };
            await preview.roster(subscription, [row]);
            await status(page, /Saved · 1 users/);
            await page.setViewportSize(viewport);
            await settleCanvas(page);

            const edges = async () => {
                const dividers = await horizontalRuns(page, rowY + 35, tableStart, viewport.width, [61, 58, 67]);
                // After ID and Name, the next two dividers enclose Enable.
                return [dividers[1][0], dividers[2][0]];
            };
            const [left, right] = await edges();
            const accent = [243, 129, 74];
            const track = await horizontalRuns(page, rowY, left, right, accent);
            assert.ok(track.length && track[0][0] - left <= 10 && right - track.at(-1)[1] <= 10,
                'The switch reaches both sides of the cell inside its padding.');
            const height = await verticalRuns(page, left + 22, rowY - 20, rowY + 20, accent, 21);
            assert.ok(height.length, 'The track uses the row control height.');

            await page.mouse.move(right, rowY + 35);
            await page.mouse.down();
            await settleCanvas(page);
            await page.mouse.move(right + 36, rowY + 35, { steps: 6 });
            await settleCanvas(page);
            await page.mouse.up();
            await page.mouse.move(0, 0);
            await settleCanvas(page);
            assert.deepEqual(await edges(), [left, right], 'Dragging the Enable divider keeps its width fixed.');

            await click(page, right - 10, rowY);
            const toggle = await preview.action(1);
            assertRequest(toggle.request, { action: 'edit_config_user', ...scope, user_id: row.id,
                roster: true, user: { enabled: false } });
            toggle.reply({ result: { user_id: row.id } });
            await preview.roster(subscription, [{ ...row, enabled: false }], { sequence: '2' });
            await status(page, /Selected account 41 is disabled/);
        } finally {
            await page.close();
        }
    });

    test(`keeps the users table stationary during account updates on ${layout}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { preview, scope } = await openRosterFixture(page, 'live');
            const { subscription } = await preview.rosterAction(0);
            const row = { id: '41', enabled: true };
            await preview.roster(subscription, [row]);
            await status(page, /Live · 1 users/);
            await page.setViewportSize(viewport);
            await settleCanvas(page);

            const scanBottom = rowY + 80;
            const border = [61, 58, 67];
            const dividers = await horizontalRuns(page, scanBottom, tableStart, viewport.width, border);
            const toggleLeft = dividers[1][0];
            const tableTop = async () => {
                const runs = await verticalRuns(page, toggleLeft, rowY - 90, scanBottom, border, 20);
                return runs.find(([, end]) => end === scanBottom)?.[0];
            };
            const top = await tableTop();
            assert.ok(top, 'The table divider reaches the body of the roster.');
            const [track] = await verticalRuns(page, toggleLeft + 22, top, scanBottom, [243, 129, 74], 20);
            assert.ok(track, 'The first account has an enabled toggle.');
            const spinner = () => rectangleContainsColor(page, {
                x: viewport.width - 90, y: top - 80, width: 88, height: 85,
            }, [81, 165, 246]);

            assert.equal(await spinner(), false);
            await preview.rosterFrame(subscription, { event: 'state', state: 'loading',
                message: 'Checking the latest charged upload and download counters for every account in the user database.' });
            await status(page, /Refreshing accounts/);
            assert.equal(await tableTop(), top, 'A roster refresh keeps the rows in place.');
            assert.equal(await spinner(), true);
            await preview.rosterFrame(subscription, { event: 'state', state: 'ready' });
            await status(page, /Live · 1 users/);
            await preview.rosterFrame(subscription, { event: 'begin', attempt: '2', sequence: '2',
                base_sequence: '1', total: 1 });
            await settleCanvas(page);
            await status(page, /Live · 1 users/);
            assert.equal(await spinner(), false, 'Background changes do not flash the spinner.');
            await preview.rosterFrame(subscription, { event: 'end', attempt: '2', chunks: 0 });

            const rowCenter = (track[0] + track[1]) / 2;
            await click(page, toggleLeft + 22, rowCenter);
            const details = await preview.action(1);
            assertRequest(details.request, { action: 'get_live_config_user', ...scope, user_id: row.id });
            assert.equal(await tableTop(), top, 'The pending read keeps the rows in place.');
            assert.equal(await spinner(), true, 'The spinner is visible near the top right of the roster.');
            const token = '34'.repeat(24);
            details.reply({ result: { user: row, if_match: token } });
            const update = await preview.action(2);
            assertRequest(update.request, { action: 'edit_live_config_user', ...scope, user_id: row.id,
                if_match: token, user: { enabled: false } });
            assert.equal(await tableTop(), top, 'The pending write keeps the rows in place.');
            const message = 'The account change was applied to the running core.';
            update.reply({ feedback: message, feedback_in_logs_only: true,
                result: { outcome: 'applied', user_id: row.id } });
            await preview.roster(subscription, [{ ...row, enabled: false }],
                { attempt: '3', sequence: '3', base_sequence: '2' });
            await status(page, /Selected account 41 is disabled/);
            assert.equal(await tableTop(), top, 'The result keeps the rows in place.');
            assert.equal(await spinner(), false);
            await expectLoggedOnce(page, message);

            const optionsX = dividers[2][0] + 25;
            await click(page, optionsX, rowCenter);
            await click(page, layout === 'compact' ? viewport.width / 2 : optionsX + 40, rowCenter + 18);
            const lookup = await preview.action(3);
            assertRequest(lookup.request, { action: 'get_live_config_user', ...scope, user_id: row.id });
            await status(page, /Loading account details/);
            assert.equal(await tableTop(), top, 'Opening the cancellable lookup keeps the rows in place.');
            assert.equal(await spinner(), true);
            await click(page, viewport.width - 110, top - 14);
            assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Loading account details/);
            lookup.reply({ result: { user: row, if_match: token } });
            await settleCanvas(page);
            assert.equal(await spinner(), false);
            assert.equal(await tableTop(), top, 'Cancelling the lookup leaves the roster in place.');
        } finally {
            await page.close();
        }
    });
}

for (const source of ['live', 'offline']) {
    test(`sorts, searches and scrolls 1,000 ${source} accounts locally`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { preview, scope, entry } = await openRosterFixture(page, source);
            const sub = await preview.rosterAction(0);
            assert.deepEqual(sub, { action: 'users_roster_subscribe', ...scope, database_generation: 'staff-db',
                subscription: sub.subscription });
            await status(page, /Loading users/);
            const id = index => String(9007199254740993n + BigInt(index));
            const alice = '18446744073709551615';
            const rows = Array.from({ length: 1000 }, (_, index) => ({
                id: index === 999 ? alice : id(index), enabled: true, gid: '0',
                name: index === 999 ? 'Élodie##team' : index === 998 ? 'Elodie' : `Account ${index}`,
                name_preview_state: index === 999 ? 'truncated' : 'complete',
                stats: { traffic: { up: index === 998 ? '1' : '18446744073709551615',
                    down: index === 999 ? '18446744073709551615' : String(20 + index) } },
                time: { 'expire-at-ms': '0', 'expire-after-first-usage-ms': '0' },
            }));
            await preview.rosterFrame(sub.subscription, { event: 'begin', attempt: '1', sequence: '1', total: 1000 });
            await preview.rosterFrame(sub.subscription, { event: 'chunk', attempt: '1', index: 0,
                rows: rows.slice(0, 250), removed: [] });
            await status(page, /Loading 250 of 1000/);
            await replaceText(page, 420, 137, '  ÉLODIE  ');
            await status(page, /Loading 250 of 1000/);
            assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /No matches/);
            for (let index = 1; index < 4; ++index) {
                await preview.rosterFrame(sub.subscription, { event: 'chunk', attempt: '1', index,
                    rows: rows.slice(index * 250, (index + 1) * 250), removed: [] });
            }
            await preview.rosterFrame(sub.subscription, { event: 'end', attempt: '1', chunks: 4 });
            await status(page, /1 matches of 1000 users/);
            await click(page, 320, 222);
            await status(page, new RegExp(`Selected account ${alice}`));
            await replaceText(page, 420, 137, '  000' + id(474) + '  ');
            await status(page, /1 matches of 1000 users/);
            await click(page, 320, 222);
            await status(page, new RegExp(`Selected account ${id(474)}`));
            await replaceText(page, 420, 137, 'ELODIE');
            await status(page, /1 matches of 1000 users/);
            await click(page, 320, 222);
            await status(page, new RegExp(`Selected account ${id(998)}`));

            await replaceText(page, 420, 137, '  Account 474  ');
            await status(page, /1 matches of 1000 users/);
            await replaceText(page, 420, 137, 'Account  474');
            await status(page, /0 matches of 1000 users.*No matches/);
            await replaceText(page, 420, 137, ' 18446744073709551616 ');
            await status(page, /0 matches of 1000 users.*No matches/);
            await replaceText(page, 420, 137, '   ');
            await status(page, /1000 users\. Sorted by ID ascending/);
            await click(page, rosterColumnX.trafficUsage, 194);
            await status(page, /Sorted by Traffic usage ascending/);
            await click(page, 320, 222);
            await status(page, new RegExp(`Selected account ${id(998)}`));
            await page.mouse.move(350, 430);
            await page.mouse.wheel(0, 600);
            await settleCanvas(page);
            await click(page, 320, 222);
            assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'),
                new RegExp(`Selected account ${id(998)} is`), 'The frozen header keeps the first visible row clickable.');
            await page.mouse.wheel(0, -1000);
            await settleCanvas(page);

            await preview.roster(sub.subscription, [{ ...rows[999], stats: { traffic: { up: '0', down: '0' } } }],
                { sequence: '2', base_sequence: '1', total: 1000, attempt: '2' });
            await click(page, 320, 222);
            await status(page, new RegExp(`Selected account ${alice} is enabled\\. Group Default\\. Traffic usage 0 B`));
            assert.equal(preview.actions.length, 1, 'Search, sort and scrolling leave the ordinary socket action queue unchanged.');
            assert.equal(preview.rosterActions.length, 1);

            await page.setViewportSize(portrait);
            await settleCanvas(page);
            await status(page, /Sorted by Traffic usage ascending/);
            await click(page, 110, 505);
            await status(page, new RegExp(`Selected account ${alice}`));
            await click(page, 44, 478);
            await status(page, /Sorted by ID ascending/);
            assert.equal(preview.rosterActions.length, 1);
            assert.equal(entry.active, source === 'live');
        } finally {
            await page.close();
        }
    });
}

test('sorts combined traffic exactly across the 64-bit boundary and keeps missing usage last', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview } = await openRosterFixture(page, 'live');
        const { subscription } = await preview.rosterAction(0);
        const max = '18446744073709551615';
        const row = (id, up, down) => ({ id: String(id), enabled: true, stats: { traffic: { up, down } } });
        await preview.roster(subscription, [
            row(1, max, max), row(2, '10', null), row(7, max, '1'), row(9, max, '2'),
            row(11, max, '0'), row(12, '1', max), row(20, '0', '0'),
        ]);
        await status(page, /Live · 7 users/);
        for (const [direction, ids] of [
            ['ascending', [20, 11, 7, 12, 9, 1, 2]],
            ['descending', [1, 9, 7, 12, 11, 20, 2]],
        ]) {
            await click(page, rosterColumnX.trafficUsage, 194);
            await status(page, new RegExp(`Sorted by Traffic usage ${direction}`));
            for (const [index, id] of ids.entries()) {
                await click(page, 320, 222 + 38 * index);
                await status(page, new RegExp(`Selected account ${id} is enabled`));
                if (id === 1) {
                    await status(page, /Traffic usage 36\.9 EB\. Avg upload speed Unavailable\. Avg download speed Unavailable\./);
                }
            }
            await status(page, /Traffic usage Unavailable\./);
        }
        assert.equal(preview.actions.length, 1, 'Sorting and reading unavailable speeds use the local roster.');
        assert.equal(preview.rosterActions.length, 1);
    } finally {
        await page.close();
    }
});

for (const source of ['live', 'offline']) {
    test(`shows per-direction average charged speeds only over observed live reports (${source})`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { preview } = await openRosterFixture(page, source);
            const { subscription } = await preview.rosterAction(0);
            const origin = await page.evaluate(() => Math.ceil(performance.now()));
            const row = (id, up, down) => ({ id, enabled: true, stats: { traffic: { up, down } } });
            const report = async (sequence, rows) => {
                const elapsed = (sequence - 1) * 2000;
                await page.evaluate(time => { performance.now = () => time; }, origin + elapsed);
                await preview.roster(subscription, rows, { sequence: String(sequence), total: 3,
                    base_sequence: sequence === 1 ? undefined : String(sequence - 1),
                    host_time: String(1795000000000 + elapsed) });
            };
            const unavailable = /Avg upload speed Unavailable\. Avg download speed Unavailable\./;
            await report(1, [row('1', '10', '50'), row('2', '0', '0'), row('3', '0', '0')]);
            await status(page, /3 users/);
            await click(page, 320, 222);
            await status(page, unavailable);
            await report(2, [row('1', '6000000', '2000000'), row('2', '5000', '1000')]);
            await status(page, /Traffic usage 8\.0 MB\. Avg upload speed Unavailable\. Avg download speed Unavailable\./);
            await report(3, [row('1', '2006000000', '2000000'), row('2', '5000', '2001000')]);
            await status(page, /Traffic usage 2\.0 GB/);
            if (source === 'offline') {
                await status(page, unavailable);
                return;
            }
            await status(page, /Avg upload speed 1\.0 GB\/s\. Avg download speed 0 B\/s\./);

            await click(page, rosterColumnX.uploadSpeed, 194);
            await status(page, /Sorted by Avg upload speed ascending/);
            await click(page, 320, 298);
            await status(page, /Selected account 3 .*Avg upload speed Unavailable\./);
            await click(page, 320, 222);
            await status(page, /Selected account 2 .*Avg upload speed 0 B\/s\. Avg download speed 1\.0 MB\/s\./);
            await report(4, [{ ...row('2', '5000', '2001000'), enabled: false }]);
            await status(page, /Selected account 2 is disabled.*Avg download speed 1\.0 MB\/s\./);
            await click(page, rosterColumnX.downloadSpeed, 194);
            await status(page, /Sorted by Avg download speed ascending/);
            await click(page, 320, 222);
            await status(page, /Selected account 1 /);

            await report(5, [row('1', '2006000000', '502000000'), row('2', '5600', '2001000')]);
            await status(page, /Traffic usage 2\.5 GB/);
            await click(page, 320, 222);
            await status(page, /Selected account 2 .*Avg upload speed 150 B\/s\. Avg download speed 0 B\/s\./);
            await preview.rosterFrame(subscription, { event: 'state', state: 'stale', message: 'Fixture outage.' });
            await status(page, /Stale · 3 users/);
            await status(page, unavailable);
            assert.equal(preview.actions.length, 1);
            assert.equal(preview.rosterActions.length, 1);
        } finally {
            await page.close();
        }
    });
}

test('discards broken chunks, retains a complete stale roster, and scopes data to visible subscriptions', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope, entry, data } =
            await openRosterFixture(page, 'live', { access: { operations: ['list-users'] } });
        const first = (await preview.rosterAction(0)).subscription;
        const alice = { id: '42', name: 'Alice', enabled: true };
        await preview.rosterFrame(first, { event: 'begin', attempt: '1', sequence: '1', total: 2 });
        await preview.rosterFrame(first, { event: 'chunk', attempt: '1', index: 0, rows: [alice] });
        await status(page, /Loading 1 of 2/);
        await preview.rosterFrame(first, { event: 'chunk', attempt: '1', index: 2,
            rows: [{ id: '43', enabled: true }], removed: [] });
        assert.deepEqual(await preview.rosterAction(1), { action: 'users_roster_refresh', subscription: first, resync: true });
        await preview.rosterFrame(first, { event: 'end', attempt: '1', chunks: 2 });
        await status(page, /Loading users/);
        await preview.roster(first, [alice], { attempt: '2' });
        await status(page, /Live · 1 users/);
        await preview.rosterFrame(first, { event: 'state', state: 'stale', message: 'Core probe failed.' });
        await status(page, /Core probe failed.*Stale · 1 users.*Showing stale accounts/);
        await preview.rosterFrame(first, { event: 'state', state: 'ready' });
        await status(page, /Live · 1 users/);

        const visibility = state => page.evaluate(state => {
            Object.defineProperty(document, 'visibilityState', { configurable: true, value: state });
            document.dispatchEvent(new Event('visibilitychange'));
        }, state);
        await visibility('hidden');
        assert.deepEqual(await preview.rosterAction(2), { action: 'users_roster_unsubscribe', subscription: first });
        await preview.roster(first, [alice, { id: '43', enabled: true }], { sequence: '2' });
        await visibility('visible');
        const second = await preview.rosterAction(3);
        assert.equal(second.action, 'users_roster_subscribe');
        assert.notEqual(second.subscription, first);
        await status(page, /Loading users/);
        await preview.roster(second.subscription, []);
        await status(page, /0 users.*No users/);

        entry.users_access.staff = { source: 'live', operations: ['list-users'], run_id: 'restarted',
            database_generation: 'replacement' };
        await preview.broadcast();
        assert.equal((await preview.rosterAction(4)).action, 'users_roster_unsubscribe');
        const restarted = await preview.rosterAction(5);
        assert.deepEqual(restarted, { action: 'users_roster_subscribe', ...scope, run_id: 'restarted',
            database_generation: 'replacement', subscription: restarted.subscription });
        await preview.rosterFrame(second.subscription, { event: 'state', state: 'error', message: 'Old subscription' });
        await preview.roster(restarted.subscription, [alice]);
        await status(page, /Live · 1 users/);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Old subscription/);

        entry.users_access.staff.operations = ['status'];
        await preview.broadcast();
        await status(page, /check database status/);
        assert.deepEqual(await preview.rosterAction(6), { action: 'users_roster_unsubscribe',
            subscription: restarted.subscription });
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /1 users|Alice/);
        await click(page, 260, 160);
        const databaseStatus = await preview.action(1);
        assertRequest(databaseStatus.request, { action: 'get_live_config_user_status', ...scope, run_id: 'restarted' });
        databaseStatus.reply({ result: { status: { database_loaded: true, user_count: '1' } } });
        await status(page, /Database status\./);
        assert.equal(preview.rosterActions.length, 7);

        data.server.core.state = 'stopping';
        await preview.broadcast();
        await status(page, /WaterWall is stopping/);
        assert.equal(preview.rosterActions.length, 7);
        data.server.core.state = 'stopped';
        entry.active = false;
        entry.users_access.staff = { source: 'offline', run_id: 'saved-run', database_generation: 'replacement' };
        await preview.broadcast();
        const saved = await preview.rosterAction(7);
        assert.equal(saved.source, 'offline');
        await preview.roster(saved.subscription, []);
        await status(page, /Saved · 0 users/);
    } finally {
        await page.close();
    }
});

test('uses saved detail before editing and blocks a draft when its roster row is removed', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope } = await openRosterFixture(page);
        const { subscription } = await preview.rosterAction(0);
        const row = { id: '41', name: 'Saved account', enabled: true };
        await preview.roster(subscription, [row]);
        await status(page, /Saved · 1 users/);
        await openOfflineEditor(page, 226, 0);
        const detail = await preview.action(1);
        assertRequest(detail.request, { action: 'get_config_user', ...scope, user_id: row.id });
        await status(page, /Loading account details/);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Edit user/);
        detail.reply({ result: { user: { ...row, email: 'provided-only-by-detail@example.invalid' } } });
        await status(page, /Edit user\. Account section/);
        await preview.roster(subscription, [], { sequence: '2', base_sequence: '1', total: 0, removed: [row.id] });
        await status(page, /This account was removed\. Its draft can no longer be saved/);
        await click(page, 237, 747);
        assert.equal(preview.actions.length, 2);
        assert.equal(preview.rosterActions.length, 1);
    } finally {
        await page.close();
    }
});

test('resets saved traffic for the selected user only after confirmation', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope } = await openRosterFixture(page);
        const { subscription } = await preview.rosterAction(0);
        const row = { id: '41', name: 'Saved account', enabled: true, stats: { traffic: { up: '40', down: '52' } } };
        await preview.roster(subscription, [row]);
        await status(page, /Saved · 1 users/);

        await openOfflineEditor(page, 226, 3);
        await status(page, /Reset traffic\./);
        assert.equal(preview.actions.length, 1, 'Opening confirmation sends no mutation.');
        await click(page, 336, 179);
        await status(page, /Selected account 41 is enabled.*Traffic usage 92 B/);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Reset traffic\./);
        assert.equal(preview.actions.length, 1, 'Cancel keeps the previous counters.');

        await openOfflineEditor(page, 226, 3);
        await status(page, /Reset traffic\./);
        await click(page, 243, 179);
        const reset = await preview.action(1);
        assertRequest(reset.request, { action: 'reset_config_user_traffic', ...scope, user_id: row.id, roster: true });
        reset.reply({ result: { user_id: row.id } });
        await preview.roster(subscription, [{ ...row, stats: { traffic: { up: '0', down: '0' } } }],
            { sequence: '2', base_sequence: '1' });
        await status(page, /Selected account 41 is enabled.*Traffic usage 0 B/);
        assert.equal(preview.actions.length, 2);
    } finally {
        await page.close();
    }
});

test('disables reset traffic while WaterWall is running with every management permission', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope } = await openRosterFixture(page, 'live');
        const { subscription } = await preview.rosterAction(0);
        const row = { id: '41', name: 'Live account', enabled: true, stats: { traffic: { up: '40', down: '52' } } };
        await preview.roster(subscription, [row]);
        await status(page, /Live · 1 users/);

        await click(page, rosterColumnX.options, 226);
        await click(page, rosterColumnX.options + 40, 290);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Reset traffic\./);
        assert.equal(preview.actions.length, 1);
        // A disabled menu item keeps Options open. Account details still works in the same popup.
        await click(page, rosterColumnX.options + 40, 244);
        const details = await preview.action(1);
        assertRequest(details.request, { action: 'get_live_config_user', ...scope, user_id: row.id });
    } finally {
        await page.close();
    }
});

test('keeps oversized rows identifiable and deletes them by ID after confirmation', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope } = await openRosterFixture(page);
        const { subscription } = await preview.rosterAction(0);
        const oversized = { id: '41', enabled: true, oversized: true };
        const neighbor = { id: '42', name: 'Neighbor', enabled: false };
        await preview.roster(subscription, [oversized, neighbor]);
        await status(page, /Saved · 2 users/);
        await openOfflineEditor(page, 226, 0);
        assert.equal(preview.actions.length, 1, 'An oversized roster row has no edit action.');
        await page.keyboard.press('Escape');
        await settleCanvas(page);
        await openOfflineEditor(page, 226, 2);
        await status(page, /Delete user/);
        assert.equal(preview.actions.length, 1, 'The option opens confirmation before sending a write.');
        await click(page, 243, 179);
        const deletion = await preview.action(1);
        assertRequest(deletion.request, { action: 'delete_config_user', ...scope, user_id: oversized.id, roster: true });
        deletion.reply({ result: { user_id: oversized.id } });
        await preview.roster(subscription, [], { sequence: '2', base_sequence: '1', total: 1, removed: [oversized.id] });
        await status(page, /Saved · 1 users/);
        await click(page, 320, 222);
        await status(page, /Selected account 42 is disabled/);
    } finally {
        await page.close();
    }
});

test('discards pending saved details when a database binding changes', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope, entry } = await openRosterFixture(page);
        const first = await preview.rosterAction(0);
        const row = { id: '41', name: 'Original account', enabled: true };
        await preview.roster(first.subscription, [row]);
        await status(page, /Saved · 1 users/);
        await openOfflineEditor(page, 226, 0);
        const obsolete = await preview.action(1);
        assertRequest(obsolete.request, { action: 'get_config_user', ...scope, user_id: row.id });
        entry.users_access.staff = { source: 'offline', run_id: 'new-run', database_generation: 'new-db' };
        await preview.broadcast();
        assert.equal((await preview.rosterAction(1)).action, 'users_roster_unsubscribe');
        const next = await preview.rosterAction(2);
        assert.equal(next.database_generation, 'new-db');
        await preview.roster(next.subscription, [{ ...row, enabled: false, name: 'New account' }]);
        obsolete.reply({ result: { user: row } });
        await status(page, /database changed\. Open the account again\..*1 users/);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Edit user|Selected account/);
        await click(page, 320, 226);
        await status(page, /Selected account 41 is disabled/);
    } finally {
        await page.close();
    }
});

test('sorts relative and absolute expiry using exact remaining time and saturates large durations', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview } = await openRosterFixture(page, 'live');
        const { subscription } = await preview.rosterAction(0);
        const wall = 1795000000000n;
        const accounting = 1794000000000n;
        const day = 86400000n;
        const maximum = '18446744073709551615';
        const row = (id, absolute, relative, first) => ({ id: String(id), enabled: true,
            time: { 'expire-at-ms': absolute, 'expire-after-first-usage-ms': relative, 'first-usage-at-ms': first } });
        await preview.roster(subscription, [
            row(1, '0', '0', '0'),
            row(2, String(wall + day), String(3n * day), '0'),
            row(3, '0', String(3n * day), '0'),
            row(4, String(wall - day), '0', '0'),
            row(5, null, null, null),
            row(6, String(wall + 4n * day), String(3n * day), String(accounting - day / 2n)),
            row(7, String(wall + 5n * day / 2n), String(2n * day), String(accounting + day)),
            row(8, '0', String(2n * day), String(accounting)),
            row(9, '0', String(2n * day), String(accounting + day)),
            row(10, '0', maximum, maximum),
        ], { host_time: String(wall), accounting_time: String(accounting) });
        await status(page, /Live · 10 users/);
        await click(page, rosterColumnX.daysLeft, 194);
        await status(page, /Sorted by Days Left ascending/);
        for (const [index, [id, expiry]] of [[4, 'Expired'], [2, '1 day'], [8, '2 days'], [6, '3 days'],
            [7, '3 days'], [9, '3 days'], [10, '213503982335 days'], [3, 'Awaiting first use'],
            [1, 'Unlimited'], [5, 'Unavailable']].entries()) {
            await click(page, 320, 222 + 38 * index);
            await status(page, new RegExp(`Selected account ${id} .*Days Left ${expiry}`));
        }
        await preview.roster(subscription, [row(9, '0', String(2n * day), String(accounting + day))],
            { sequence: '2', host_time: String(wall), accounting_time: null });
        await status(page, /Live · 1 users/);
        await click(page, 320, 222);
        await status(page, /Selected account 9 .*Days Left Unavailable/);
        assert.equal(preview.actions.length, 1);
    } finally {
        await page.close();
    }
});

test('recovers a missing saved file through a single fresh subscribe', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    try {
        const { preview, scope } = await openRosterFixture(page);
        const lost = await preview.rosterAction(0);
        await preview.roster(lost.subscription, [{ id: '42', name: 'Old saved account', enabled: true }]);
        await status(page, /Saved · 1 users\./);
        await preview.rosterFrame(lost.subscription, { event: 'state', state: 'lost',
            cause: 'identity', message: 'The saved file is missing.' });
        await status(page, /The saved file is missing\./);
        assert.deepEqual(await preview.rosterAction(1),
            { action: 'users_roster_unsubscribe', subscription: lost.subscription });
        await click(page, 497, 160);
        const fresh = await preview.rosterAction(2);
        assert.deepEqual(fresh, { action: 'users_roster_subscribe', ...scope, database_generation: 'staff-db',
            subscription: fresh.subscription });
        assert.notEqual(fresh.subscription, lost.subscription);
        await preview.roster(lost.subscription, [{ id: '1', enabled: true }, { id: '2', enabled: true }],
            { sequence: '2' });
        await preview.roster(fresh.subscription, [{ id: '42', name: 'Current saved account', enabled: true }]);
        await status(page, /Saved · 1 users\./);
        assert.equal(preview.rosterActions.length, 3);
    } finally {
        await page.close();
    }
});

for (const recovery of ['same binding', 'replacement binding']) {
    test(`retains the saved tab and draft during a configuration read error with ${recovery}`, async () => {
        const page = await workspace.browser.newPage({ viewport: coreDesktop });
        try {
            const { preview, entry, scope } = await openRosterFixture(page);
            const lost = await preview.rosterAction(0);
            const account = { id: '42', name: 'Original name', enabled: true };
            await preview.roster(lost.subscription, [account]);
            await status(page, /Saved · 1 users\./);
            await click(page, rosterColumnX.options, 226);
            await click(page, rosterColumnX.options + 40, 244);
            const opened = await preview.action(1);
            assertRequest(opened.request, { action: 'get_config_user', ...scope, user_id: account.id });
            opened.reply({ result: { user: account } });
            await status(page, /Edit user\. Account section\./);
            await replaceText(page, 405, 245, 'Saved document draft');

            // The other active viewer discovers the partially written config during refresh.
            const error = 'Could not read the saved configuration.';
            entry.users_access.staff.binding_error = error;
            await preview.broadcast();
            await preview.rosterFrame(lost.subscription, { event: 'state', state: 'lost',
                cause: 'config_unverified', message: error });
            await status(page, /Could not read the saved configuration.*Edit user\. Account section\./);
            assert.deepEqual(await preview.rosterAction(1),
                { action: 'users_roster_unsubscribe', subscription: lost.subscription });
            await page.keyboard.press('Enter');
            assert.equal(preview.actions.length, 2, 'The invalid graph cannot authorize a write or detail read.');
            assert.ok(await page.getByRole('tab', { name: 'staff', exact: true }).count());

            await click(page, 222, 160);
            const retry = await preview.rosterAction(2);
            assert.deepEqual(retry, { action: 'users_roster_subscribe', ...scope, database_generation: 'staff-db',
                subscription: retry.subscription });
            delete entry.users_access.staff.binding_error;
            if (recovery === 'replacement binding') {
                Object.assign(entry.users_access.staff,
                    { run_id: 'replacement-run', database_generation: 'replacement-database' });
                await preview.broadcast();
                await status(page, /node's database changed/);
                await preview.roster(retry.subscription, [account]);
                await click(page, 234, 747);
                assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Edit user/);
                assert.equal(preview.actions.length, 2);
                return;
            }
            await preview.broadcast();
            await preview.roster(retry.subscription, [account]);
            const review = await preview.action(2);
            assertRequest(review.request, { action: 'get_config_user', ...scope, user_id: account.id });
            review.reply({ result: { user: account } });
            await status(page, /Review current account details before saving/);
            const [reviewButton] = await verticalRuns(page, 206, 150, 245, [69, 64, 79], 6);
            assert.ok(reviewButton, 'The verified draft offers account review.');
            await click(page, 332, (reviewButton[0] + reviewButton[1]) / 2);
            await page.waitForFunction(() => !document.querySelector('#canvas').ariaLabel.includes(
                'Review current account details before saving.'));
            await click(page, 234, 747);
            const saved = await preview.action(3);
            assertRequest(saved.request, { action: 'edit_config_user', ...scope, user_id: account.id, roster: true,
                user: { name: 'Saved document draft' } });
            saved.reply({ result: { user_id: account.id } });
            await status(page, /Saved · 1 users\./);
            assert.equal(preview.rosterActions.length, 3);
        } finally {
            await page.close();
        }
    });
}
