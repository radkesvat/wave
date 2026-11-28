const assert = require('node:assert/strict');
const { readFile } = require('node:fs/promises');
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
    openView,
    settleCanvas,
    click,
    selectPanelTheme,
    waitForTheme,
    acknowledgeThemeSave,
    replaceText,
    assertRequest,
    verticalRuns,
    rectangleColorCount,
    rectangleContainsColor,
    expectDonationBox,
    dialogFrame,
    confirmDialog,
    graphToolbar,
    graphMenuItem,
    graphCanvasBounds,
    graphCanvasTop,
    waitForServerStatus,
} = require('./workspace_fixture.cjs');

const workspace = setupWorkspace();
const { mockPreview } = workspace;

async function saveGraphChanges(page) {
    const [top, bottom] = await graphToolbar(page);
    await click(page, 214, (top + bottom) / 2);
}

function graphCenter({ left, top, right, bottom }) {
    return { x: (left + right) / 2, y: (top + bottom) / 2 };
}

function assertNodePosition(position, { x, y }) {
    assert.ok(Math.abs(position.x - x) <= 1 && Math.abs(position.y - y) <= 1,
        `Expected node coordinates near (${x}, ${y}), got (${position.x}, ${position.y}).`);
}

async function openConfigJsonEditor(page) {
    await graphMenuItem(page, 'config', 0);
    await status(page, /Edit configuration JSON\./);
    await page.locator('#wave-json-editor .cm-content').waitFor({ timeout });
}

async function selectConfigJsonText(page) {
    const editor = page.locator('#wave-json-editor');
    const text = editor.locator('.cm-content');
    if (!await text.isVisible()) {
        await editor.getByRole('button', { name: 'text', exact: true }).click({ timeout });
    }
    await text.click({ timeout });
    await page.keyboard.press('Control+a');
}

async function pasteConfigJson(page, value) {
    await page.evaluate(value => navigator.clipboard.writeText(value), value);
    await selectConfigJsonText(page);
    await page.keyboard.press('Control+v');
    await settleCanvas(page);
    await confirmDialog(page, (x, y) => click(page, x, y), 740);
}

test('shows the footer spinner when reloading a config with unsaved changes', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'Reload fixture', nodes: [{ name: 'saved', type: 'BlackHole', settings: {} }] };
    data.server.core.configs = [{ id: 7, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const confirmReload = () => confirmDialog(page, (x, y) => click(page, x, y), 460);
    const spinner = frame => rectangleContainsColor(page, {
        x: (desktop.width - 460) / 2 + 10, y: frame.bottom - 42, width: 36, height: 34,
    }, [81, 165, 246]);
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const initial = await preview.action(0);
        assertRequest(initial.request, { action: 'get_core_config', id: 7 });
        initial.reply({ result: { config, revision: 'saved', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);

        await openConfigJsonEditor(page);
        await pasteConfigJson(page, JSON.stringify([...config.nodes, { name: 'draft', type: 'BlackHole' }]));
        await status(page, /2 nodes\..*Unsaved changes\./);
        await graphMenuItem(page, 'config', 2);
        await status(page, /Reload configuration\./);
        const idle = await dialogFrame(page);
        assert.equal(await spinner(idle), false);
        await confirmReload();
        const reload = await preview.action(1);
        assertRequest(reload.request, { action: 'get_core_config', id: 7 });
        await status(page, /Reload configuration\..*Operation in progress\./);
        assert.deepEqual(await dialogFrame(page), idle);
        assert.equal(await spinner(idle), true);
        await confirmReload();
        assert.equal(preview.actions.length, 2, 'A pending reload cannot be submitted twice.');

        reload.reply({ ok: false, feedback: 'The saved config could not be read.' });
        await status(page, /2 nodes\..*Unsaved changes\..*The saved config could not be read\./);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await graphMenuItem(page, 'config', 2);
        await status(page, /Reload configuration\./);
        await confirmReload();
        const retry = await preview.action(2);
        assertRequest(retry.request, { action: 'get_core_config', id: 7 });
        retry.reply({ result: { config, revision: 'saved', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Unsaved changes|Operation in progress/);
    } finally {
        await page.close();
    }
});

test('selecting the API starter grants live user management with a single checkbox', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture();
    const preview = await mockPreview(page, data);
    const activate = (x, y) => click(page, x, y);
    const openCreate = async name => {
        await click(page, 130, 24);
        await page.locator('#menu[aria-expanded="true"]').waitFor({ timeout });
        const dividers = await verticalRuns(page, 250, 67, 80, [61, 58, 67]);
        assert.equal(dividers.length, 1, 'A divider separates Create config from the next row.');
        await click(page, 164, 59);
        await status(page, /Create new config\. Add API and authentication database nodes unchecked\./);
        const { top } = await dialogFrame(page);
        await replaceText(page, 640, top + 69, name);
        return top;
    };
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        const name = 'Starter fixture';
        const top = await openCreate(name);
        const [checkbox] = await verticalRuns(page, 474, top + 87, top + 137, [13, 11, 14], 15);
        assert.ok(checkbox, 'Expected the unchecked starter nodes option.');
        await click(page, 474, (checkbox[0] + checkbox[1]) / 2);
        await status(page, /Add API and authentication database nodes checked\. Grants Wave all live user-management permissions for this config\./);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Allow live user management/);
        await confirmDialog(page, activate);

        const created = await preview.action(0);
        assertRequest(created.request, {
            action: 'create_core_config', name, add_auth_nodes: true, allow_live_user_management: true,
        });
        data.server.core.configs.push({
            id: 2, name, enabled: false, active: false, auth_nodes: ['AuthenticationServer'], live_users: true,
        });
        created.reply({ feedback: `Created config ${name} (disabled).`, feedback_in_logs_only: true });
        await page.getByRole('link', { name, exact: true }).waitFor({ timeout });
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        await openCreate('Empty fixture');
        await confirmDialog(page, activate);
        const empty = await preview.action(1);
        assertRequest(empty.request, { action: 'create_core_config', name: 'Empty fixture' });
        empty.reply();
    } finally {
        await page.close();
    }
});

test('clears the starter selection when another release lacks a management grant slot', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const preview = await mockPreview(page, data);
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await preview.broadcast({ schematic_revision: 1 });
        await click(page, 130, 24);
        await page.locator('#menu[aria-expanded="true"]').waitFor({ timeout });
        await click(page, 164, 59);
        await status(page, /Create new config\. Add API and authentication database nodes unchecked\./);
        const { top } = await dialogFrame(page);
        const name = 'Starterless fixture';
        await replaceText(page, 640, top + 69, name);
        const [checkbox] = await verticalRuns(page, 474, top + 87, top + 137, [13, 11, 14], 15);
        assert.ok(checkbox, 'Expected the starter checkbox.');
        await click(page, 474, (checkbox[0] + checkbox[1]) / 2);
        await status(page, /Add API and authentication database nodes checked\./);

        const schematic = structuredClone(workspace.waterwallSchematic);
        schematic.version = '1.6.1';
        delete schematic.config.users_starter.management_operations_slot;
        data.server.core.version = schematic.version;
        await preview.broadcast({ schematic, schematic_revision: 2 });
        await status(page, /Create new config\.(?! Add API and authentication database nodes)/);

        data.server.core.version = workspace.waterwallSchematic.version;
        await preview.broadcast({ schematic: workspace.waterwallSchematic, schematic_revision: 3 });
        await status(page, /Add API and authentication database nodes unchecked\./);

        await confirmDialog(page, (x, y) => click(page, x, y));
        const created = await preview.action(0);
        assertRequest(created.request, { action: 'create_core_config', name, schematic_revision: 3 });
        const id = 2;
        data.server.core.configs.push({ id, name, enabled: false, active: false });
        created.reply();
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await page.getByRole('link', { name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(1);
        assertRequest(opened.request, { action: 'get_core_config', id, schematic_revision: 3 });
        opened.reply({ result: { config: { name, nodes: [] }, revision: 'empty', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 0 nodes\./);
    } finally {
        await page.close();
    }
});

test('uses the sidebar and creates, toggles, and deletes a config on touch', async () => {
    const page = await workspace.browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
    const data = fixture();
    const preview = await mockPreview(page, data);
    let nextAction = 0;
    const tap = async (x, y) => {
        await page.touchscreen.tap(x, y);
        await settleCanvas(page);
    };
    const menuItem = async y => {
        await tap(130, 24);
        await page.locator('#menu[aria-expanded="true"]').waitFor({ timeout });
        await tap(164, y);
    };
    const configHeaderY = 279 + donationSlotHeight;
    const toggleConfig = () => tap(282, configHeaderY);
    const deleteConfig = () => tap(340, configHeaderY);
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        const dividers = await verticalRuns(page, 310, 120, 165, [61, 58, 67]);
        assert.deepEqual(dividers.map(([top, bottom]) => bottom - top), [2, 1]);
        const { width } = await page.locator('#canvas').boundingBox();
        for (const x of [11, Math.floor(width) - 12]) {
            assert.deepEqual(await verticalRuns(page, x, 120, 165, [61, 58, 67]), dividers);
        }
        assert.deepEqual(await verticalRuns(page, Math.floor(width / 2), 136, 156, [61, 58, 67]), []);
        await expectDonationBox(page, { left: 10, right: Math.floor(width) - 10, top: 212, bottom: 263,
            stickerDiameter: 36 });
        await tap(Math.floor(width) - 14, 237);
        await status(page, /^Wave About\./);

        const footerY = 180;
        await tap(60, footerY);
        await status(page, /^Wave Console\./);
        await tap(185, footerY);
        await status(page, /^Wave Settings\./);
        const rightEdge = Math.floor(width) - 12;
        await tap(rightEdge, footerY);
        await status(page, /^Wave About\./);
        const aboutRow = [footerY - 9, footerY + 7];
        assert.deepEqual(await verticalRuns(page, rightEdge, ...aboutRow, [31, 27, 34]), [aboutRow]);
        for (const x of [rightEdge - 3, rightEdge - 2]) {
            assert.deepEqual(await verticalRuns(page, x, ...aboutRow, [243, 129, 74]), [aboutRow]);
        }
        await selectPanelTheme(page, 'Light', tap);
        await acknowledgeThemeSave(preview, data, nextAction++, 'light');
        await waitForTheme(page, 'f0f2f7');
        await selectPanelTheme(page, 'Default', tap);
        await acknowledgeThemeSave(preview, data, nextAction++, 'default');
        await waitForTheme(page, '080c10');
        await tap(130, 24);
        await menuItem(59);
        await status(page, /Create new config\. Add API and authentication database nodes unchecked\./);
        await tap(80, (await dialogFrame(page)).top + 69);
        await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
        await page.keyboard.insertText('Lab');
        await settleCanvas(page);
        await confirmDialog(page, tap);
        const created = await preview.action(nextAction++);
        assertRequest(created.request, { action: 'create_core_config', name: 'Lab' });
        const config = { id: 2, name: 'Lab', enabled: false, active: false };
        data.server.core.configs.push(config);
        created.reply({ feedback: 'Created config Lab (disabled).', feedback_in_logs_only: true });
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        const labLink = page.getByRole('link', { name: 'Lab', exact: true });
        await labLink.waitFor({ timeout });
        await tap(80, 180);
        const loaded = await preview.action(nextAction++);
        assertRequest(loaded.request, { action: 'get_core_config', id: 2 });
        loaded.reply({ result: { config: { name: 'Lab', nodes: [] }, revision: 'loaded', editable: true } });
        await status(page, /^Wave Config Lab\..*0 nodes\./);
        const canvasTop = () => graphCanvasTop(page, 180, 345 + donationSlotHeight, 475 + donationSlotHeight);
        const initialCanvasTop = await canvasTop();

        const toggleStates = [true, false, true];
        for (const enabled of toggleStates) {
            await toggleConfig();
            const changed = await preview.action(nextAction++);
            assertRequest(changed.request, { action: 'set_core_config_enabled', id: 2, enabled });
            await status(page, /Operation in progress\./);
            assert.equal(await canvasTop(), initialCanvasTop, 'The graph stays in place while the toggle is pending.');
            config.enabled = enabled;
            const feedback = `${enabled ? 'Enabled' : 'Disabled'} config Lab.`;
            changed.reply({ feedback, feedback_in_logs_only: true });
            await expectLoggedOnce(page, feedback);
            assert.equal(await canvasTop(), initialCanvasTop, 'The graph stays in place after a successful toggle.');
        }

        await toggleConfig();
        const rejected = await preview.action(nextAction++);
        assertRequest(rejected.request, { action: 'set_core_config_enabled', id: 2, enabled: false });
        const rejection = 'Stop WaterWall before changing this config.';
        rejected.reply({ ok: false, feedback: rejection, feedback_in_logs_only: true });
        await expectLoggedOnce(page, rejection);
        assert.equal(await canvasTop(), initialCanvasTop, 'The rejected toggle does not move the graph.');

        await deleteConfig();
        await status(page, /Confirm deletion of Lab and its config\.json file\./);
        await tap(220, (await dialogFrame(page)).bottom - 25);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(await labLink.getAttribute('aria-current'), 'page');
        assert.equal(preview.actions.length, nextAction, 'Cancelling leaves the config in place.');

        await deleteConfig();
        await status(page, /Confirm deletion of Lab and its config\.json file\./);
        await confirmDialog(page, tap);
        const removed = await preview.action(nextAction++);
        assertRequest(removed.request, { action: 'remove_core_config', id: 2 });
        data.server.core.configs = data.server.core.configs.filter(entry => entry.id !== config.id);
        removed.reply({ feedback: 'Removed config Lab.', feedback_in_logs_only: true });
        await waitForServerStatus(page);
        await labLink.waitFor({ state: 'detached', timeout });
    } finally {
        await page.close();
    }
});

async function backupButtonsRow(page) {
    await settleCanvas(page);
    const rows = await verticalRuns(page, 510, 400, 820, [69, 64, 79], 20);
    assert.ok(rows.length, 'Expected the Backup control.');
    const [top, bottom] = rows.at(-1);
    return Math.floor((top + bottom) / 2);
}

async function chooseBackupFile(page, buffer) {
    const choosing = page.waitForEvent('filechooser', { timeout });
    await click(page, 560, (await dialogFrame(page)).top + 80);
    await (await choosing).setFiles({ name: 'wave-backup.zip', mimeType: 'application/zip', buffer });
}

test('downloads a backup ZIP, cancels uploads, and retries a busy restore with the same proposal', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const archive = Buffer.from('504b0506000000000000000000000000000000000000', 'hex');
    const uploadUrl = new URL('api/backup/prepare', workspace.url).href;
    const applyUrl = new URL('api/backup/apply', workspace.url).href;
    const applyGate = Promise.withResolvers();
    let applyRequests = 0;
    let completedUploads = 0;
    let holdUpload = true;
    const heldUploads = [];
    const preview = await mockPreview(page, data);
    await page.route(new URL('api/backup', workspace.url).href, route =>
        route.fulfill({ status: 200, contentType: 'application/zip', body: archive }));
    await page.route(uploadUrl, route => {
        if (!route.request().postDataBuffer()?.equals(archive)) {
            return route.fulfill({ status: 400, json: { ok: false, error: 'The backup ZIP is invalid.' } });
        }
        if (holdUpload) {
            heldUploads.push(route);
            return;
        }
        completedUploads++;
        return route.fulfill({ status: 200, json: {
            ok: true,
            token: 'fixture-restore-token',
            current_waterwall_version: '1.6.0',
            waterwall_version: '1.7.0',
            configs: [{ name: 'Standby', enabled: true }, { name: 'New config', enabled: false }],
            replaced_configs: ['Standby'],
            removed_configs: ['Old config'],
            missing_dependencies: ['Install WaterWall 1.7.0 before starting the core.'],
            compatibility_warnings: ['Review the retained draft before enabling it.'],
        } });
    });
    await page.route(applyUrl, async route => {
        applyRequests++;
        if (applyRequests === 1) {
            await applyGate.promise;
            return route.fulfill({ status: 409, json: {
                ok: false, retryable: true, error: 'Wait for the WaterWall release check to finish before restoring.',
            } });
        }
        await route.fulfill({ status: 200, json: { ok: true } });
        data.server.core.version = '1.7.0';
        await preview.broadcast({});
    });
    try {
        await page.goto(workspace.url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core stopped/);
        const rowY = await backupButtonsRow(page);
        const downloading = page.waitForEvent('download', { timeout });
        await click(page, 510, rowY);
        const download = await downloading;
        assert.equal(download.suggestedFilename(), 'wave-backup.zip');
        assert.deepEqual(await readFile(await download.path()), archive);
        await status(page, /Downloaded the Wave workspace backup/);

        const openRestore = async () => {
            await click(page, 675, rowY);
            await status(page, /Restore backup/);
        };
        for (const cancel of [
            async () => click(page, 670, (await dialogFrame(page)).bottom - 25),
            async () => page.keyboard.press('Escape'),
        ]) {
            await openRestore();
            const stalled = page.waitForRequest(uploadUrl, { timeout });
            await chooseBackupFile(page, archive);
            await stalled;
            await status(page, /Restore backup\.[\s\S]*Operation in progress\./);
            const aborted = page.waitForEvent('requestfailed', {
                timeout, predicate: request => request.url() === uploadUrl,
            });
            await cancel();
            await aborted;
            await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        }
        holdUpload = false;
        await openRestore();
        await chooseBackupFile(page, Buffer.from('Invalid ZIP'));
        await status(page, /The backup ZIP is invalid\./);
        const uploading = page.waitForRequest(uploadUrl, { timeout });
        await chooseBackupFile(page, archive);
        const upload = await uploading;
        assert.equal(upload.method(), 'POST');
        assert.equal(upload.headers()['content-type'], 'application/zip');
        assert.equal(upload.headers()['x-wave-csrf'], '1');
        await status(page, /WaterWall will change from 1\.6\.0 to 1\.7\.0\./);
        await status(page, /Replace Standby \(enabled\)\. Add New config \(disabled\)\. Remove Old config\./);
        await status(page, /Install WaterWall 1\.7\.0 before starting the core\./);
        await status(page, /Review the retained draft before enabling it\./);
        assert.equal(applyRequests, 0, 'Selecting a file waits for confirmation.');
        const restoreFrame = await dialogFrame(page);
        const applying = page.waitForRequest(applyUrl, { timeout });
        await confirmDialog(page, (x, y) => click(page, x, y));
        const apply = await applying;
        assert.equal(apply.method(), 'POST');
        assert.equal(apply.headers()['content-type'], 'application/json');
        assert.equal(apply.headers()['x-wave-csrf'], '1');
        assert.deepEqual(apply.postDataJSON(), { token: 'fixture-restore-token' });
        await status(page, /Restore backup\.[\s\S]*Operation in progress\./);
        assert.deepEqual(await dialogFrame(page), restoreFrame, 'Progress keeps the dialog at its original size.');
        data.server.core.checking_updates = true;
        await preview.broadcast({});
        applyGate.resolve();
        await status(page, /Wait for the WaterWall release check to finish before restoring\./);
        await status(page, /Replace Standby \(enabled\)/);
        data.server.core.checking_updates = false;
        await preview.broadcast({});
        const retrying = page.waitForRequest(applyUrl, { timeout });
        await confirmDialog(page, (x, y) => click(page, x, y));
        assert.deepEqual((await retrying).postDataJSON(), apply.postDataJSON());
        await status(page, /Core version 1\.7\.0/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(completedUploads, 1, 'Retry uses the same proposal without another upload.');
    } finally {
        applyGate.resolve();
        await Promise.all(heldUploads.map(route => route.abort().catch(() => {})));
        await page.close();
    }
});

test('reports an unconfirmed restore outcome when Apply times out', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const applyUrl = new URL('api/backup/apply', workspace.url).href;
    await mockPreview(page, data);
    await page.clock.install();
    await page.route(new URL('api/backup/prepare', workspace.url).href, route => route.fulfill({
        status: 200, json: { ok: true, token: 'fixture-restore-token', current_waterwall_version: '1.6.0',
            waterwall_version: '1.6.0', configs: [], replaced_configs: [], removed_configs: [] },
    }));
    await page.route(applyUrl, () => {});
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await click(page, 675, await backupButtonsRow(page));
        await status(page, /Restore backup/);
        await chooseBackupFile(page, Buffer.from('Backup ZIP'));
        await status(page, /This backup contains no configs/);
        const stalled = page.waitForRequest(applyUrl, { timeout });
        await confirmDialog(page, (x, y) => click(page, x, y));
        await stalled;
        await page.clock.fastForward(31_000);
        await status(page, /The restore outcome is unconfirmed\. Check the workspace before trying again\./);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Operation in progress/);
        await page.keyboard.press('Escape');
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
    } finally {
        await page.close();
    }
});

test('previews restored panel settings and opens sign-in at the restored base path', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const basePath = '/restored/wave';
    const restoredUrl = new URL(`${basePath}/`, workspace.url);
    const signInUrl = new URL('sign-in', restoredUrl);
    const applyUrl = new URL('api/backup/apply', workspace.url).href;
    await mockPreview(page, data);
    await page.route(new URL('api/backup/prepare', workspace.url).href, route => route.fulfill({
        status: 200, json: {
            ok: true, token: 'panel-restore-token', current_waterwall_version: '1.6.0', waterwall_version: '1.6.0',
            wave_version: '0.0.1-beta', panel_settings: { base_path: basePath, administrator: 'restored-admin',
                restart_required: true },
            configs: [], replaced_configs: [], removed_configs: [],
        },
    }));
    await page.route(applyUrl, route => route.fulfill({ status: 200, json: { ok: true, base_path: basePath } }));
    await page.route(restoredUrl.href, route => route.fulfill({ status: 303, headers: { location: signInUrl.href } }));
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await click(page, 675, await backupButtonsRow(page));
        await status(page, /Restore backup/);
        await chooseBackupFile(page, Buffer.from('Backup ZIP'));
        await status(page, /Created by Wave 0\.0\.1-beta\./);
        await status(page, /Panel base path \/restored\/wave\. Administrator restored-admin\./);
        await status(page, /Restoring replaces panel settings and administrator credentials\./);
        await status(page, /All active Wave sessions will be signed out\./);
        await status(page, /Wave will restart the panel to apply the restored listener and TLS settings\./);
        assert.equal(page.url(), workspace.url, 'The preview keeps the current panel open until confirmation.');
        const applying = page.waitForRequest(applyUrl, { timeout });
        const navigating = page.waitForRequest(restoredUrl.href, { timeout });
        await confirmDialog(page, (x, y) => click(page, x, y));
        assert.deepEqual((await applying).postDataJSON(), { token: 'panel-restore-token' });
        assert.ok((await navigating).isNavigationRequest());
        await page.waitForURL(signInUrl.href, { timeout });
    } finally {
        await page.close();
    }
});

test('waits for the restored panel listener and opens its new address after Apply', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const preview = await mockPreview(page, fixture('stopped'));
    const basePath = '/restored/wave';
    const restart = { protocol: 'https:', hostname: 'restored.example.test', port: 2443 };
    const target = `https://${restart.hostname}:${restart.port}${basePath}/`;
    const applyGate = Promise.withResolvers();
    const probeGate = Promise.withResolvers();
    const applyUrl = new URL('api/backup/apply', workspace.url).href;
    await page.route(new URL('api/backup/prepare', workspace.url).href, route => route.fulfill({
        status: 200, json: {
            ok: true, token: 'panel-restore-token', current_waterwall_version: '1.6.0', waterwall_version: '1.6.0',
            wave_version: '0.0.1-beta', panel_settings: { base_path: basePath, administrator: 'restored-admin',
                restart_required: true },
            configs: [], replaced_configs: [], removed_configs: [],
        },
    }));
    await page.route(applyUrl, async route => {
        await applyGate.promise;
        await route.fulfill({ status: 200, json: { ok: true, base_path: basePath, restart } });
    });
    await page.route(target + 'sign-in', async route => {
        await probeGate.promise;
        await route.fulfill({ status: 200 });
    });
    await page.route(target, route => route.fulfill({ contentType: 'text/html', body: '<title>Restored panel</title>' }));
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await click(page, 675, await backupButtonsRow(page));
        await status(page, /Restore backup/);
        await chooseBackupFile(page, Buffer.from('Backup ZIP'));
        await status(page, /Wave will restart the panel/);
        const applying = page.waitForRequest(applyUrl, { timeout });
        await confirmDialog(page, (x, y) => click(page, x, y));
        await applying;
        preview.notifyBasePath(basePath);
        await preview.disconnectBoth();
        await settleCanvas(page);
        assert.equal(page.url(), workspace.url, 'The applying tab waits for the restored listener address.');
        assert.ok(await page.evaluate(() => Module.backupRequestPending('apply')),
            'Revoking the previous session does not interrupt Apply.');

        const probing = page.waitForRequest(target + 'sign-in', { timeout });
        applyGate.resolve();
        assert.equal((await probing).isNavigationRequest(), false);
        const transition = page.locator('#startup');
        await transition.waitFor({ timeout });
        assert.equal(await transition.textContent(), 'Restarting Wave…');
        assert.equal(page.url(), workspace.url, 'The browser waits for the restored listener to respond.');
        probeGate.resolve();
        await page.waitForURL(target, { timeout });
        assert.equal(await page.title(), 'Restored panel');
    } finally {
        applyGate.resolve();
        probeGate.resolve();
        await page.close();
    }
});

test('restores an open config by name and returns to Server Status when its ID is reused', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const profile = data.server.core.configs[0];
    profile.active = false;
    const preview = await mockPreview(page, data);
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await preview.broadcast({ schematic_revision: 1 });
        await settleCanvas(page);
        await page.getByRole('link', { name: profile.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const beforeRestore = await preview.action(0);
        assertRequest(beforeRestore.request, { action: 'get_core_config', id: profile.id, schematic_revision: 1 });
        const config = { name: profile.name, nodes: [] };
        beforeRestore.reply({ result: { config, revision: 'original', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 0 nodes\./);

        const newId = profile.id + 1;
        data.server.core.configs = [
            { ...profile, name: 'Reused ID' },
            { ...profile, id: newId },
        ];
        await preview.broadcast({ schematic_revision: 2, schematic: workspace.waterwallSchematic });
        const refreshed = await preview.action(1);
        assertRequest(refreshed.request, { action: 'get_core_config', id: newId, schematic_revision: 2 });
        const nodes = [{ name: 'restored', type: 'BlackHole', settings: {} }];
        refreshed.reply({ result: { config: { ...config, nodes }, revision: 'restored', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);

        data.server.core.configs = [{ ...profile, id: newId, name: 'Different config' }];
        await preview.broadcast({ schematic_revision: 3, schematic: workspace.waterwallSchematic });
        await waitForServerStatus(page);
        assert.equal(preview.actions.length, 2, 'The unrelated config reusing the selected ID stays closed.');
    } finally {
        await page.close();
    }
});

test('requires a WaterWall schematic before opening the node editor', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const config = { id: 7, name: 'No schematic', enabled: false, active: false };
    data.server.core.configs = [config];
    const preview = await mockPreview(page, data, Promise.resolve(), null);
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        await status(page, /The WaterWall schematic is unavailable\./);
        assert.equal(preview.actions.length, 0, 'The config is not opened without a schematic.');
    } finally {
        await page.close();
    }
});

test('renders new node types and settings from the workspace schematic', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const config = { name: 'Schematic fixture', nodes: [] };
    data.server.core.configs = [{ id: 7, name: config.name, enabled: false, active: false }];
    const schematic = structuredClone(workspace.waterwallSchematic);
    schematic.version = '1.6.1';
    const type = 'AAFixtureNode';
    const field = { key: 'recipe', label: 'Recipe', kind: 'text', hint: 'Chosen by the active schematic.' };
    schematic.nodes[type] = {
        ...schematic.nodes.BlackHole,
        ports: { previous: true, next: false, dual_ended: false },
        fields: [field],
    };
    const preview = await mockPreview(page, data, Promise.resolve(), schematic);
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 7 });
        loaded.reply({ result: { config, revision: 'initial', editable: true } });
        await status(page, /Schematic fixture\. Nodes tab\. Nodes\. 0 nodes\./);

        await graphMenuItem(page, 'nodes', 0);
        await status(page, /Add node\./);
        const { top } = await dialogFrame(page);
        await replaceText(page, 640, top + 72, 'alternate');
        await click(page, 893, top + 128);
        await click(page, 420, top + 154);
        const form = await dialogFrame(page);
        const [input] = await verticalRuns(page, 820, form.top + 320, form.bottom - 45, [13, 11, 14], 18);
        assert.ok(input, 'The schematic supplies an editable setting for the new node type.');
        await page.mouse.move(430, input[0] - 13);
        await status(page, /recipe\s+Chosen by the active schematic\./);
        await replaceText(page, 640, (input[0] + input[1]) / 2, 'fixture value');
        await confirmDialog(page, (x, y) => click(page, x, y), 560);
        await status(page, /Selected alternate\. Type AAFixtureNode\./);
        await saveGraphChanges(page);
        const saved = await preview.action(1);
        assert.equal(saved.request.revision, 'initial');
        assert.deepEqual(saved.request.config.nodes.map(({ name, type, settings }) => ({ name, type, settings })),
            [{ name: 'alternate', type, settings: { recipe: 'fixture value' } }]);
    } finally {
        await page.close();
    }
});

test('edits a WaterWall node graph and reopens the saved JSON', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = {
        name: 'Lab graph', author: 'Wave fixture', 'config-version': 1,
        nodes: [{ name: 'inlet', type: 'TcpListener', settings: { address: '127.0.0.1', port: 2046 } }],
    };
    data.server.core.configs = [{ id: 7, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const activate = (x, y) => click(page, x, y);
    const menuItem = (menu, row) => graphMenuItem(page, menu, row);
    const graphTop = () => graphCanvasTop(page);
    const nodeSpan = async () => {
        const top = await graphTop();
        const [node] = await verticalRuns(page, 345, top + 2, top + 180, [43, 39, 48], 20);
        assert.ok(node, 'Expected the inlet node card.');
        return node;
    };
    const nodeHeight = ([top, bottom]) => bottom - top;
    const wheel = async (x, y, delta) => {
        await page.mouse.move(x, y);
        await page.mouse.wheel(0, delta);
        await settleCanvas(page);
    };
    const paste = value => pasteConfigJson(page, value);
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 7 });
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Wave Config Lab graph. Nodes tab. Nodes. 1 node\./);

        const canvas = await graphCanvasBounds(page);
        assert.equal(canvas.left, 189, 'The graph starts immediately inside the left pane border.');
        assert.equal(canvas.right, desktop.width - 11, 'The graph reaches the right pane border.');
        assert.equal(canvas.bottom, desktop.height - 221, 'The graph reaches the bottom pane border.');
        const [toolbarTop] = await graphToolbar(page);
        for (const x of [canvas.left + 1, canvas.right - 2]) {
            assert.ok(await rectangleContainsColor(page, { x, y: toolbarTop + 3, width: 1, height: 5 },
                [26, 23, 28]), 'The toolbar reaches both pane edges.');
            assert.ok(await rectangleContainsColor(page, { x, y: canvas.top + 40, width: 1, height: 56 },
                [61, 58, 67]), 'The grid reaches both pane edges.');
        }
        assert.ok(await rectangleContainsColor(page, {
            x: 600, y: canvas.bottom - 2, width: 28, height: 1,
        }, [61, 58, 67]), 'The grid reaches the pane bottom.');
        for (const x of [canvas.left, canvas.right - 2]) {
            assert.equal(await rectangleColorCount(page, { x, y: canvas.bottom - 1, width: 2, height: 2 },
                [8, 12, 16]), 4, 'The grid and outline curve away from the pane corners.');
        }

        await saveGraphChanges(page);
        assert.equal(preview.actions.length, 1, 'Save stays idle while the graph is clean.');
        assert.ok(await graphTop() < 195, 'The compact menu leaves room for the graph.');
        const originalNode = await nodeSpan();
        const originalHeight = nodeHeight(originalNode);
        const wheelY = Math.floor(originalNode[0] + originalHeight / 2);
        await wheel(345, wheelY, -75);
        const zoomedNode = await nodeSpan();
        assert.ok(zoomedNode[0] < originalNode[0] && zoomedNode[1] > originalNode[1],
            'Wheeling up enlarges the node around the cursor.');
        assert.ok(Math.abs((zoomedNode[0] + zoomedNode[1]) / 2 - wheelY) <= 1,
            'The node midpoint stays under the cursor.');
        await wheel(345, wheelY, 75);
        assert.deepEqual(await nodeSpan(), originalNode, 'Wheeling down restores the previous zoom.');
        await wheel(600, 30, -75);
        assert.deepEqual(await nodeSpan(), originalNode, 'Wheeling outside the graph leaves it unchanged.');
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Unsaved changes/);

        await menuItem('view', 0);
        assert.ok(nodeHeight(await nodeSpan()) > originalHeight, 'The View menu zooms the canvas.');
        await menuItem('view', 1);
        assert.equal(nodeHeight(await nodeSpan()), originalHeight);
        assert.equal(preview.actions.length, 1, 'Zooming does not send configuration changes.');

        const center = graphCenter(canvas);
        await menuItem('nodes', 0);
        await status(page, /Add node\./);
        const { top: addTop } = await dialogFrame(page);
        await replaceText(page, 640, addTop + 72, 'outlet');
        await click(page, 893, addTop + 128);
        await click(page, 420, addTop + 275);
        await replaceText(page, 640, (await dialogFrame(page)).top + 216, '{"mode":"passive"}');
        await status(page, /Add node\..*Custom JSON enabled\./);
        await confirmDialog(page, (x, y) => click(page, x, y), 560);
        await status(page, /2 nodes. Selected outlet. Type BlackHole/);

        await page.mouse.dblclick(center.x, center.y, { delay: 40 });
        await settleCanvas(page);
        await status(page, /Edit node\./);
        await status(page, /Edit node\..*Custom JSON enabled\./);
        const { top: editTop } = await dialogFrame(page);
        await replaceText(page, 640, editTop + 72, 'discard');
        await replaceText(page, 640, editTop + 216, '{"port":1,"port":2}');
        await confirmDialog(page, (x, y) => click(page, x, y), 560);
        await status(page, /Edit node\..*duplicate JSON object key\./);
        await replaceText(page, 640, (await dialogFrame(page)).top + 216, '{"value":"\\u0000"}');
        await confirmDialog(page, (x, y) => click(page, x, y), 560);
        await status(page, /Edit node\..*cannot contain a NUL character\./);
        await replaceText(page, 640, (await dialogFrame(page)).top + 216, '{"mode":"active"}');
        await confirmDialog(page, (x, y) => click(page, x, y), 560);
        await status(page, /Selected discard. Type BlackHole/);

        const canvasTop = await graphTop();
        await page.mouse.move(300, canvasTop + 55);
        await page.mouse.down();
        await settleCanvas(page);
        await page.mouse.move(364, canvasTop + 93);
        await settleCanvas(page);
        await page.mouse.up();
        await settleCanvas(page);
        await status(page, /Selected inlet. Type TcpListener/);
        await click(page, canvas.left + 26 + 148 + 64, canvasTop + 93);
        await click(page, center.x, center.y);
        await status(page, /Selected inlet. Type TcpListener. Next discard\./);

        await openConfigJsonEditor(page);
        await page.evaluate(() => navigator.clipboard.writeText(''));
        await selectConfigJsonText(page);
        await page.keyboard.press('Control+c');
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0);
        const draft = JSON.parse(await page.evaluate(() => navigator.clipboard.readText()));
        const [inlet, discard] = draft.nodes;
        assert.ok(inlet['panel-settings'].x > 26 && inlet['panel-settings'].y > 26,
            'The JSON editor includes unsaved canvas coordinates.');
        const { x, y } = discard['panel-settings'];
        assertNodePosition({ x, y }, { x: center.x - canvas.left - 74, y: center.y - canvas.top - 29 });
        assert.deepEqual(draft, {
            ...config,
            nodes: [
                { ...config.nodes[0], next: 'discard', 'panel-settings': inlet['panel-settings'] },
                { name: 'discard', type: 'BlackHole', settings: { mode: 'active' },
                    'panel-settings': { x, y, 'custom-json': true } },
            ],
        });

        await paste('{');
        await status(page, /2 nodes\. Selected inlet\. Type TcpListener\. Next discard\..*Enter valid JSON\./);
        await paste('{"name":"Missing nodes"}');
        await status(page, /2 nodes\. Selected inlet\. Type TcpListener\. Next discard\..*with a nodes array\./);
        await paste(JSON.stringify([
            { ...inlet, next: 'middle' }, discard, { name: 'middle', type: 'LoggerTunnel', next: 'discard' },
        ], null, 2));
        await status(page, /3 nodes\..*Unsaved changes\./);

        const authNode = {
            name: 'accounts', type: 'AuthenticationServer',
            settings: { 'file-save-rate-ms': 15000 }, 'panel-settings': { x: 240, y: 180 },
        };
        const savedInlet = { ...inlet, next: authNode.name, settings: { ...inlet.settings, port: 2047 } };
        const pastedAuth = { ...authNode, settings: { ...authNode.settings, 'db-path': '../other/users.json' } };
        const branchNodes = target => [
            { name: target, type: 'HalfDuplexServer', next: 'accounts', 'panel-settings': { x: 700, y: 50 } },
            { name: 'downlink', type: 'BlackHole', 'panel-settings': { x: 700, y: 145 } },
            {
                name: 'splitter', type: 'PacketSplitStream', settings: { up: target, down: 'downlink' },
                'panel-settings': { x: 300, y: 24 },
            },
            {
                name: 'split client', type: 'HalfDuplexClient', settings: { upload: target, download: 'downlink' },
                'panel-settings': { x: 300, y: 112 },
            },
            {
                name: 'paired server', type: 'HalfDuplexServer', settings: { pair: target },
                'panel-settings': { x: 510, y: 155 },
            },
            {
                name: 'router', type: 'Router', next: 'accounts',
                settings: { rules: [{ 'destination-port': 8443, target }] }, 'panel-settings': { x: 300, y: 236 },
            },
            {
                name: 'sniffer', type: 'SniffRouter', next: 'accounts', settings: { routes: [
                    { domain: 'first.example.invalid', next: target },
                    { domain: 'legacy.example.invalid', target },
                ] }, 'panel-settings': { x: 300, y: 324 },
            },
        ];
        await openConfigJsonEditor(page);
        await paste(JSON.stringify({
            name: 'Different profile', author: 'Imported', 'config-version': 8,
            nodes: [savedInlet, pastedAuth, ...branchNodes('uplink')],
        }, null, 2));
        await status(page, /9 nodes\..*Unsaved changes\./);
        assert.equal(preview.actions.length, 1, 'Applying JSON changes only the draft.');

        const branchTop = await graphTop();
        await click(page, 530, branchTop + 52);
        await status(page, /Selected splitter\. Type PacketSplitStream\..*Up uplink\..*Down downlink\./);
        const branchArrow = await verticalRuns(page, canvas.left + 695, branchTop + 145, branchTop + 210,
            [255, 195, 106]);
        assert.ok(branchArrow.length, 'The graph draws a split branch into the downlink.');

        await click(page, 930, branchTop + 76);
        await status(page, /Selected uplink\. Type HalfDuplexServer\./);
        await menuItem('nodes', 1);
        await status(page, /Edit node\./);
        await replaceText(page, 640, (await dialogFrame(page)).top + 72, 'upstream');
        await confirmDialog(page, activate, 560);
        await status(page, /Selected upstream\. Type HalfDuplexServer\./);
        assert.equal(preview.actions.length, 1, 'Renaming updates references in the draft.');

        await saveGraphChanges(page);
        const saved = await preview.action(1);
        const nodes = [savedInlet, authNode, ...branchNodes('upstream')];
        assertRequest(saved.request, { action: 'save_core_config', id: 7, revision: 'loaded', config: { ...config, nodes } });
        const document = {
            ...config,
            nodes: [savedInlet, {
                ...authNode,
                settings: { ...authNode.settings, 'db-path': 'configs/Lab graph/auth-0123456789abcdef0123456789abcdef.json' },
            }, ...branchNodes('upstream')],
        };
        saved.reply({ feedback: 'Saved Lab graph.', feedback_in_logs_only: true,
            result: { config: document, revision: 'saved', editable: true } });
        await status(page, /Latest log: Saved Lab graph\./);
        await saveGraphChanges(page);
        assert.equal(preview.actions.length, 2, 'Save becomes idle again after acknowledgement.');

        await menuItem('config', 1);
        await status(page, /Saved configuration JSON\./);
        const viewer = page.locator('#wave-json-editor');
        const text = viewer.locator('.cm-content');
        const level2 = viewer.getByRole('button', { name: 'Level 2', exact: true });
        const level3 = viewer.getByRole('button', { name: 'Level 3', exact: true });
        const level4 = viewer.getByRole('button', { name: 'Level 4', exact: true });
        const nodesLine = text.locator('.cm-line').filter({ hasText: /^\s*"nodes": \[/ });
        const nodesFold = nodesLine.locator('.cm-foldPlaceholder');
        const inletLine = text.locator('.cm-line').filter({ hasText: /^\s*"name": "inlet"/ });
        const settingsFold = text.locator('.cm-line:has(.cm-foldPlaceholder)')
            .filter({ hasText: /^\s*"settings": \{/ }).first();
        await text.waitFor({ timeout });
        const foldButtons = await Promise.all([level2, level3, level4].map(button => button.boundingBox({ timeout })));
        assert.ok(foldButtons.every(box => box && box.width >= 60), 'Fold buttons have wider click targets.');
        for (let index = 1; index < foldButtons.length; index++) {
            const previous = foldButtons[index - 1];
            assert.ok(foldButtons[index].x - (previous.x + previous.width) >= 6,
                'Adjacent fold buttons leave at least six pixels between them.');
        }
        await level2.click({ timeout });
        await nodesFold.waitFor({ timeout });
        await level3.click({ timeout });
        await nodesLine.waitFor({ timeout });
        await nodesFold.waitFor({ state: 'hidden', timeout });
        await text.locator('.cm-line:has(.cm-foldPlaceholder)')
            .filter({ hasText: /^\s*\{/ }).first().waitFor({ timeout });
        await inletLine.waitFor({ state: 'hidden', timeout });
        await level4.click({ timeout });
        await inletLine.waitFor({ timeout });
        await settingsFold.waitFor({ timeout });
        await level2.click({ timeout });
        await nodesFold.waitFor({ timeout });

        await page.evaluate(() => navigator.clipboard.writeText(''));
        await confirmDialog(page, activate, 740);
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0);
        assert.deepEqual(JSON.parse(await page.evaluate(() => navigator.clipboard.readText())), document);

        await viewer.getByRole('button', { name: 'tree', exact: true }).click({ timeout });
        const inletValue = viewer.locator('.jse-value').getByText('inlet', { exact: true });
        const nodesArray = viewer.locator('.jse-json-node[data-path="%2Fnodes"]');
        const firstNode = viewer.locator('.jse-json-node[data-path="%2Fnodes%2F0"]');
        const firstNodeSettings = viewer.locator('.jse-json-node[data-path="%2Fnodes%2F0%2Fsettings"]');
        await level2.click({ timeout });
        await nodesArray.waitFor({ timeout });
        assert.doesNotMatch(await nodesArray.getAttribute('class'), /\bjse-expanded\b/);
        await level3.click({ timeout });
        await firstNode.waitFor({ timeout });
        assert.match(await nodesArray.getAttribute('class'), /\bjse-expanded\b/);
        assert.doesNotMatch(await firstNode.getAttribute('class'), /\bjse-expanded\b/);
        await level4.click({ timeout });
        await firstNodeSettings.waitFor({ timeout });
        assert.match(await firstNode.getAttribute('class'), /\bjse-expanded\b/);
        assert.doesNotMatch(await firstNodeSettings.getAttribute('class'), /\bjse-expanded\b/);
        await level2.click({ timeout });
        await firstNode.waitFor({ state: 'hidden', timeout });
        await viewer.getByTitle('Expand all', { exact: true }).click({ timeout });
        await inletValue.dblclick({ timeout });
        assert.equal(await viewer.getByRole('textbox', { name: 'Edit value', exact: true }).count(), 0,
            'Tree mode displays saved values without allowing edits.');

        await page.evaluate(() => navigator.clipboard.writeText(''));
        await selectConfigJsonText(page);
        assert.equal(await viewer.locator('.cm-content').isEditable(), false, 'The saved text is read-only.');
        await page.keyboard.press('Control+c');
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0);
        assert.deepEqual(JSON.parse(await page.evaluate(() => navigator.clipboard.readText())), document,
            'Text mode makes the full saved JSON selectable.');
    } finally {
        await page.close();
    }
});

test('edits config values in the JSON tree without rounding large integers', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'JSON editor fixture', nodes: [{ name: 'before', type: 'BlackHole' }] };
    data.server.core.configs = [{ id: 7, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const editor = page.locator('#wave-json-editor');
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);

        await openConfigJsonEditor(page);
        await page.evaluate(() => navigator.clipboard.writeText(
            '[{"name":"before","type":"BlackHole","settings":{"bytes":18446744073709551615}}]'));
        await selectConfigJsonText(page);
        await page.keyboard.press('Control+v');
        await editor.locator('.cm-content').getByText('18446744073709551615', { exact: true }).waitFor({ timeout });
        await editor.getByRole('button', { name: 'tree', exact: true }).click({ timeout });
        await editor.locator('.jse-value').getByText('before', { exact: true }).dblclick({ timeout });
        const value = editor.getByRole('textbox', { name: 'Edit value', exact: true });
        await value.fill('after', { timeout });
        await value.press('Enter', { timeout });
        await editor.getByText('after', { exact: true }).waitFor({ timeout });
        await confirmDialog(page, (x, y) => click(page, x, y), 740);
        await editor.waitFor({ state: 'hidden', timeout });
        await status(page, /1 node\..*Unsaved changes\./);
        assert.equal(preview.actions.length, 1, 'Applying the tree edit updates the draft.');

        await saveGraphChanges(page);
        const saved = await preview.action(1);
        assert.equal(saved.request.action, 'save_core_config');
        assert.equal(saved.request.config.nodes[0].name, 'after');
        assert.match(saved.raw, /"bytes":18446744073709551615[,}]/,
            'The original unsigned 64-bit integer survives Text to Tree, Apply nodes, and Save.');
    } finally {
        await page.close();
    }
});

test('uses the graph context menu and centers new nodes after panning and zooming', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const seed = { name: 'seed', type: 'BlackHole', 'panel-settings': { x: 60, y: 50 } };
    const config = { name: 'Menu fixture', nodes: [seed] };
    data.server.core.configs = [{ id: 17, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const activate = (x, y) => click(page, x, y);
    const empty = { x: 520, y: 320 };
    const openContext = async ({ x, y }) => {
        await page.mouse.click(x, y, { button: 'right', delay: 40 });
        await settleCanvas(page);
    };
    const contextItem = ({ x, y }, item) => {
        const row = { add: 18, save: 41, fit: 64, edit: 93, remove: 116 }[item];
        return click(page, x + 36, y + row);
    };
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 17 });
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Wave Config Menu fixture\. Nodes tab\. Nodes\. 1 node\./);
        const canvas = await graphCanvasBounds(page);
        const center = graphCenter(canvas);
        await page.evaluate(() => {
            document.addEventListener('contextmenu', event => {
                document.getElementById('canvas').dataset.contextMenuPrevented = String(event.defaultPrevented);
            }, { once: true });
        });
        await openContext(empty);
        assert.equal(await page.locator('#canvas').getAttribute('data-context-menu-prevented'), 'true');
        await contextItem(empty, 'save');
        assert.ok(await rectangleContainsColor(page, {
            x: empty.x + 70, y: empty.y + 37, width: 5, height: 8,
        }, [43, 39, 48]), 'Clicking disabled Save leaves the menu open.');
        assert.equal(await rectangleContainsColor(page, {
            x: empty.x + 70, y: empty.y + 88, width: 5, height: 30,
        }, [43, 39, 48]), false, 'The menu on empty space has no node actions.');
        assert.equal(preview.actions.length, 1, 'A clean graph sends no save request.');
        await page.keyboard.press('Escape');
        await settleCanvas(page);

        const pan = { x: 105, y: 40 };
        await page.mouse.move(empty.x, empty.y);
        await page.mouse.down();
        await settleCanvas(page);
        await page.mouse.move(empty.x + pan.x, empty.y + pan.y);
        await settleCanvas(page);
        await page.mouse.up();
        const anchor = { x: 550, y: 300 };
        const scale = 1.25;
        await page.mouse.move(anchor.x, anchor.y);
        await page.mouse.wheel(0, -75);
        await settleCanvas(page);

        await openContext(empty);
        await contextItem(empty, 'add');
        await status(page, /Add node\./);
        const { top: addTop } = await dialogFrame(page);
        await replaceText(page, 640, addTop + 72, 'new node');
        await click(page, 893, addTop + 128);
        await click(page, 420, addTop + 275);
        await confirmDialog(page, activate, 560);
        await status(page, /2 nodes\. Selected new node\. Type BlackHole\./);
        const [card] = await verticalRuns(page, center.x + 35, center.y - 55, center.y + 55, [43, 39, 48], 60);
        assert.ok(card && Math.abs((card[0] + card[1]) / 2 - center.y) <= 1, 'The new card is centered on screen.');

        const seedCenter = {
            x: anchor.x + (canvas.left + pan.x + seed['panel-settings'].x + 74 - anchor.x) * scale,
            y: anchor.y + (canvas.top + pan.y + seed['panel-settings'].y + 29 - anchor.y) * scale,
        };
        await click(page, seedCenter.x, seedCenter.y);
        await status(page, /Selected seed\./);
        await openContext(center);
        await contextItem(center, 'edit');
        await status(page, /Selected new node\. Type BlackHole\. Edit node\./);
        await replaceText(page, 640, (await dialogFrame(page)).top + 72, 'centered');
        await confirmDialog(page, activate, 560);
        await status(page, /Selected centered\. Type BlackHole\./);

        await openContext(empty);
        await contextItem(empty, 'save');
        const saved = await preview.action(1);
        const position = saved.request.config.nodes[1]['panel-settings'];
        assertNodePosition(position, {
            x: (center.x - anchor.x) / scale + anchor.x - canvas.left - pan.x - 74,
            y: (center.y - anchor.y) / scale + anchor.y - canvas.top - pan.y - 29,
        });
        const savedConfig = { ...config, nodes: [seed, { name: 'centered', type: 'BlackHole', settings: {},
            'panel-settings': position }] };
        assertRequest(saved.request, { action: 'save_core_config', id: 17, revision: 'loaded', config: savedConfig });
        saved.reply({ result: { config: savedConfig, revision: 'saved', editable: true } });
        await page.waitForFunction(() => !document.getElementById('canvas').ariaLabel.includes('Unsaved changes'));

        await openContext(empty);
        await contextItem(empty, 'fit');
        const fittedSeed = { x: canvas.left + 24 + 74, y: canvas.top + 24 + 29 };
        await click(page, fittedSeed.x, fittedSeed.y);
        await status(page, /Selected seed\./);
        const fittedNode = {
            x: fittedSeed.x + position.x - seed['panel-settings'].x,
            y: fittedSeed.y + position.y - seed['panel-settings'].y,
        };
        await openContext(fittedNode);
        await contextItem(fittedNode, 'remove');
        await status(page, /Selected centered\. Type BlackHole\. Delete node\./);
        await confirmDialog(page, activate, 460);
        await status(page, /1 node\. Unsaved changes\./);
        assert.equal(preview.actions.length, 2, 'Fit and Delete send no request.');
    } finally {
        await page.close();
    }
});

test('centers a new node in the visible compact graph after the draft status appears', async () => {
    const viewport = { ...portrait, width: 320 };
    const page = await workspace.browser.newPage({ viewport });
    const data = fixture('stopped');
    const config = { name: 'Compact graph', nodes: [] };
    data.server.core.configs = [{ id: 18, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const graphLeft = 11;
    const graphTop = async () => {
        // The header menu can still block hit testing while the sidebar link has focus.
        const [grid] = await verticalRuns(page, graphLeft, 320 + donationSlotHeight, 430 + donationSlotHeight,
            [61, 58, 67], 18);
        assert.ok(grid, 'Expected the grid at the left edge of the compact pane.');
        return grid[0];
    };
    try {
        await page.goto(workspace.url);
        await page.locator('#menu').focus();
        await page.keyboard.press('Enter');
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 18 });
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Wave Config Compact graph\. Nodes tab\. Nodes\. 0 nodes\./);
        await page.keyboard.press('Escape');
        await settleCanvas(page);
        const cleanTop = await graphTop();

        await page.mouse.click(100, 472 + donationSlotHeight, { button: 'right', delay: 40 });
        await settleCanvas(page);
        await click(page, 132, 490 + donationSlotHeight);
        await status(page, /Add node\./);
        const { top: addTop } = await dialogFrame(page, 0);
        await replaceText(page, 100, addTop + 71, 'Compact center');
        await click(page, 280, addTop + 126);
        await click(page, 75, addTop + 275);
        await confirmDialog(page, (x, y) => click(page, x, y), 560, 0);
        await status(page, /Selected Compact center\. Type BlackHole\. Unsaved changes\./);
        await settleCanvas(page);

        const top = await graphTop();
        assert.ok(top > cleanTop, 'The draft status appears above the graph.');
        const centerY = (top + viewport.height) / 2;
        const [card] = await verticalRuns(page, 205, Math.floor(centerY) - 55, Math.ceil(centerY) + 55,
            [43, 39, 48], 40);
        assert.ok(card, 'Expected the new node card within the visible graph.');
        const cardCenterY = (card[0] + card[1]) / 2;
        assert.ok(Math.abs(cardCenterY - centerY) <= 1,
            `Expected the card center at y=${centerY}, got y=${cardCenterY}.`);

        const [toolbarTop, toolbarBottom] = await graphToolbar(page, 300, 285 + donationSlotHeight,
            375 + donationSlotHeight);
        await click(page, graphLeft + 25, (toolbarTop + toolbarBottom) / 2);
        const saved = await preview.action(1);
        const position = saved.request.config.nodes[0]['panel-settings'];
        assertNodePosition(position, { x: viewport.width / 2 - graphLeft - 74, y: centerY - top - 29 });
        assertRequest(saved.request, { action: 'save_core_config', id: 18, revision: 'loaded', config: {
            ...config, nodes: [{ name: 'Compact center', type: 'BlackHole', settings: {}, 'panel-settings': position }],
        } });
    } finally {
        await page.close();
    }
});

test('preserves imported settings during form edits and locks both kinds of controls in Custom JSON', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'Resolver fixture', nodes: [] };
    const resolver = {
        name: 'resolver', type: 'DomainResolver', next: 'sink',
        settings: {
            mode: 'chain', strategy: 'prefer-ipv4', servers: ['192.0.2.53'], 'dns-next': 'sink',
            'imported-extra': { values: [true, { marker: 'retained' }] },
        },
        'panel-settings': { x: 28, y: 42, 'imported-metadata': 7 },
        'pannel-settinsg': { marker: 'preserved as imported' },
    };
    const nodes = [
        resolver,
        { name: 'sink', type: 'BlackHole', settings: { mode: 'passive' } },
        { name: 'other', type: 'BlackHole', settings: { mode: 'active' } },
    ];
    data.server.core.configs = [{ id: 10, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const activate = (x, y) => click(page, x, y);
    const openResolver = async () => {
        await click(page, 270, (await graphCanvasTop(page)) + 70);
        await status(page, /Selected resolver\. Type DomainResolver\./);
        await graphMenuItem(page, 'nodes', 1);
        await status(page, /Edit node\./);
        return dialogFrame(page, 0);
    };
    const rawY = top => top + 218;
    const readSettings = async top => {
        await page.evaluate(() => navigator.clipboard.writeText(''));
        await click(page, 800, rawY(top));
        await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
        await page.keyboard.press('Control+a');
        await settleCanvas(page);
        await page.keyboard.press('Control+c');
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0);
        return JSON.parse(await page.evaluate(() => navigator.clipboard.readText()));
    };
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 10 });
        loaded.reply({ result: { config, revision: 'before-form', editable: true } });
        await status(page, /Wave Config Resolver fixture\. Nodes tab\. Nodes\. 0 nodes\./);
        await openConfigJsonEditor(page);
        await pasteConfigJson(page, JSON.stringify(nodes));
        await status(page, /3 nodes\..*Unsaved changes\./);

        const { top, bottom } = await openResolver();
        const rows = await verticalRuns(page, 820, top + 325, bottom - 45, [13, 11, 14], 18);
        assert.ok(rows.length >= 5, 'Expected the DNS strategy and branch dropdowns.');
        const strategy = rows[3];
        const branch = rows[4];
        await click(page, 855, (strategy[0] + strategy[1]) / 2);
        await click(page, 435, strategy[1] + 109);
        const formSettings = { ...resolver.settings, strategy: 'prefer-ipv6' };
        assert.deepEqual(await readSettings(top), formSettings);
        await confirmDialog(page, activate, 560, 0);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        const { top: customTop } = await openResolver();
        assert.deepEqual(await readSettings(customTop), formSettings);
        const customSettings = { ...formSettings, strategy: 'only-ipv6', 'dns-next': 'other' };
        await replaceText(page, 640, rawY(customTop), JSON.stringify(customSettings));
        await status(page, /Edit node\..*Custom JSON enabled\./);
        for (const [row, optionOffset] of [[strategy, 109], [branch, 19]]) {
            await click(page, 855, customTop + (row[0] + row[1]) / 2 - top);
            await click(page, 435, customTop + row[1] + optionOffset - top);
        }
        assert.deepEqual(await readSettings(customTop), customSettings);
        await confirmDialog(page, activate, 560, 0);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        await saveGraphChanges(page);
        const saved = await preview.action(1);
        assertRequest(saved.request, { action: 'save_core_config', id: 10, revision: 'before-form', config: {
            ...config, nodes: [{
                ...resolver, settings: customSettings,
                'panel-settings': { ...resolver['panel-settings'], 'custom-json': true },
            }, ...nodes.slice(1)],
        } });
    } finally {
        await page.close();
    }
});

test('reviews a WaterWall import with variables or comments before saving', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = {
        name: 'Import fixture', author: 'Saved fixture', 'config-version': 3,
        nodes: [{ name: 'retained', type: 'BlackHole' }],
    };
    data.server.core.configs = [{ id: 8, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const activate = (x, y) => click(page, x, y);
    const review = /Resolve and save configuration\..*replaces variables with their values and discards variable definitions, comments, and commits metadata\./;
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 8 });
        loaded.reply({ result: { config, revision: 'before-import', editable: true } });
        await status(page, /Wave Config Import fixture\. Nodes tab\. Nodes\. 1 node\./);

        await openConfigJsonEditor(page);
        await pasteConfigJson(page, JSON.stringify([{ name: 'retained draft', type: 'BlackHole' }]));
        await status(page, /1 node\..*Unsaved changes\./);

        await openConfigJsonEditor(page);
        await pasteConfigJson(page, '[// Fixture comment\n{"name":"discarded-a","type":"BlackHole"},'
            + '{"name":"discarded-b","type":"BlackHole"}]');
        await status(page, review);
        await page.locator('#wave-json-editor').waitFor({ state: 'hidden', timeout });
        assert.equal(preview.actions.length, 1, 'The review does not save the import.');
        await activate(683, (await dialogFrame(page)).bottom - 25);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await activate(300, 215);
        await status(page, /1 node\. Selected retained draft\. Type BlackHole\..*Unsaved changes\./);
        assert.equal(preview.actions.length, 1, 'Cancel keeps the current graph and sends no save.');

        await openConfigJsonEditor(page);
        await pasteConfigJson(page, '{"nodes":[{"name":$fixture_missing$,"type":"BlackHole"}]}');
        await status(page, /Edit configuration JSON\..*undefined variable\./);
        assert.equal(preview.actions.length, 1, 'An undefined variable does not save the import.');

        await pasteConfigJson(page, String.raw`{
            "name": "Source fixture",
            "author": "Source metadata",
            "commits": [{"message": "Fixture metadata"}],
            "nodes": [
                // The variable tokens are outside JSON strings.
                {"name":"dummy-in","type":"TcpListener","settings":{"address":$fixture_address$,"port":$fixture_port$},"next":$fixture_target$},
                {"name":"dummy-out","type":"BlackHole","settings":{"marker":"literal // $fixture_port$"}}
            ],
            "var\u0069ables": {"fixture_address":"192.0.2.10","fixture_port":31870,"fixture_target":"dummy-out"}
        }`);
        await status(page, review);
        assert.equal(preview.actions.length, 1, 'Variables and comments need explicit confirmation.');
        await confirmDialog(page, activate, 500);
        const saved = await preview.action(1);
        const document = {
            ...config,
            nodes: [
                { name: 'dummy-in', type: 'TcpListener', settings: { address: '192.0.2.10', port: 31870 }, next: 'dummy-out' },
                { name: 'dummy-out', type: 'BlackHole', settings: { marker: 'literal // $fixture_port$' } },
            ],
        };
        assertRequest(saved.request, { action: 'save_core_config', id: 8, revision: 'before-import', config: document });
        await status(page, /Wave Config Import fixture\..*1 node\..*Operation in progress\./);
        await page.locator('#wave-json-editor').waitFor({ state: 'hidden', timeout });
        saved.reply({ feedback: 'Saved Import fixture.', feedback_in_logs_only: true,
            result: { config: document, revision: 'after-import', editable: true } });
        await status(page, /Wave Config Import fixture\..*2 nodes\..*Latest log: Saved Import fixture\./);
    } finally {
        await page.close();
    }
});

test('shows only the connected side of each packet adapter and both sides before wiring', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const nodes = [
        { name: 'tun source', type: 'TunDevice', next: 'raw sink', 'panel-settings': { x: 30, y: 30 } },
        { name: 'raw sink', type: 'RawSocket', 'panel-settings': { x: 270, y: 30 } },
        { name: 'raw idle', type: 'RawSocket', 'panel-settings': { x: 30, y: 130 } },
        { name: 'tun idle', type: 'TunDevice', 'panel-settings': { x: 270, y: 130 } },
    ];
    const config = { name: 'Port fixture', nodes };
    data.server.core.configs = [{ id: 9, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const graphLeft = 189;
    const cardWidth = 148;
    const ports = async (node, top, expected) => {
        const { x, y } = node['panel-settings'];
        const mid = top + y + 29;
        const scans = [];
        // Probe inside each card so blue cables behind it cannot count as ports.
        for (const offset of [2, cardWidth - 2]) {
            scans.push(await verticalRuns(page, graphLeft + x + offset, mid - 3, mid + 4, [81, 165, 246]));
        }
        assert.deepEqual(scans.map(runs => runs.length > 0), expected, node.name);
    };
    try {
        await page.goto(workspace.url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 9 });
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Wave Config Port fixture\..*4 nodes\./);

        const [tunSource, rawSink, rawIdle, tunIdle] = nodes;
        const top = await graphCanvasTop(page);
        await ports(tunSource, top, [false, true]);
        await ports(rawSink, top, [true, false]);
        await ports(rawIdle, top, [true, true]);
        await ports(tunIdle, top, [true, true]);

        await click(page, graphLeft + 30 + cardWidth, top + 130 + 29);
        await click(page, graphLeft + 270 + cardWidth / 2, top + 130 + 29);
        await status(page, /Selected raw idle\. Type RawSocket\. Next tun idle\..*Unsaved changes\./);
        const updatedTop = await graphCanvasTop(page);
        await ports(rawIdle, updatedTop, [false, true]);
        await ports(tunIdle, updatedTop, [true, false]);
        assert.equal(preview.actions.length, 1, 'Wiring the adapters changes only the draft.');
    } finally {
        await page.close();
    }
});
