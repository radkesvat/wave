const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
const { test } = require('node:test');
const { chromium } = require('playwright-core');
const {
    setupWorkspace,
    desktop,
    coreDesktop,
    portrait,
    donationSlotHeight,
    timeout,
    fixture,
    tlsFixture,
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
    horizontalRuns,
    rectangleColorCount,
    rectangleContainsColor,
    expectDonationBox,
    dialogFrame,
    confirmDialog,
    graphCanvasTop,
    waitForServerStatus,
    selectConfigTab,
} = require('./workspace_fixture.cjs');

const workspace = setupWorkspace();
const { mockPreview } = workspace;

async function grayscalePixelCount(page, clip, minimum, maximum) {
    const screenshot = await page.screenshot({ clip });
    return page.evaluate(async ({ encoded, minimum, maximum }) => {
        const bytes = Uint8Array.from(atob(encoded), character => character.charCodeAt(0));
        const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
        const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const { data } = context.getImageData(0, 0, bitmap.width, bitmap.height);
        let count = 0;
        for (let i = 0; i < data.length; i += 4) {
            const red = data[i];
            const green = data[i + 1];
            const blue = data[i + 2];
            if (red >= minimum && red <= maximum && Math.max(red, green, blue) - Math.min(red, green, blue) <= 2) {
                count++;
            }
        }
        return count;
    }, { encoded: screenshot.toString('base64'), minimum, maximum });
}

function tlsInputRows(page) {
    return verticalRuns(page, 220, 170, 374, [13, 11, 14], 20);
}

async function tlsApplyRow(page, afterFields) {
    const [row] = await verticalRuns(page, 220, afterFields, 515, [69, 64, 79], 18);
    assert.ok(row, 'Expected the TLS Apply changes button.');
    return Math.floor((row[0] + row[1]) / 2);
}

async function lifecycleButton(page, action) {
    const [title] = await verticalRuns(page, 510, 250, 650, [43, 39, 48], 14);
    assert.ok(title, 'Expected the Panel Controls title.');
    const x = action === 'stop' ? 675 : 510;
    await click(page, x, title[1] + (action === 'restart' ? 84 : 47));
}

async function versionControls(page) {
    await page.mouse.move(0, 0);
    await settleCanvas(page);
    const [title] = await verticalRuns(page, 510, 250, 650, [43, 39, 48], 14);
    assert.ok(title, 'Expected the Panel Controls title.');
    const [picker] = await verticalRuns(page, 590, title[1], title[1] + 230, [13, 11, 14], 20);
    assert.ok(picker, 'Expected a target core version picker.');
    const [check] = await verticalRuns(page, 490, picker[1], picker[1] + 45, [69, 64, 79], 20);
    assert.ok(check, 'Expected the Check for updates button in Panel Controls.');
    const checkY = (check[0] + check[1]) / 2;
    const pickerY = (picker[0] + picker[1]) / 2;
    return { pickerY, pickerBottom: picker[1], checkY, changeY: checkY + (checkY - pickerY) };
}

test('opens the authenticated workspace, updates host metrics, and navigates the sidebar', async () => {
    const panelVersion = process.env.WAVE_EXPECT_PANEL_VERSION;
    assert.ok(panelVersion, 'Set WAVE_EXPECT_PANEL_VERSION or run through a Wave frontend CTest preset.');
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture();
    delete data.server.version;
    const firstSnapshot = Promise.withResolvers();
    const preview = await mockPreview(page, data, firstSnapshot.promise);
    const canvasColor = [8, 12, 16];
    const paneColor = [31, 27, 34];
    const badgeBorder = [138, 132, 149];
    const selectedColor = [243, 129, 74];
    const configRow = color => verticalRuns(page, 11, 110, 128, color);
    const expectInsetLink = async top => {
        const interior = [top + 5, top + 23];
        for (const x of [11, 176]) {
            assert.deepEqual(await verticalRuns(page, x, ...interior, paneColor), [interior],
                'The selected sidebar row leaves the pane edges visible.');
        }
        assert.deepEqual(await verticalRuns(page, 13, ...interior, canvasColor), [interior]);
        for (const x of [173, 174]) {
            assert.deepEqual(await verticalRuns(page, x, top, top + 30, selectedColor), [[top, top + 30]]);
        }
    };
    try {
        assert.equal((await page.goto(workspace.url)).status(), 200);
        await status(page, /Wave Server Status.*Loading panel data/);
        firstSnapshot.resolve();
        await waitForServerStatus(page);

        await openView(page, 'Server Status');
        await status(page, /CPU 25\.0%\. Memory 4\.0 \/ 16\.0 GiB.*Upload 2\.0 MiB\/s\. Download 6\.0 MiB\/s/);
        await status(page, `Panel version ${panelVersion}.`);
        await status(page, /Core version 1\.6\.0\./);
        await status(page, /Server Information\..*Host sent 13\.4 GB\. Host received 59\.9 GB\./);
        const dividerScanEnd = desktop.height - 11 - donationSlotHeight;
        const dividers = await verticalRuns(page, 152, 91, dividerScanEnd, [61, 58, 67]);
        assert.deepEqual(dividers.map(([top, bottom]) => bottom - top), [2, 1, 1, 1]);
        for (const x of [11, 176]) {
            assert.deepEqual(await verticalRuns(page, x, 91, dividerScanEnd, [61, 58, 67]), dividers);
        }
        await expectInsetLink(63);
        assert.deepEqual(await configRow(paneColor), [[110, 128]]);
        await page.mouse.move(12, 115);
        await settleCanvas(page);
        for (const x of [11, 176]) {
            assert.deepEqual(await verticalRuns(page, x, 110, 128, [55, 49, 59]), [[110, 128]]);
        }
        const links = page.getByRole('navigation', { name: 'Wave' }).getByRole('link');
        const expectLinks = async names => {
            await page.waitForFunction(names => JSON.stringify(
                Array.from(document.querySelectorAll('#navigation a'), link => link.textContent)
            ) === JSON.stringify(names), names, { timeout });
            assert.deepEqual(await links.allTextContents(), names);
        };
        const configs = data.server.core.configs;
        const archive = { id: 2, name: 'Archive', enabled: false, active: false };
        configs.push(archive);
        await preview.broadcast();
        await expectLinks(['Server Status', 'Default', 'Archive', 'Console', 'Settings', 'About']);
        await page.getByRole('link', { name: 'Default' }).focus();
        configs.reverse();
        await preview.broadcast();
        await expectLinks(['Server Status', 'Archive', 'Default', 'Console', 'Settings', 'About']);
        assert.equal(await page.evaluate(() => document.activeElement.textContent), 'Default');
        configs.shift();
        await preview.broadcast();
        await expectLinks(['Server Status', 'Default', 'Console', 'Settings', 'About']);
        Object.assign(data.server, {
            cpu_percent: null, cpu_history: [25, null],
            upload_rate: 0.5 / 1024, download_rate: 1.5 / 1024,
            upload_history: [0.5 / 1024], download_history: [1.5 / 1024],
        });
        await preview.broadcast();
        await status(page, /CPU Unavailable.*Upload 0\.5 KiB\/s\. Download 1\.5 KiB\/s/);

        await click(page, 80, 115);
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 1 });
        opened.reply({ result: { config: { name: 'Default', nodes: [] }, revision: 'saved', editable: false } });
        await status(page, /^Wave Config Default\..*0 nodes\./);
        assert.deepEqual(await verticalRuns(page, 11, 68, 86, paneColor), [[68, 86]]);
        assert.deepEqual(await configRow(canvasColor), [[110, 128]]);
        assert.deepEqual(await verticalRuns(page, 176, 110, 128, selectedColor), [[110, 128]]);
        await page.mouse.move(11, 77);
        await settleCanvas(page);
        assert.deepEqual(await verticalRuns(page, 11, 68, 86, paneColor), [[68, 86]]);
        assert.deepEqual(await verticalRuns(page, 13, 68, 86, [55, 49, 59]), [[68, 86]],
            'Hovering over the sidebar edge still highlights the inset row.');

        await expectDonationBox(page, { left: 10, right: 178, top: 735, bottom: 786, stickerDiameter: 36 });
        assert.equal(await rectangleContainsColor(page, {
            x: 89, y: desktop.height - 46 - donationSlotHeight, width: 82, height: 24,
        }, badgeBorder), false, 'About has the sidebar row to itself.');
        assert.equal(await page.getByRole('navigation', { name: 'Wave' }).getAttribute('aria-description'),
            'no donation');
        await click(page, 174, 760);
        await status(page, /^Wave About\./);
        await click(page, 80, desktop.height - 103 - donationSlotHeight);
        await status(page, /^Wave Console\./);
        const aboutY = desktop.height - 35 - donationSlotHeight;
        await click(page, 176, aboutY);
        await status(page, /^Wave About\./);
        await expectInsetLink(aboutY - 14);
        await page.setViewportSize({ width: 1920, height: 958 });
        await page.mouse.move(0, 0);
        await settleCanvas(page);
        await expectDonationBox(page, { left: 10, right: 224, top: 886, bottom: 944, stickerDiameter: 44 });
        const shortHeight = 500;
        await page.setViewportSize({ ...desktop, height: shortHeight });
        await settleCanvas(page);
        await click(page, 80, shortHeight - 103 - donationSlotHeight);
        await status(page, /^Wave Console\./);
        await click(page, 80, shortHeight - 69 - donationSlotHeight);
        await status(page, /^Wave Settings\./);
        await click(page, 80, shortHeight - 35 - donationSlotHeight);
        await status(page, /^Wave About\./);
    } finally {
        firstSnapshot.resolve();
        await page.close();
    }
});

test('records all illustrated donation choices and shows the 30-day reminder', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, hasTouch: true });
    const data = fixture();
    const preview = await mockPreview(page, data);
    const buttonFace = [69, 64, 79];
    const linkColor = [81, 165, 246];
    const desktopBadge = { x: 22, y: 743, width: 36, height: 36 };
    const responseWidth = 420;
    const pickerWidth = 737;
    const choices = /Did you donate to the project in the past 30 days\? No donation\. 1 USD\. 3 USD\. 5 USD\. 10 USD\. 15 USD\. 30 USD\. 50\+ USD\./;
    const inlineChoices = page.getByRole('img', { name: choices });
    const picker = page.getByRole('img', { name: /Change answer.*Cancel\./ });
    const reminder = page.getByRole('img', { name: /The sidebar badge will remind you in 30 days\./ });
    const modal = page.getByRole('img', { name: /Donation response/ });
    const nextReminder = page.getByRole('img', { name: /Next reminder/ });
    const clickAt = (x, y) => click(page, x, y);
    const tapAt = async (x, y) => {
        await page.touchscreen.tap(x, y);
        await settleCanvas(page);
    };
    const expectBadge = text => page.waitForFunction(text =>
        document.getElementById('navigation').getAttribute('aria-description') === text, text, { timeout });
    const captureBadge = async () => {
        await page.mouse.move(0, 0);
        await settleCanvas(page);
        return (await page.screenshot({ clip: desktopBadge })).toString('base64');
    };
    const dialogBody = dialogWidth => {
        const { width } = page.viewportSize();
        const outerWidth = Math.min(dialogWidth, width - 24);
        const left = Math.floor((width - outerWidth) / 2) + 12;
        return { left, right: left + outerWidth - 24 };
    };
    const expectSummary = async (label, date) => {
        await status(page, `Saved answer is ${label}.${date ? ` Next reminder ${date} (UTC).` : ''} Change answer.`);
        assert.equal(await inlineChoices.count(), 0, 'A current answer hides the inline options.');
        assert.equal(await nextReminder.count(), date ? 1 : 0);
    };
    const choiceGrid = async ({ left, right, top, bottom }) => {
        const rows = await verticalRuns(page, left + 5, top, bottom, buttonFace, 48);
        const cells = [];
        for (const [rowTop, rowBottom] of rows) {
            const columns = await horizontalRuns(page, rowTop + 3, left, right, buttonFace, 56);
            cells.push(...columns.map(([start, end]) => [Math.floor((start + end) / 2),
                Math.floor((rowTop + rowBottom) / 2)]));
        }
        assert.equal(cells.length, 8, 'All eight donation buttons are visible.');
        return { cells, rows };
    };
    const openChangeAnswer = async (activate, left, scanTop) => {
        const [button] = await verticalRuns(page, left, scanTop, 580, buttonFace, 26);
        assert.ok(button, 'Change answer is available below the summary.');
        await activate(left, Math.floor((button[0] + button[1]) / 2));
        await picker.waitFor({ timeout });
        await status(page, choices);
        await page.mouse.move(0, 0);
        await settleCanvas(page);
    };
    const modalChoices = async (touch = false) => {
        const { top, bottom } = await dialogFrame(page, 30);
        const { width } = page.viewportSize();
        const expectedWidth = Math.min(pickerWidth, width - 24);
        const outline = await horizontalRuns(page, top, 0, width, [138, 132, 149], expectedWidth - 2);
        assert.ok(outline.some(([left, right]) => Math.abs(right - left - expectedWidth) <= 2),
            'The enlarged Change answer dialog fits within the viewport.');
        if (touch) {
            // Move the last touch from the opening button onto the noninteractive title bar.
            await tapAt(Math.floor(width / 2), top + 12);
        }
        return choiceGrid({ ...dialogBody(pickerWidth), top: top + 35, bottom: bottom - 45 });
    };
    const activateDonateLink = async (left, top, bottom) => {
        await status(page, /Donate link\./);
        const description = await page.locator('#canvas').getAttribute('aria-label');
        assert.equal(description.split('Donate link.').length, 2, 'The donation link is announced once.');
        const openedBefore = await page.evaluate(() => window.openedLinks.length);
        await page.mouse.move(0, 0);
        await settleCanvas(page);
        const link = { x: left, y: top, width: 65, height: bottom - top };
        assert.ok(await rectangleContainsColor(page, link, linkColor), 'Donate is visible.');
        await clickAt(left + 16, Math.floor((top + bottom) / 2));
        const opened = await page.evaluate(() => window.openedLinks);
        assert.deepEqual(opened.slice(openedBefore), [
            ['https://radkesvat-support.vercel.app/#support', '_blank', 'noopener,noreferrer'],
        ]);
    };
    const respond = async ({ key, label, position, activate = clickAt, answeredAt = '2027-01-15T13:00:00Z',
        remindAt = '2027-02-14T13:00:00Z', date = '27/02/14' }) => {
        const isDonation = key !== 'no';
        const index = preview.actions.length;
        await activate(...position);
        const request = await preview.action(index);
        assertRequest(request.request, { action: 'apply_donation', answer: key });
        await modal.waitFor({ timeout });
        assert.equal(await picker.count(), 0, 'The donation response replaces the choice dialog.');
        await status(page, isDonation ? /Thank you for supporting the project!/ : /The project is free\. No problem\./);
        await status(page, /Operation in progress\./);
        assert.equal(await reminder.count(), 0);
        data.settings.donation = {
            answer: key, answered_at: answeredAt, reminder_due: false,
            ...(isDonation ? { next_reminder_at: remindAt } : {}),
        };
        request.reply();
        await status(page, `Saved answer is ${label}.`);
        if (isDonation) {
            await reminder.waitFor({ timeout });
        } else {
            assert.equal(await reminder.count(), 0);
            const { top } = await dialogFrame(page, 30);
            const { left, right } = dialogBody(responseWidth);
            assert.ok(await rectangleContainsColor(page, {
                x: left + 200, y: top + 33, width: right - left - 200, height: 21,
            }, [255, 195, 106]), 'An inline smiley sticker is visible at the end of the decline message.');
            await activateDonateLink(left + 64, top + 53, top + 78);
        }
        await confirmDialog(page, activate, responseWidth, 30);
        await modal.waitFor({ state: 'hidden', timeout });
        await expectBadge(isDonation ? 'Donator' : 'no donation');
        await expectSummary(label, isDonation ? date : undefined);
    };

    try {
        await page.addInitScript(() => {
            window.openedLinks = [];
            window.open = (...args) => {
                window.openedLinks.push(args);
                return null;
            };
        });
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await expectBadge('no donation');
        const noBadge = await captureBadge();
        await openView(page, 'About');
        await status(page, choices);
        assert.equal(preview.actions.length, 0, 'Opening About leaves the question unanswered.');

        const inline = await choiceGrid({ left: 198, right: 1256, top: 113, bottom: 415 });
        const lastRow = inline.rows.at(-1)[1];
        await activateDonateLink(198, lastRow, lastRow + 30);
        assert.ok(await rectangleContainsColor(page, {
            x: 198, y: lastRow + 40, width: 150, height: 70,
        }, linkColor), 'Project links follow the Donate link.');
        await respond({ key: '1', label: '1 USD', position: inline.cells[1], answeredAt: '2026-12-31T23:30:00Z',
            remindAt: '2027-01-30T23:30:00Z', date: '27/01/30' });
        const badges = [noBadge, await captureBadge()];

        await openChangeAnswer(clickAt, 203, 113);
        const desktopPicker = await modalChoices();
        const artwork = {
            x: desktopPicker.cells[2][0] - 10, y: desktopPicker.rows[0][0] + 72, width: 20, height: 10,
        };
        const artworkBackground = await rectangleColorCount(page, artwork, buttonFace);
        assert.ok(artworkBackground < artwork.width * artwork.height / 2,
            'The cat image extends into the enlarged area above its label.');
        await confirmDialog(page, clickAt, pickerWidth, 30);
        await picker.waitFor({ state: 'hidden', timeout });
        await expectSummary('1 USD', '27/01/30');
        assert.equal(preview.actions.length, 1, 'Cancel preserves the saved answer.');

        await page.reload();
        await waitForServerStatus(page);
        await expectBadge('Donator');
        await openView(page, 'About');
        await expectSummary('1 USD', '27/01/30');
        assert.equal(preview.actions.length, 1, 'Reloading restores the saved answer.');

        await openChangeAnswer(clickAt, 203, 113);
        await respond({ key: 'no', label: 'No donation', position: desktopPicker.cells[0] });
        assert.equal(await captureBadge(), noBadge, 'No donation uses the first cat.');

        const amounts = ['3', '5', '10', '15', '30'];
        for (const [index, amount] of amounts.entries()) {
            await openChangeAnswer(clickAt, 203, 113);
            await respond({ key: amount, label: `${amount} USD`, position: desktopPicker.cells[index + 2] });
            badges.push(await captureBadge());
        }

        await page.setViewportSize(portrait);
        await settleCanvas(page);
        await openChangeAnswer(tapAt, 25, 245 + donationSlotHeight);
        const touchPicker = await modalChoices(true);
        assert.ok(touchPicker.rows.length > desktopPicker.rows.length, 'The choices reflow on a narrow screen.');
        await respond({ key: '50_plus', label: '50+ USD', position: touchPicker.cells[7], activate: tapAt });
        await page.setViewportSize(desktop);
        await settleCanvas(page);
        badges.push(await captureBadge());
        assert.equal(new Set(badges).size, 8, 'Each donation answer has a distinct sticker.');
        assert.ok(await grayscalePixelCount(page, desktopBadge, 60, 105) > 550,
            'The highest tier uses the grayscale Gigachad sticker.');

        delete data.settings.donation.next_reminder_at;
        data.settings.donation.reminder_due = true;
        await preview.broadcast();
        await expectBadge('donation?');
        assert.equal(await captureBadge(), noBadge, 'A due reminder uses the first cat.');
        await status(page, choices);
        await status(page, 'Last answer was 50+ USD.');
        assert.equal(await nextReminder.count(), 0);
        assert.equal(preview.actions.length, 8, 'The reminder does not submit an answer.');
    } finally {
        await page.close();
    }
});

test('shows the Tools demo section only when enabled in the build', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const showDemos = process.env.WAVE_EXPECT_UI_DEMOS === '1';
    try {
        await mockPreview(page, fixture());
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await openView(page, 'About');
        await page.getByRole('button', { name: 'Tools', exact: true }).focus();
        await page.keyboard.press('Enter');
        await page.locator('#tools[aria-expanded="true"]').waitFor({ timeout });
        await settleCanvas(page);

        const demoRows = await verticalRuns(page, 340, 127, 167, [43, 39, 48]);
        const demoDivider = await verticalRuns(page, 340, 114, 124, [61, 58, 67]);
        assert.deepEqual(demoRows, showDemos ? [[127, 167]] : []);
        assert.equal(demoDivider.length, showDemos ? 1 : 0);

        await click(page, 230, 81);
        await page.locator('#tools[aria-expanded="false"]').waitFor({ timeout });
        assert.equal((await verticalRuns(page, 441, 49, 96, [26, 23, 28], 25)).length, 1,
            'Renderer metrics still opens from Tools.');
    } finally {
        await page.close();
    }
});

test('opens the Mini keyboard from desktop Tools and copies the composed text', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    try {
        await mockPreview(page, fixture());
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        const keyboard = page.locator('#wave-keyboard');
        assert.equal(await keyboard.isVisible(), false);
        await page.getByRole('button', { name: 'Tools', exact: true }).focus();
        await page.keyboard.press('Enter');
        await page.locator('#tools[aria-expanded="true"]').waitFor({ timeout });
        await click(page, 230, 103);
        await keyboard.waitFor({ timeout });
        assert.equal(await page.locator('#wave-text-input').evaluate(input => input === document.activeElement), true);

        for (const name of ['Shift', 'W', 'a']) {
            await keyboard.getByRole('button', { name, exact: true }).click({ timeout });
        }
        await page.evaluate(() => navigator.clipboard.writeText(''));
        const frame = await dialogFrame(page);
        const [leftEdge] = await horizontalRuns(page, frame.top + 10, 0, desktop.width, [138, 132, 149]);
        assert.ok(leftEdge, 'The scratch dialog has a visible border.');
        const copyX = leftEdge[0] + 18;
        const [copy] = await verticalRuns(page, copyX, frame.top + 40, frame.bottom - 45, [43, 39, 48], 20);
        assert.ok(copy, 'The scratch dialog keeps its Copy button.');
        await page.mouse.move(copyX, (copy[0] + copy[1]) / 2);
        await page.mouse.down();
        await settleCanvas(page);
        await page.mouse.up();
        await page.waitForFunction(async () => await navigator.clipboard.readText() === 'Wa', undefined, { timeout });
    } finally {
        await page.close();
    }
});

test('loads workspace pages without passive spinners', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const config = { name: 'Default', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    const offlineScope = { source: 'offline', run_id: 'staff-stopped' };
    data.server.core.configs = [{ id: 1, name: config.name, enabled: false, active: false, auth_nodes: ['staff'],
        users_access: { staff: { ...offlineScope, database_generation: 'staff-database' } } }];
    const firstSnapshot = Promise.withResolvers();
    const preview = await mockPreview(page, data, firstSnapshot.promise);
    const expectNoSpinner = async (view, clip) => {
        await settleCanvas(page);
        assert.equal(await rectangleContainsColor(page, clip, [81, 165, 246]), false,
            `${view} waits for its initial data without a spinner.`);
    };
    try {
        await page.goto(workspace.url);
        await status(page, /Wave Server Status.*Loading panel data/);
        await expectNoSpinner('Server Status', { x: 198, y: 62, width: 90, height: 55 });
        await expectNoSpinner('Logs', { x: 198, y: 628, width: 90, height: 75 });
        await openView(page, 'Settings');
        await expectNoSpinner('Panel settings', { x: 198, y: 96, width: 60, height: 100 });
        await click(page, 281, 82);
        await status(page, /Core tab.*Loading panel data/);
        await expectNoSpinner('Core settings', { x: 198, y: 96, width: 60, height: 130 });
        await openView(page, 'Server Status');
        firstSnapshot.resolve();
        await waitForServerStatus(page);
        await status(page, /Latest log: Preview fixture is ready\./);

        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 1 });
        await status(page, /Nodes tab\. Nodes\. Loading configuration\./);
        await expectNoSpinner('Nodes', { x: 198, y: 118, width: 90, height: 90 });
        opened.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);
        await graphCanvasTop(page);

        await selectConfigTab(page, 'staff');
        const userControls = { x: 198, y: 121, width: desktop.width - 198, height: 60 };
        const users = [{ id: '1', name: 'Fixture account', enabled: true }];
        const { subscription, ...subscribe } = await preview.rosterAction(0);
        assert.deepEqual(subscribe, { action: 'users_roster_subscribe', config_id: 1, node: 'staff',
            database_generation: 'staff-database', ...offlineScope });
        await status(page, /staff tab\. AuthenticationServer users\..*Loading users\./);
        await expectNoSpinner('Offline users', userControls);
        await preview.roster(subscription, users);
        await status(page, /staff tab\. AuthenticationServer users\..*1 users?\./);

        await click(page, 642, 137);
        assert.deepEqual(await preview.rosterAction(1), { action: 'users_roster_refresh', subscription, resync: false });
        await preview.roster(subscription, users, { sequence: '2' });
        await status(page, /staff tab\. AuthenticationServer users\..*1 users?\./);
    } finally {
        firstSnapshot.resolve();
        await page.close();
    }
});

test('switches the canvas, browser, and Console among all five themes', async () => {
    const classicBrowser = await chromium.launch({
        args: ['--enable-unsafe-swiftshader'],
        ignoreDefaultArgs: ['--hide-scrollbars'],
    });
    const [defaultTheme, lightTheme, ...darkThemes] = [
        { name: 'Default', canvas: '080c10', sidebar: '1f1b22', terminal: '050506', track: '0d0b0e', thumb: '8a8495' },
        { name: 'Light', canvas: 'f0f2f7', sidebar: 'f9fafd', terminal: 'f9fafd', track: 'ffffff', thumb: '687189' },
        { name: 'Classic', canvas: '1b2529', sidebar: '232c36', terminal: '121b21', track: '141a22', thumb: '778999' },
        { name: 'Dark', canvas: '171a20', sidebar: '22262e', terminal: '101217', track: '111318', thumb: '808ba1' },
        { name: 'Darker', canvas: '08090b', sidebar: '141517', terminal: '030405', track: '050608', thumb: '787b84' },
    ];
    const channels = hex => [...Buffer.from(hex, 'hex')];
    const rgb = hex => `rgb(${channels(hex).join(', ')})`;
    try {
        const page = await classicBrowser.newPage({ viewport: desktop });
        const data = fixture();
        const preview = await mockPreview(page, data);
        let nextAction = 0;
        const menu = page.locator('#menu');
        const expectTheme = async (theme, thumb = theme.thumb) => {
            await waitForTheme(page, theme.canvas);
            await settleCanvas(page);
            const browserColors = await page.evaluate(() => {
                const root = document.documentElement;
                const style = getComputedStyle(root);
                return {
                    scheme: style.colorScheme,
                    background: style.backgroundColor,
                    track: getComputedStyle(root, '::-webkit-scrollbar-track').backgroundColor,
                    thumb: style.getPropertyValue('--scrollbar-thumb'),
                    terminal: getComputedStyle(document.getElementById('wave-console')).backgroundColor,
                };
            });
            assert.deepEqual(browserColors, {
                scheme: theme.name === 'Light' ? 'light' : 'dark',
                background: rgb(theme.canvas),
                track: rgb(theme.track),
                thumb: rgb(thumb),
                terminal: rgb(theme.terminal),
            }, theme.name);
            assert.deepEqual(await verticalRuns(page, 11, 68, 86, channels(theme.sidebar)), [[68, 86]],
                `${theme.name} recolors the ImGui sidebar.`);
        };

        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await page.waitForFunction(() => document.documentElement.classList.contains('classic-scrollbars'),
            undefined, { timeout });
        await expectTheme(defaultTheme);
        for (const theme of [lightTheme, ...darkThemes, defaultTheme]) {
            await selectPanelTheme(page, theme.name);
            await acknowledgeThemeSave(preview, data, nextAction++, theme.name.toLowerCase());
            await expectTheme(theme);
            assert.equal(await menu.getAttribute('aria-expanded'), 'true', 'The theme picker stays inside Menu.');
        }

        await click(page, 145, 145);
        await expectTheme(defaultTheme, 'b5b0c4');
        assert.equal(preview.actions.length, nextAction, 'Changing contrast does not send a theme save.');
        await selectPanelTheme(page, 'Light');
        await acknowledgeThemeSave(preview, data, nextAction++, 'light');
        await expectTheme(lightTheme, '495063');
        await click(page, 130, 24);
        await openView(page, 'Console');
        const terminal = page.locator('#wave-console');
        await terminal.waitFor({ state: 'visible', timeout });
        assert.equal(await terminal.evaluate(element => getComputedStyle(element).backgroundColor), rgb(lightTheme.terminal));

        await page.reload();
        await waitForServerStatus(page);
        await expectTheme(lightTheme);
        assert.equal(preview.actions.length, nextAction, 'Reload restores the saved theme without sending another save.');
    } finally {
        await classicBrowser.close();
    }
});

test('keeps the latest theme visible and saves it after a delayed acknowledgement', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture();
    data.settings.theme = 'default';
    const preview = await mockPreview(page, data);
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await selectPanelTheme(page, 'Light');
        await waitForTheme(page, 'f0f2f7');
        const firstSave = await preview.action(0);
        const firstId = assertRequest(firstSave.request, { action: 'apply_theme', theme: 'light' });

        await selectPanelTheme(page, 'Darker');
        await waitForTheme(page, '08090b');
        data.server.cpu_percent = 41;
        await preview.broadcast();
        await status(page, /CPU 41\.0%/);
        assert.equal((await page.locator('meta[name="theme-color"]').getAttribute('content')).toLowerCase(), '#08090b',
            'A snapshot with the previous saved theme leaves the latest selection visible.');

        await selectPanelTheme(page, 'Default');
        await waitForTheme(page, '080c10');
        await settleCanvas(page);
        assert.equal(preview.actions.length, 1, 'Only one theme save is sent while its reply is pending.');
        await page.evaluate(() => {
            const meta = document.querySelector('meta[name="theme-color"]');
            window.themeColors = [meta.content];
            const observer = new MutationObserver(records => {
                window.themeColors.push(...records.map(record => record.oldValue), meta.content);
            });
            observer.observe(meta, { attributeFilter: ['content'], attributeOldValue: true });
        });

        data.settings.theme = 'light';
        firstSave.reply();
        const latestSave = await preview.action(1);
        const latestId = assertRequest(latestSave.request, { action: 'apply_theme', theme: 'default' });
        assert.ok(latestId > firstId);
        data.settings.theme = 'default';
        latestSave.reply();
        await settleCanvas(page);
        const visibleColors = await page.evaluate(() => [...new Set(window.themeColors.map(color => color.toLowerCase()))]);
        assert.deepEqual(visibleColors, ['#080c10'], 'The old Light acknowledgement never replaces Default on screen.');
        assert.equal(preview.actions.length, 2, 'Only the first and newest selections are saved.');
    } finally {
        await page.close();
    }
});

test('scrolls stacked Server Status inside the desktop workspace and uses page scrolling on compact screens', async () => {
    const classicBrowser = await chromium.launch({
        args: ['--enable-unsafe-swiftshader'],
        ignoreDefaultArgs: ['--hide-scrollbars'],
    });
    try {
        const page = await classicBrowser.newPage({ viewport: coreDesktop });
        await mockPreview(page, fixture());
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await page.waitForFunction(() => document.documentElement.classList.contains('classic-scrollbars'),
            undefined, { timeout });

        const canvas = page.locator('#canvas');
        const expectFullWidth = async ({ width, height }) => {
            await page.waitForFunction(({ width, height }) => document.documentElement.scrollHeight === height &&
                document.querySelector('#canvas').getBoundingClientRect().width === width, { width, height }, { timeout });
            await settleCanvas(page);
            assert.equal(await page.evaluate(() => document.documentElement.scrollHeight), height);
            assert.equal((await canvas.boundingBox()).width, width);
            assert.deepEqual(await verticalRuns(page, width - 1, 0, 5, [243, 129, 74]), [[0, 2]]);
        };
        const logsEdge = async ({ width, height }) => {
            const runs = await verticalRuns(page, width - 11, height - 270, height, [61, 58, 67], 18);
            assert.ok(runs.length, 'Expected the right-hand Logs border.');
            return runs.at(-1);
        };
        await expectFullWidth(coreDesktop);
        const paneStart = coreDesktop.height - 100;
        const paneEnd = coreDesktop.height - 30;
        assert.deepEqual(await verticalRuns(page, coreDesktop.width - 16, paneStart, paneEnd, [31, 27, 34]),
            [[paneStart, paneEnd]]);

        const columns = { width: 890, height: 800 };
        await page.setViewportSize(columns);
        await expectFullWidth(columns);
        await page.mouse.move(482, 445);
        await status(page, /Checking for updates refreshes the available WaterWall releases\./);
        await page.mouse.move(0, 0);
        const stacked = { ...columns, width: 860 };
        await page.setViewportSize(stacked);
        await expectFullWidth(stacked);
        const chartBands = () => verticalRuns(page, 215, 60, 550, [43, 39, 48], 14);
        const initialBands = await chartBands();
        assert.ok(initialBands.length, 'Expected the stacked card title bands.');
        const initialLogs = await logsEdge(stacked);
        await page.mouse.move(420, 350);
        await page.mouse.wheel(0, 450);
        await settleCanvas(page);
        assert.notDeepEqual(await chartBands(), initialBands, 'Wheel scrolling moves the stacked cards.');
        assert.equal(await page.evaluate(() => scrollY), 0, 'The desktop document stays in place.');
        assert.deepEqual(await logsEdge(stacked), initialLogs, 'Logs stay fixed as the work pane scrolls.');

        await openView(page, 'Settings');
        await expectFullWidth(stacked);
        assert.deepEqual(await logsEdge(stacked), initialLogs, 'Settings and Server Status share the Logs boundary.');
        await openView(page, 'Server Status');

        await page.setViewportSize({ ...columns, width: 700 });
        await page.waitForFunction(() => document.documentElement.scrollHeight > innerHeight &&
            document.documentElement.clientWidth < innerWidth, undefined, { timeout });
        await settleCanvas(page);
        const { width, scrollWidth } = await page.evaluate(() => ({
            width: document.documentElement.clientWidth,
            scrollWidth: document.documentElement.scrollWidth,
        }));
        assert.equal((await canvas.boundingBox()).width, width);
        assert.equal(scrollWidth, width);
        await page.mouse.move(420, 350);
        await page.mouse.wheel(0, 450);
        await page.waitForFunction(() => scrollY > 0, undefined, { timeout });

        await page.setViewportSize(columns);
        await page.waitForFunction(() => scrollY === 0 && document.documentElement.scrollHeight === innerHeight,
            undefined, { timeout });
        await expectFullWidth(columns);
    } finally {
        await classicBrowser.close();
    }
});

test('filters Logs by source for display, search, and clipboard copy', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const toolbarY = 613;
    const filters = [
        ['panel', 'Panel', 307], ['core', 'Core', 378], ['dns', 'DNS', 444],
        ['network', 'Network', 536], ['internal', 'Internal', 625],
    ];
    const data = fixture();
    data.logs = filters.map(([source], index) => ({
        source, ts: '09:41:02', level: 'warning', message: `Fixture log ${index + 1}`,
    }));
    const lines = data.logs.map(({ ts, level, message }, index) => `${ts} [${filters[index][1]}] [${level}] ${message}`);
    const copy = async () => {
        await page.evaluate(() => navigator.clipboard.writeText(''));
        await click(page, 813, toolbarY);
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0, null, { timeout });
        return (await page.evaluate(() => navigator.clipboard.readText())).trimEnd().split('\n');
    };
    await mockPreview(page, data);
    try {
        await page.goto(workspace.url);
        await openView(page, 'About');
        await status(page, /5 log entries\./);
        await settleCanvas(page);
        assert.equal(await rectangleContainsColor(page, {
            x: 1076, y: toolbarY - 2, width: 17, height: 9,
        }, [181, 180, 197]), true, 'The help glyph lines up with the Search label.');
        await page.mouse.move(1084, toolbarY + 6);
        await status(page, /Use spaces to require all terms, commas to match alternatives/);
        await page.mouse.move(0, 0);
        await click(page, 663, toolbarY);
        await page.mouse.move(794, toolbarY + 23);
        await status(page, /Follow new entries while you are at the bottom of Logs/);
        await page.keyboard.press('Escape');
        assert.deepEqual(await copy(), lines);
        for (const [index, [, , x]] of filters.entries()) {
            await click(page, x, toolbarY);
            await status(page, /4 log entries\./);
            assert.deepEqual(await copy(), lines.filter((_, row) => row !== index));
            await click(page, x, toolbarY);
        }
        await replaceText(page, 921, toolbarY, 'dns');
        await status(page, /1 log entr(?:y|ies)\. Latest log: Fixture log 3/);
        assert.deepEqual(await copy(), [lines[2]]);
        await click(page, 411, toolbarY); // The DNS label also toggles its checkbox.
        await status(page, /0 log entries\./);
    } finally {
        await page.close();
    }
});

test('installs, retries, starts, restarts, and stops WaterWall from Server Status', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('missing');
    const core = data.server.core;
    const preview = await mockPreview(page, data);
    const press = action => lifecycleButton(page, action);
    const controlsTitle = () => verticalRuns(page, 510, 250, 650, [43, 39, 48], 14);
    const progress = /Operation in progress\./;
    const progressIndicator = page.getByRole('img', { name: progress });
    const spinnerColor = [81, 165, 246];
    const runningColor = [163, 211, 83];
    const stoppedColor = [255, 195, 106];
    const expectCoreTextColor = async (state, controlsColor, headerColor = controlsColor) => {
        const [title] = await controlsTitle();
        assert.ok(title, 'Expected the Panel Controls title.');
        assert.ok(await rectangleContainsColor(page, {
            x: 477, y: title[1] + 6, width: 183, height: 22,
        }, controlsColor), `The Panel Controls ${state} text has the expected color.`);
        assert.ok(await rectangleContainsColor(page, {
            x: coreDesktop.width - 112, y: 14, width: 88, height: 20,
        }, headerColor), `The topbar ${state} text has the expected color.`);
    };
    const modalSpinner = frame => rectangleContainsColor(page, {
        x: (coreDesktop.width - 380) / 2 + 10, y: frame.bottom - 42, width: 40, height: 34,
    }, spinnerColor);
    try {
        await page.goto(workspace.url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core missing\. Core version 1\.6\.0\. Target version 1\.6\.0\./);
        const titleBeforeError = await controlsTitle();
        const coreSpinner = () => rectangleContainsColor(page, {
            x: 470, y: titleBeforeError[0][1] + 2, width: 54, height: 30,
        }, spinnerColor);
        assert.equal(await coreSpinner(), false, 'The core status starts without a spinner.');

        await press('install');
        const install = await preview.action(0);
        const firstId = assertRequest(install.request, { action: 'install_core' });
        await progressIndicator.waitFor({ timeout });
        assert.equal(await coreSpinner(), true, 'The direct action shows a spinner beside core status.');
        await press('install');
        assert.equal(preview.actions.length, 1, 'An install waiting for acknowledgement cannot be sent twice.');
        core.state = 'installing';
        install.reply({ feedback: 'Installing WaterWall.', feedback_in_logs_only: true, level: 'info' });
        await status(page, /WaterWall core installing.*Operation in progress\./);
        assert.equal(await coreSpinner(), true, 'The core status remains busy while installation runs.');
        await expectLoggedOnce(page, 'Installing WaterWall.');
        await press('install');
        assert.equal(preview.actions.length, 1, 'Installation cannot restart while in progress.');
        core.state = 'missing';
        const installationError = 'The WaterWall archive failed its SHA-256 check.';
        core.error = installationError + '\nCheck the installation logs.'.repeat(24);
        data.logs.push({ source: 'panel', ts: '09:41:04', level: 'error', message: core.error });
        await preview.broadcast();
        await status(page, /WaterWall core missing\. Core version/);
        await expectLoggedOnce(page, installationError);
        await progressIndicator.waitFor({ state: 'hidden', timeout });
        assert.equal(await coreSpinner(), false, 'Core status returns to idle after installation ends.');
        assert.deepEqual(await controlsTitle(), titleBeforeError, 'Core errors should leave the card in place.');

        await press('install');
        const retry = await preview.action(1);
        assert.notEqual(assertRequest(retry.request, { action: 'install_core' }), firstId);
        delete core.error;
        core.state = 'stopped';
        retry.reply();
        await status(page, /WaterWall core stopped/);
        await expectCoreTextColor('stopped', stoppedColor);
        await press('restart');
        assert.equal(await page.locator('#panel-controls').evaluate(controls => controls.inert), false);
        await press('start');
        const start = await preview.action(2);
        assertRequest(start.request, { action: 'start_core' });
        await progressIndicator.waitFor({ timeout });
        core.state = 'starting';
        start.reply();
        await status(page, /WaterWall core starting.*Operation in progress\./);
        await expectCoreTextColor('starting', [234, 235, 255], [181, 180, 197]);
        core.state = 'running';
        await preview.broadcast();
        await status(page, /WaterWall core running/);
        await progressIndicator.waitFor({ state: 'hidden', timeout });
        await expectCoreTextColor('running', runningColor);

        await press('restart');
        await status(page, /Confirm restart\./);
        assert.equal(preview.actions.length, 3, 'Opening the confirmation leaves WaterWall running.');
        await click(page, 670, (await dialogFrame(page)).bottom - 25);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(preview.actions.length, 3, 'Cancelling does not restart WaterWall.');

        await press('restart');
        await status(page, /Confirm restart\./);
        await confirmDialog(page, (x, y) => click(page, x, y));
        const interrupted = await preview.action(3);
        assertRequest(interrupted.request, { action: 'restart_core' });
        await status(page, /Confirm restart\..*Operation in progress\./);
        await preview.disconnect();
        await status(page, /Preview disconnected\. Showing last reported values\./);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await progressIndicator.waitFor({ state: 'hidden', timeout });
        const transportError = 'Request to restart core: The connection closed before Wave confirmed the request.';
        await expectLoggedOnce(page, transportError);
        await status(page, /4 log entries\. Latest log: Request to restart core/);

        const reconnected = 'Preview fixture reconnected.';
        data.logs.push({ source: 'panel', ts: '09:41:05', level: 'success', message: reconnected });
        const reconnectRows = await verticalRuns(page, 510, 400, 820, [69, 64, 79], 20);
        assert.ok(reconnectRows.length, 'Expected the Reconnect control.');
        const [top, bottom] = reconnectRows.at(-1);
        await click(page, 510, (top + bottom) / 2);
        await expectLoggedOnce(page, reconnected);
        await status(page, /5 log entries\. Latest log: Preview fixture reconnected\./);

        await press('restart');
        await status(page, /Confirm restart\./);
        const restartFrame = await dialogFrame(page);
        assert.equal(await modalSpinner(restartFrame), false, 'Confirmation starts without a spinner.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const restart = await preview.action(4);
        assertRequest(restart.request, { action: 'restart_core' });
        await status(page, /Confirm restart\..*Operation in progress\./);
        assert.equal(await modalSpinner(restartFrame), true, 'The spinner appears in the modal footer.');
        assert.deepEqual(await dialogFrame(page), restartFrame, 'Progress does not add a line to the dialog.');
        core.state = 'stopping';
        restart.reply();
        await status(page, /WaterWall core stopping.*Confirm restart\..*Operation in progress\./);
        assert.equal(await modalSpinner(restartFrame), true, 'The modal stays busy after acknowledgement.');
        core.state = 'starting';
        await preview.broadcast();
        await status(page, /WaterWall core starting.*Confirm restart\..*Operation in progress\./);
        assert.equal(await page.locator('#panel-controls').evaluate(controls => controls.inert), true);
        assert.equal(preview.actions.length, 5, 'Restart remains one operation across both transitional states.');
        core.state = 'running';
        await preview.broadcast();
        await status(page, /WaterWall core running/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await progressIndicator.waitFor({ state: 'hidden', timeout });

        await press('stop');
        await status(page, /Confirm stop/);
        assert.equal(preview.actions.length, 5, 'Opening the confirmation leaves WaterWall running.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const stop = await preview.action(5);
        assertRequest(stop.request, { action: 'stop_core' });
        await status(page, /Confirm stop\..*Operation in progress\./);
        core.state = 'stopped';
        stop.reply();
        await status(page, /WaterWall core stopped/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await progressIndicator.waitFor({ state: 'hidden', timeout });

        const clearLogs = () => click(page, 753, 849);
        await clearLogs();
        const cleared = await preview.action(6);
        assertRequest(cleared.request, { action: 'clear_logs' });
        await status(page, /5 log entries\. Latest log: Preview fixture reconnected\..*Operation in progress\./);
        await clearLogs();
        assert.equal(preview.actions.length, 7, 'Clear sends one request while waiting for acknowledgement.');
        data.logs = [];
        cleared.reply({ feedback: 'Logs cleared.' });
        await status(page, /0 log entries\./);
        await progressIndicator.waitFor({ state: 'hidden', timeout });
    } finally {
        await page.close();
    }
});

test('checks for WaterWall releases and switches core versions only after stopping', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture();
    data.server.version = '9.4.2';
    const { core, panel } = data.server;
    const preview = await mockPreview(page, data);
    const newVersion = '1.6.1';
    const initialRevision = 1;
    const updatedRevision = 2;
    try {
        await page.goto(workspace.url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core running\. Core version 1\.6\.0\. Target version 1\.6\.0\./);
        await preview.broadcast({ schematic_revision: initialRevision });
        const running = await versionControls(page);
        await click(page, 525, running.checkY);
        const checking = await preview.action(0);
        assertRequest(checking.request, { action: 'check_updates', schematic_revision: initialRevision });
        data.server.checking_updates = true;
        core.checking_updates = true;
        panel.checking_updates = true;
        checking.reply();
        await status(page, /Checking for updates\..*Operation in progress\./);
        await click(page, 525, running.checkY);
        assert.equal(preview.actions.length, 1, 'The check stays disabled while discovery runs.');

        Object.assign(core, {
            checking_updates: false, latest_version: newVersion, update_available: true,
            available_versions: [...core.available_versions, newVersion],
        });
        panel.checking_updates = false;
        panel.update_check_generation++;
        data.server.checking_updates = false;
        await preview.broadcast();
        await page.getByRole('img', { name: /Checking for updates\./ }).waitFor({ state: 'hidden', timeout });
        await click(page, 590, running.pickerY);
        await click(page, 590, running.pickerBottom + 58);
        await status(page, /Core version 1\.6\.0\. Target version 1\.6\.1\..*Stop WaterWall before changing its version\./);
        await click(page, 525, running.changeY);
        assert.equal(preview.actions.length, 1, 'Running WaterWall blocks an update.');
        assert.equal(await page.locator('#panel-controls').evaluate(controls => controls.inert), false);

        await lifecycleButton(page, 'stop');
        await status(page, /Confirm stop\./);
        await confirmDialog(page, (x, y) => click(page, x, y));
        const stop = await preview.action(1);
        assertRequest(stop.request, { action: 'stop_core', schematic_revision: initialRevision });
        core.state = 'stopped';
        stop.reply();
        await status(page, /WaterWall core stopped\./);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        const stopped = await versionControls(page);
        await click(page, 525, stopped.changeY);
        await status(page, /Confirm core version change to 1\.6\.1\./);
        assert.equal(preview.actions.length, 2, 'Choosing a version waits for confirmation.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const update = await preview.action(2);
        assertRequest(update.request, { action: 'update_core', version: newVersion, schematic_revision: initialRevision });
        core.update_target = newVersion;
        update.reply();
        await status(page, /Updating core to 1\.6\.1\..*Confirm core version change to 1\.6\.1\..*Operation in progress\./);
        assert.equal(await page.locator('#panel-controls').evaluate(controls => controls.inert), true);

        delete core.update_target;
        const failure = 'The WaterWall archive failed verification.';
        core.update_error = failure;
        data.logs.push({ source: 'panel', ts: '09:41:04', level: 'error', message: failure });
        await preview.broadcast();
        await expectLoggedOnce(page, failure);
        await status(page, /Core version 1\.6\.0\. Target version 1\.6\.1\./);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        const failed = await versionControls(page);
        await click(page, 525, failed.changeY);
        await status(page, /Confirm core version change to 1\.6\.1\./);
        await confirmDialog(page, (x, y) => click(page, x, y));
        const retry = await preview.action(3);
        assertRequest(retry.request, { action: 'update_core', version: newVersion, schematic_revision: initialRevision });
        core.update_target = newVersion;
        delete core.update_error;
        retry.reply();
        await status(page, /Updating core to 1\.6\.1\./);

        core.version = newVersion;
        core.update_available = false;
        delete core.update_target;
        await preview.broadcast({
            schematic_revision: updatedRevision, schematic: { ...workspace.waterwallSchematic, version: newVersion },
        });
        await status(page, /WaterWall core stopped\. Core version 1\.6\.1\. Target version 1\.6\.1\./);
        await status(page, /Server Information\..*Core version 1\.6\.1\. Latest core 1\.6\.1\./);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Core update available\./);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await page.getByRole('img', { name: /Operation in progress\./ }).waitFor({ state: 'hidden', timeout });
        assert.equal(preview.actions.length, 4, 'Switching versions leaves the core stopped.');
        await lifecycleButton(page, 'start');
        const start = await preview.action(4);
        assertRequest(start.request, { action: 'start_core', schematic_revision: updatedRevision });
    } finally {
        await page.close();
    }
});

test('logs WaterWall update errors without moving Panel Controls or introducing scrolling', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    data.settings.tls = tlsFixture();
    const preview = await mockPreview(page, data);
    const controlRows = () => verticalRuns(page, 510, 400, 820, [69, 64, 79], 20);
    const documentSize = () => page.evaluate(() => ({
        scrollHeight: document.documentElement.scrollHeight,
        clientWidth: document.documentElement.clientWidth,
    }));
    try {
        await page.goto(workspace.url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core stopped\./);
        await page.mouse.move(0, 0);
        await settleCanvas(page);
        const initialRows = await controlRows();
        assert.ok(initialRows.length >= 2, 'The update check and Backup controls are visible.');
        const initialSize = await documentSize();
        for (const [key, message] of [
            ['update_check_error', 'Could not fetch the Wave schematic list from GitHub.'],
            ['update_error', 'Could not update WaterWall. The WaterWall archive failed its SHA-256 check.'],
        ]) {
            data.server.core[key] = message;
            data.logs.push({ source: 'panel', ts: '09:41:04', level: 'error', message });
            await preview.broadcast();
            await status(page, `Latest log: ${message}`);
            await settleCanvas(page);
            assert.deepEqual(await controlRows(), initialRows, 'Panel Controls rows retain their positions.');
            assert.deepEqual(await documentSize(), initialSize, 'Update errors keep the same document geometry.');
            await expectLoggedOnce(page, message);
        }
    } finally {
        await page.close();
    }
});

test('checks both versions from Panel Controls and shows themed update dots in Server Information', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    data.server.version = '9.4.2';
    const { core, panel } = data.server;
    Object.assign(panel, { update_check_generation: 1, latest_version: '9.4.3', update_available: true });
    const preview = await mockPreview(page, data);
    const buttonX = 525;
    const busy = page.getByRole('img', { name: /Checking for updates\./ });
    const modal = page.getByRole('img', { name: /Panel update available\. Wave .* is available\./ });
    const startCheck = async (index, checkY) => {
        await click(page, buttonX, checkY);
        const check = await preview.action(index);
        assertRequest(check.request, { action: 'check_updates' });
        data.server.checking_updates = true;
        panel.checking_updates = true;
        core.checking_updates = true;
        delete panel.update_check_error;
        check.reply();
        await busy.waitFor({ timeout });
    };
    const finishPanelCheck = async () => {
        panel.checking_updates = false;
        panel.update_check_generation++;
        await preview.broadcast();
    };
    const finishCoreCheck = async () => {
        core.checking_updates = false;
        data.server.checking_updates = false;
        await preview.broadcast();
        await busy.waitFor({ state: 'hidden', timeout });
    };
    try {
        await page.addInitScript(() => {
            window.openedLinks = [];
            window.open = (...args) => {
                window.openedLinks.push(args);
                return null;
            };
        });
        await page.goto(workspace.url);
        await openView(page, 'Server Status');
        await status(page, /Server Information\. Panel version 9\.4\.2\. Latest panel 9\.4\.3\. Panel update available\. Core version 1\.6\.0\. Latest core 1\.6\.0\./);
        await status(page, /Check for updates\. Change core version\./);
        assert.equal(await modal.count(), 0, 'A check from an earlier session does not open a dialog.');
        const [title] = await verticalRuns(page, 220, 250, 650, [43, 39, 48], 14);
        assert.ok(title, 'Expected the Server Information title.');
        assert.deepEqual(await verticalRuns(page, 220, title[1], title[1] + 280, [69, 64, 79], 20), [],
            'Server Information has no standalone update check button.');
        const infoBody = { x: 199, y: title[1], width: 260, height: 280 };
        const defaultWarning = [255, 195, 106];
        const updatePixels = () => rectangleColorCount(page, infoBody, defaultWarning);
        const panelDotPixels = await updatePixels();
        assert.ok(panelDotPixels > 0, 'A newer panel version has a themed warning dot.');
        const originalControls = await versionControls(page);
        const checkY = originalControls.checkY;

        // Another browser started this combined check, and its panel worker already finished.
        data.server.checking_updates = true;
        core.checking_updates = true;
        await preview.broadcast();
        await busy.waitFor({ timeout });
        await click(page, buttonX, checkY);
        assert.equal(preview.actions.length, 0, 'The shared check stays disabled while its core worker finishes.');
        await finishCoreCheck();
        assert.equal(await modal.count(), 0, 'Another browser’s combined check does not open a dialog here.');

        await startCheck(0, checkY);
        await click(page, buttonX, checkY);
        assert.equal(preview.actions.length, 1, 'The button stays disabled while the check is running.');
        Object.assign(panel, { latest_version: '9.4.1', update_available: false });
        await finishPanelCheck();
        await status(page, /Latest panel 9\.4\.1\. Core version 1\.6\.0\. Latest core 1\.6\.0\./);
        await busy.waitFor({ timeout });
        await settleCanvas(page);
        assert.equal(await updatePixels(), 0, 'An older panel tag and an equal core version show no dots.');

        Object.assign(core, { latest_version: '1.6.1', update_available: true, available_versions: ['1.6.1', '1.6.0'] });
        await finishCoreCheck();
        await status(page, /Latest core 1\.6\.1\. Core update available\./);
        assert.equal(await modal.count(), 0, 'A newer core alone does not open the panel update dialog.');
        await settleCanvas(page);
        const coreDotPixels = await updatePixels();
        assert.ok(coreDotPixels > 0, 'A newer core version has a themed warning dot.');

        await startCheck(1, checkY);
        const failure = 'Could not fetch the latest Wave tag from GitHub.';
        panel.update_check_error = failure;
        data.logs.push({ source: 'panel', ts: '09:41:03', level: 'error', message: failure });
        await finishPanelCheck();
        await finishCoreCheck();
        await expectLoggedOnce(page, failure);
        assert.equal(await modal.count(), 0, 'A failed check does not open a dialog.');
        assert.deepEqual(await versionControls(page), originalControls, 'An update check error leaves the controls in place.');

        await startCheck(2, checkY);
        Object.assign(panel, { latest_version: '9.5.0', update_available: true });
        await finishPanelCheck();
        await status(page, /Latest panel 9\.5\.0\. Panel update available\..*Latest core 1\.6\.1\. Core update available\./);
        assert.equal(await modal.count(), 0, 'The dialog waits until the combined check finishes.');

        await page.getByRole('link', { name: 'Default', exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(3);
        assertRequest(opened.request, { action: 'get_core_config', id: 1 });
        opened.reply({ result: { config: { name: 'Default', nodes: [] }, revision: 'saved', editable: true } });
        await status(page, /^Wave Config Default\..*0 nodes\./);
        await finishCoreCheck();
        await modal.waitFor({ timeout });
        const description = await page.locator('#canvas').getAttribute('aria-label');
        assert.match(description, /^Wave Config Default\..*0 nodes\..*Panel update available\. Wave 9\.5\.0 is available\. Use the build and deployment guide to install this version\. Close\. Open build guide\.$/);
        assert.equal(description.split('Open build guide').length - 1, 1);
        await confirmDialog(page, (x, y) => click(page, x, y));
        assert.deepEqual(await page.evaluate(() => window.openedLinks), [
            ['https://github.com/radkesvat/wave/blob/main/doc/building.md#native-backend', '_blank', 'noopener,noreferrer'],
        ]);
        await click(page, 614, (await dialogFrame(page)).bottom - 25);
        await modal.waitFor({ state: 'hidden', timeout });
        assert.equal(preview.actions.length, 4, 'Opening the build guide issues no installation request.');
        await openView(page, 'Server Status');
        await settleCanvas(page);
        assert.ok(await updatePixels() > Math.max(panelDotPixels, coreDotPixels), 'Both updates can be indicated together.');

        await selectPanelTheme(page, 'Light');
        await acknowledgeThemeSave(preview, data, 4, 'light');
        await waitForTheme(page, 'f0f2f7');
        await settleCanvas(page);
        assert.equal(await updatePixels(), 0, 'The default warning color disappears after changing the theme.');
        assert.ok(await rectangleContainsColor(page, infoBody, [135, 80, 0]), 'Light mode uses its own warning color.');
    } finally {
        await page.close();
    }
});

test('opens settings field help by mouse, keyboard, and touch without reflowing inputs', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop, hasTouch: true });
    const data = fixture();
    data.settings.tls = tlsFixture();
    const preview = await mockPreview(page, data);
    const description = /Enter the lowercase DNS hostname that the certificate covers\./;
    const help = page.getByRole('img', { name: description });
    const inputRows = () => tlsInputRows(page);
    try {
        await page.goto(workspace.url);
        await openView(page, 'Settings');
        await status(page, /TLS settings\. Enable TLS on\./);
        await settleCanvas(page);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), description);
        const originalRows = await inputRows();
        assert.equal(originalRows.length, 3, 'Expected the TLS input rows.');
        const [domainTop, domainBottom] = originalRows[0];
        const marker = { x: 280, y: domainTop - 12 };

        await page.mouse.move(marker.x, marker.y);
        await help.waitFor({ timeout });
        assert.deepEqual(await inputRows(), originalRows, 'Help leaves the TLS fields in place.');
        await page.mouse.move(0, 0);
        await help.waitFor({ state: 'hidden', timeout });

        await click(page, 325, Math.floor((domainTop + domainBottom) / 2));
        await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
        await page.mouse.move(0, 0);
        await page.keyboard.press('Shift+Tab');
        await help.waitFor({ timeout });
        await page.keyboard.press('Tab');
        await help.waitFor({ state: 'hidden', timeout });

        await page.touchscreen.tap(marker.x, marker.y);
        await help.waitFor({ timeout });
        await page.mouse.move(0, 0);
        await settleCanvas(page);
        assert.match(await page.locator('#canvas').getAttribute('aria-label'), description,
            'The tapped popup stays open when the pointer leaves.');
        await page.touchscreen.tap(900, 108);
        await help.waitFor({ state: 'hidden', timeout });
        assert.deepEqual(await inputRows(), originalRows, 'Dismissing help leaves the fields in place.');
        assert.equal(preview.actions.length, 0, 'Reading field help leaves the settings draft idle.');
    } finally {
        await page.close();
    }
});

test('validates and saves WaterWall form settings alongside a separate Panel TLS draft', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture();
    data.settings.tls = tlsFixture();
    const preview = await mockPreview(page, data);
    const editWorkers = text => replaceText(page, 320, 228, text);
    const saveCore = () => click(page, 267, 557);
    const tabBodyFrame = async () => {
        const border = [138, 132, 149];
        const left = await verticalRuns(page, 199, 94, 579, border, 400);
        assert.equal(left.length, 1, 'Expected a border along the Settings tab body.');
        const [[top, bottom]] = left;
        assert.equal(top, 95, 'The body border sits directly beneath the tabs.');
        assert.deepEqual(await verticalRuns(page, desktop.width - 22, top, bottom, border), left);
        const horizontal = await verticalRuns(page, 520, top, bottom, border);
        assert.deepEqual(horizontal[0], [top, top + 1], 'The frame spans the top of the tab body.');
        assert.deepEqual(horizontal.at(-1), [bottom - 1, bottom], 'The frame spans the bottom of the tab body.');
        return left[0];
    };
    try {
        await page.goto(workspace.url);
        await openView(page, 'Settings');
        await status(page, /TLS settings\. Enable TLS on\./);
        const panelFrame = await tabBodyFrame();
        const border = [61, 58, 67];
        const edges = await horizontalRuns(page, 130, 205, desktop.width - 28, border);
        assert.equal(edges.length, 6, 'The first row contains three Settings cards.');
        const [tlsLeft, tlsRight, adminLeft, adminRight, sessionsLeft, sessionsRight] =
            edges.map(([x]) => x);
        const columns = [[tlsLeft, tlsRight], [adminLeft, adminRight], [sessionsLeft, sessionsRight]];
        const widths = columns.map(([left, right]) => right - left + 1);
        assert.ok(Math.max(...widths) - Math.min(...widths) <= 1, 'Compact Settings columns have equal widths.');
        const cardTop = async (left, right) => {
            const edge = x => verticalRuns(page, x, 101, 410, border, 80);
            const bounds = await edge(left);
            assert.equal(bounds.length, 1, 'Expected the left edge of the Settings card.');
            assert.deepEqual(await edge(right), bounds, 'The Settings card stays within its column.');
            return bounds[0][0];
        };
        const tops = [];
        for (const [left, right] of columns) {
            tops.push(await cardTop(left, right));
        }
        assert.deepEqual(tops, [106, 106, 106], 'All three compact Settings cards align at the top.');
        const fields = await tlsInputRows(page);
        assert.equal(fields.length, 3, 'Expected all enabled TLS fields.');
        const [domainTop, domainBottom] = fields[0];
        const domainInput = x => verticalRuns(page, x, domainTop, domainBottom, [13, 11, 14], 15);
        assert.deepEqual(await domainInput(212), [], 'The Domain input leaves room beside the card border.');
        assert.equal((await domainInput(220)).length, 1, 'The Domain input begins inside the card padding.');
        const tls = { enabled: true, domain: 'updated.example.invalid',
            certificate_chain: data.settings.tls.certificate_chain, private_key: data.settings.tls.private_key };
        await replaceText(page, 325, Math.floor((domainTop + domainBottom) / 2), tls.domain);
        await click(page, 281, 82);
        await status(page, /Core tab\. WaterWall core settings\. Runtime\. Version 1\.6\.0/);
        assert.deepEqual(await tabBodyFrame(), panelFrame, 'Core and Panel use the same outer frame bounds.');
        const runtimeScrolls = () => Boolean(Module._wave_scroll_window_at(620, 375));
        assert.equal(await page.evaluate(runtimeScrolls), false, 'The collapsed Runtime form fits its pane.');
        await click(page, 420, 469);
        await page.waitForFunction(runtimeScrolls, undefined, { timeout });
        await click(page, 420, 469);
        await page.waitForFunction(() => !Module._wave_scroll_window_at(620, 375), undefined, { timeout });

        await editWorkers('1.5');
        await status(page, /Worker threads.*whole number/);
        await saveCore();
        assert.equal(preview.actions.length, 0, 'An invalid worker count blocks save.');
        await click(page, 386, 557);
        await status(page, /Runtime\. Version 1\.6\.0\. \d+ log entr/);
        await saveCore();
        assert.equal(preview.actions.length, 0, 'Revert clears the rejected draft.');

        await editWorkers('4');
        await click(page, 369, 157);
        await status(page, /WaterWall core settings\. DNS/);
        const servers = '192.0.2.53,2001:db8::53';
        await replaceText(page, 840, 228, servers);
        const core = { misc: { workers: 4 }, dns: { servers } };
        await click(page, 223, 82);
        await status(page, /TLS settings\. Enable TLS on\./);
        await click(page, 245, await tlsApplyRow(page, fields.at(-1)[1]));
        await status(page, /Apply TLS settings\./);
        await confirmDialog(page, (x, y) => click(page, x, y), 370);
        const panelSave = await preview.action(0);
        assertRequest(panelSave.request, { action: 'apply_tls_settings', settings: tls, activate: true });
        data.settings.tls = { ...tls, supported: true, restart_required: true };
        const feedback = 'Saved panel TLS settings. Restart Wave to apply them.';
        panelSave.reply({ feedback, feedback_in_logs_only: true });
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await status(page, /Restart Wave to apply the saved TLS settings\./);
        await expectLoggedOnce(page, feedback);

        await click(page, 281, 82);
        await status(page, /Core tab/);
        await saveCore();
        const coreSave = await preview.action(1);
        assertRequest(coreSave.request, { action: 'apply_core_settings', settings: core });
        data.core_settings = core;
        coreSave.reply({ feedback: 'Saved core settings for the next launch.', feedback_in_logs_only: true });
        await status(page, /Core tab.*Latest log: Saved core settings/);

        data.core_settings_notice = 'Unsupported WaterWall core setting: log.level.';
        await preview.broadcast();
        await status(page, /Unsupported WaterWall core setting: log\.level/);
        await saveCore();
        const review = await preview.action(2);
        assertRequest(review.request, { action: 'apply_core_settings', settings: core });
        delete data.core_settings_notice;
        review.reply({ feedback: 'Saved core settings for the next launch.', feedback_in_logs_only: true });
        await status(page, /DNS\. Version 1\.6\.0\. 4 log entries\. Latest log: Saved core settings/);
    } finally {
        await page.close();
    }
});

test('locks disabled TLS fields and saves the change after confirmation', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture();
    data.settings.tls = tlsFixture();
    const preview = await mockPreview(page, data);
    const enabled = /TLS settings\. Enable TLS on\./;
    const disabled = /TLS settings\. Enable TLS off\. Certificate fields locked\./;
    const restart = /Restart Wave to apply the saved TLS settings\./;
    const input = page.locator('#wave-text-input');
    try {
        await page.goto(workspace.url);
        await openView(page, 'Settings');
        await status(page, enabled);
        const fields = await tlsInputRows(page);
        assert.equal(fields.length, 3);
        const [domainTop, domainBottom] = fields[0];
        const domainY = Math.floor((domainTop + domainBottom) / 2);
        const toggle = () => click(page, 226, domainTop - 40);
        const applyY = () => tlsApplyRow(page, fields.at(-1)[1]);
        const expectLockedFields = async () => {
            await status(page, disabled);
            assert.deepEqual(await tlsInputRows(page), [], 'Disabled TLS fields are dimmed.');
            await click(page, 325, domainY);
            assert.ok(await input.isHidden(), 'A disabled TLS field cannot take input.');
        };
        const confirmDisable = async index => {
            await click(page, 245, await applyY());
            await status(page, /Apply TLS settings\./);
            assert.equal(preview.actions.length, index, 'Applying waits for confirmation.');
            await confirmDialog(page, (x, y) => click(page, x, y), 370);
            const changed = await preview.action(index);
            assertRequest(changed.request, { action: 'apply_tls_settings', settings: { enabled: false } });
            await status(page, /Apply TLS settings\..*Operation in progress\./);
            return changed;
        };

        await toggle();
        await expectLockedFields();
        await click(page, 365, await applyY());
        await status(page, enabled);
        assert.deepEqual(await tlsInputRows(page), fields, 'Revert restores the enabled fields.');
        await click(page, 325, domainY);
        await input.waitFor({ state: 'visible', timeout });

        await toggle();
        const rejected = await confirmDisable(0);
        const error = 'Cannot write wave-db.json.';
        rejected.reply({ ok: false, feedback: error, feedback_in_logs_only: true });
        await expectLoggedOnce(page, error);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), restart);

        const saved = await confirmDisable(1);
        data.settings.tls = { ...data.settings.tls, enabled: false, restart_required: true };
        saved.reply({ feedback: 'Saved panel TLS settings. Restart Wave to apply them.', feedback_in_logs_only: true });
        await status(page, restart);
        await page.reload();
        await waitForServerStatus(page);
        await openView(page, 'Settings');
        await expectLockedFields();
        await status(page, restart);
        assert.equal(preview.actions.length, 2, 'Reloading restores the saved setting without resubmitting it.');
    } finally {
        await page.close();
    }
});

test('enables TLS and opens the HTTPS panel after the listener responds', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    const data = fixture();
    const { supported, restart_required, ...tls } = tlsFixture();
    data.settings.tls = { ...tls, enabled: false, supported, restart_required };
    const preview = await mockPreview(page, data);
    const https = new URL(workspace.url);
    https.protocol = 'https:';
    https.hostname = tls.domain;
    const httpsUrl = https.href;
    const probeGate = Promise.withResolvers();
    let probes = 0;
    await page.route(`${https.origin}/**`, async route => {
        if (route.request().isNavigationRequest()) {
            await route.fulfill({ contentType: 'text/html', body: '<title>HTTPS Wave fixture</title>' });
        } else {
            ++probes;
            await probeGate.promise;
            await route.fulfill({ status: 200 });
        }
    });
    try {
        await page.goto(workspace.url);
        await openView(page, 'Settings');
        await status(page, /TLS settings\. Enable TLS off\./);
        await click(page, 226, 148);
        await status(page, /TLS settings\. Enable TLS on\./);
        const fields = await tlsInputRows(page);
        assert.equal(fields.length, 3);
        const confirm = async index => {
            await click(page, 245, await tlsApplyRow(page, fields.at(-1)[1]));
            await status(page, /Apply TLS settings\./);
            assert.equal(preview.actions.length, index, 'The listener restarts only after confirmation.');
            await confirmDialog(page, (x, y) => click(page, x, y), 370);
            const action = await preview.action(index);
            assertRequest(action.request, { action: 'apply_tls_settings', settings: tls, activate: true });
            return action;
        };

        const rejected = await confirm(0);
        const error = 'The TLS private key does not match the leaf certificate.';
        rejected.reply({ ok: false, feedback: error, feedback_in_logs_only: true });
        await expectLoggedOnce(page, error);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(page.url(), workspace.url);
        assert.equal(probes, 0, 'A rejected TLS configuration leaves the current listener in use.');

        const saved = await confirm(1);
        const probe = page.waitForRequest(request => !request.isNavigationRequest() &&
            new URL(request.url()).origin === https.origin, { timeout });
        saved.reply({ result: { https_url: httpsUrl } });
        await probe;
        const transition = page.locator('#startup');
        await transition.waitFor({ timeout });
        assert.equal(await transition.textContent(), 'Switching Wave to HTTPS…');
        assert.equal(page.url(), workspace.url, 'Wave waits for the HTTPS listener before navigating.');
        probeGate.resolve();
        await page.waitForURL(httpsUrl, { timeout });
        assert.equal(await page.title(), 'HTTPS Wave fixture');
    } finally {
        probeGate.resolve();
        await page.close();
    }
});

test('edits and saves session expiry from Panel Settings', async () => {
    const page = await workspace.browser.newPage({ viewport: coreDesktop });
    const data = fixture();
    const preview = await mockPreview(page, data);
    try {
        await page.goto(workspace.url);
        await openView(page, 'Settings');
        await status(page, /Panel tab.*Session expiry 3 days\./);

        const [expiry] = await verticalRuns(page, 1080, 150, 193, [13, 11, 14], 20);
        assert.ok(expiry, 'Expected the session expiry input.');
        const editExpiry = value => replaceText(page, 1080, (expiry[0] + expiry[1]) / 2, value);
        const saveExpiry = () => click(page, 985, expiry[1] + 19);

        for (const invalid of ['1.5', '1e2']) {
            await editExpiry(invalid);
            await status(page, /Enter a whole number from 1 to 365 for session expiry\./);
            await saveExpiry();
            assert.equal(preview.actions.length, 0, 'The input requires a whole number written in days.');
        }

        await editExpiry('7');
        await saveExpiry();
        const saved = await preview.action(0);
        assertRequest(saved.request, { action: 'apply_session_settings', session_expiry_days: 7 });
        await status(page, /Operation in progress\./);
        data.settings.session_expiry_days = 7;
        saved.reply({ feedback: 'Saved session expiry for future sign-ins.', feedback_in_logs_only: true });
        await status(page, /Session expiry 7 days\./);
        await expectLoggedOnce(page, 'Saved session expiry for future sign-ins.');
        await saveExpiry();
        assert.equal(preview.actions.length, 1, 'The saved value is no longer a pending edit.');

        const wide = { ...coreDesktop, width: 1920 };
        await page.setViewportSize(wide);
        await settleCanvas(page);
        const cards = await horizontalRuns(page, 150, 246, wide.width - 24, [31, 27, 34], 280);
        assert.equal(cards.length, 4, 'All four Panel Settings cards share the top row when there is room.');
        const [left, right] = cards[3];
        const [basePath] = await verticalRuns(page, Math.floor((left + right) / 2), 150, 220, [13, 11, 14], 20);
        assert.ok(basePath, 'The Panel base path input is visible in the fourth card.');

        await page.setViewportSize({ ...portrait, height: coreDesktop.height });
        await settleCanvas(page);
        const stacked = await verticalRuns(page, 27,
            251 + donationSlotHeight, 867 + donationSlotHeight, [61, 58, 67], 20);
        assert.equal(stacked.length, 4, 'The four panel settings cards stack along the same left edge.');
    } finally {
        await page.close();
    }
});

test('renders a full-screen terminal, forwards keystrokes and resize, and closes on navigation', async () => {
    const viewport = { width: 1920, height: 978 };
    const page = await workspace.browser.newPage({ viewport });
    const preview = await mockPreview(page, fixture());
    const connected = Promise.withResolvers();
    const messages = new EventEmitter();
    const frames = [];
    let connections = 0;
    let closed = false;
    await page.routeWebSocket(workspace.consoleUrl, socket => {
        connections++;
        socket.onMessage(frame => {
            frames.push(frame);
            messages.emit('frame');
        });
        socket.onClose(() => {
            closed = true;
            messages.emit('close');
        });
        connected.resolve(socket);
    });
    const waitForFrame = async predicate => {
        const signal = AbortSignal.timeout(timeout);
        while (!frames.some(predicate)) await once(messages, 'frame', { signal });
        return frames.find(predicate);
    };
    const resize = frame => {
        if (typeof frame !== 'string') return false;
        const message = JSON.parse(frame);
        return message.type === 'resize' && message;
    };
    try {
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await openView(page, 'Console');
        assert.equal(connections, 0, 'Opening Console does not start a shell on its own.');
        const screenReader = page.getByRole('checkbox', { name: 'Screen reader output' });
        await screenReader.check();
        await page.getByRole('button', { name: 'Open terminal', exact: true }).click({ timeout });
        const initial = resize(await waitForFrame(resize));
        assert.ok(initial.cols >= 80 && initial.rows >= 24);
        const socket = await connected.promise;
        socket.send(JSON.stringify({ type: 'ready' }));
        socket.send(Buffer.from('\x1b[?1049h\x1b[2J\x1b[H\x1b[32mFixture screen 雪\x1b[0m'));

        const terminal = page.locator('#wave-console-terminal');
        const bounds = await terminal.boundingBox();
        assert.ok(bounds && bounds.height >= 400 && bounds.width >= 800, 'The terminal fills the work pane.');
        const renderedText = 'Fixture screen 雪';
        await terminal.locator('.xterm-rows').getByText(renderedText, { exact: true }).waitFor({ timeout });
        await terminal.getByRole('listitem').filter({ hasText: renderedText }).waitFor({ timeout });
        const input = terminal.locator('textarea');
        assert.ok(await input.evaluate(element => element === document.activeElement));
        await page.keyboard.press('ArrowUp');
        await page.keyboard.press('Control+c');
        await waitForFrame(frame => Buffer.isBuffer(frame) && frame.equals(Buffer.from('\x1b[A')));
        await waitForFrame(frame => Buffer.isBuffer(frame) && frame.equals(Buffer.from('\x03')));

        await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
        await page.evaluate(() => navigator.clipboard.writeText('x'.repeat(1024 * 1024 + 1)));
        const sentInputs = () => frames.filter(Buffer.isBuffer);
        const beforePaste = sentInputs().length;
        await page.keyboard.press('Control+Shift+v');
        const status = page.locator('#wave-console-status');
        await status.getByText(/Split it into pieces/).waitFor({ timeout });
        await page.keyboard.press('Tab');
        await waitForFrame(frame => Buffer.isBuffer(frame) && frame.equals(Buffer.from('\t')));
        assert.equal(sentInputs().length, beforePaste + 1, 'Oversized paste sends no partial input.');
        await status.getByText(/Ctrl\+Shift\+Space/).waitFor({ timeout });
        await page.keyboard.press('Control+Shift+Space');
        const close = page.getByRole('button', { name: 'Close terminal', exact: true });
        assert.ok(await close.evaluate(element => element === document.activeElement));
        assert.equal(closed, false);

        await page.setViewportSize({ width: 1280, height: 840 });
        await waitForFrame(frame => resize(frame)?.cols < initial.cols);
        await openView(page, 'Server Status');
        await page.locator('#wave-console').waitFor({ state: 'hidden', timeout });
        if (!closed) await once(messages, 'close', { signal: AbortSignal.timeout(timeout) });
        assert.equal(preview.actions.length, 0, 'Terminal input bypasses the preview socket.');
    } finally {
        await page.close();
    }
});

test('reserves space for a whole paste until the terminal acknowledges input bytes', async () => {
    const page = await workspace.browser.newPage({ viewport: desktop });
    try {
        await mockPreview(page, fixture());
        const inputs = [];
        let initialResize;
        let socket;
        await page.routeWebSocket(workspace.consoleUrl, route => {
            socket = route;
            route.onMessage(frame => {
                if (typeof frame === 'string') {
                    if (!initialResize) {
                        initialResize = JSON.parse(frame);
                        route.send(JSON.stringify({ type: 'ready', input_window: 5 }));
                    }
                } else {
                    inputs.push(frame.toString());
                    route.send(Buffer.from(`Received ${inputs.length}\r\n`));
                }
            });
        });
        await page.goto(workspace.url);
        await waitForServerStatus(page);
        await openView(page, 'Console');
        await page.getByRole('button', { name: 'Open terminal', exact: true }).click({ timeout });
        const status = page.locator('#wave-console-status');
        await status.getByText(/Connected to host shell/).waitFor({ timeout });
        assert.equal(initialResize.input_ack, true);
        await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
        await page.evaluate(() => navigator.clipboard.writeText('雪'));
        const output = page.locator('#wave-console-terminal');
        await page.keyboard.press('Control+Shift+v');
        await output.getByText('Received 1', { exact: true }).waitFor({ timeout });
        await page.keyboard.press('Control+Shift+v');
        await status.getByText(/queue is full/).waitFor({ timeout });

        socket.send(JSON.stringify({ type: 'input_ack', bytes: 1 }));
        socket.send(Buffer.from('One byte consumed\r\n'));
        await output.getByText('One byte consumed', { exact: true }).waitFor({ timeout });
        await page.keyboard.press('Control+Shift+v');
        await output.getByText('Received 2', { exact: true }).waitFor({ timeout });
        assert.deepEqual(inputs, ['雪', '雪']);
        await status.getByText(/Connected to host shell/).waitFor({ timeout });
        socket.send(JSON.stringify({ type: 'input_ack', bytes: 6 }));
        await status.getByText(/Invalid terminal input acknowledgement/).waitFor({ timeout });
        await page.getByRole('button', { name: 'Open terminal', exact: true }).waitFor({ timeout });
    } finally {
        await page.close();
    }
});
