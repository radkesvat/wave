const assert = require('node:assert/strict');
const { readFile, writeFile } = require('node:fs/promises');
const { test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { chromium } = require('playwright-core');
const { authFixture, timeout } = require('../backend/auth_fixture.cjs');

const viewport = { width: 1280, height: 800 };

function waitForStatus(page, name) {
    return page.getByRole('img', { name, exact: true }).waitFor({ timeout });
}

function settleCanvas(page) {
    return page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

async function matchingPixels(page, clip, color) {
    const image = await page.screenshot({ clip });
    return page.evaluate(async ({ content, color }) => {
        const bitmap = await createImageBitmap(new Blob([Uint8Array.from(atob(content), c => c.charCodeAt(0))]));
        const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const pixels = context.getImageData(0, 0, bitmap.width, bitmap.height).data;
        const [r, g, b] = color;
        const matches = [];
        for (let i = 0; i < pixels.length; i += 4) {
            if (pixels[i] === r && pixels[i + 1] === g && pixels[i + 2] === b) matches.push(i / 4);
        }
        return matches;
    }, { content: image.toString('base64'), color });
}

async function expectEntryTitleStrip(page, title) {
    await settleCanvas(page);
    const { width } = page.viewportSize();
    const pixels = title ? Array.from({ length: 29 }, (_, row) => row) : [];
    for (const x of [0, width - 1]) {
        assert.deepEqual(await matchingPixels(page, { x, y: 48, width: 1, height: 30 }, [26, 23, 28]), pixels,
            title ? `${title} reaches both edges of the viewport.` : 'The entry title strip is absent.');
    }
    if (title) {
        const label = await matchingPixels(page, { x: 12, y: 48, width: 96, height: 29 }, [234, 235, 255]);
        assert.ok(label.length, `${title} text is visible in the strip.`);
    }
}

async function visibleInputs(page, top, bottom) {
    await page.mouse.move(0, 0);
    await settleCanvas(page);
    const x = 780;
    const pixels = new Set(await matchingPixels(page, { x, y: top, width: 1, height: bottom - top }, [13, 11, 14]));
    const positions = [];
    let start = -1;
    for (let y = 0; y <= bottom - top; y++) {
        const inset = pixels.has(y);
        if (inset && start === -1) start = y;
        if (!inset && start !== -1) {
            if (y - start >= 18 && y - start < 36) positions.push({ x, y: top + Math.floor((y + start) / 2) });
            start = -1;
        }
    }
    return positions;
}

async function entryDialogFrame(page) {
    await settleCanvas(page);
    const { width, height } = page.viewportSize();
    const y = 110;
    const outline = await matchingPixels(page, { x: Math.floor(width / 2), y, width: 1, height: height - y },
        [138, 132, 149]);
    assert.ok(outline.length >= 2, 'Expected the entry dialog outline.');
    return { top: y + outline[0], bottom: y + outline.at(-1) + 1 };
}

function entryDialogWidth(page) {
    return Math.min(500, page.viewportSize().width - 24);
}

async function entrySpinner(page, frame) {
    const pixels = await matchingPixels(page, {
        x: Math.floor((page.viewportSize().width - entryDialogWidth(page)) / 2) + 10,
        y: frame.bottom - 42, width: 40, height: 34,
    }, [81, 165, 246]);
    return pixels.length > 0;
}

async function expectFooterError(page, frame) {
    const { width } = page.viewportSize();
    const dialogWidth = entryDialogWidth(page);
    const height = frame.bottom - frame.top;
    const line = await matchingPixels(page, {
        x: Math.floor(width / 2), y: frame.top, width: 1, height,
    }, [61, 58, 67]);
    assert.ok(line.length, 'Expected the footer divider.');

    const error = await matchingPixels(page, {
        x: Math.floor((width - dialogWidth) / 2), y: frame.top, width: dialogWidth, height,
    }, [241, 127, 111]);
    assert.ok(error.length, 'Expected visible sign-in rejection text.');
    assert.ok(error.every(pixel => Math.floor(pixel / dialogWidth) > line.at(-1)),
        'The sign-in error is below the footer divider.');
}

async function holdFirstAuthRequest(page, endpoint) {
    const gate = Promise.withResolvers();
    let requests = 0;
    await page.route(endpoint, async route => {
        if (++requests === 1) await gate.promise;
        await route.continue();
    });
    return { release: gate.resolve, requests: () => requests };
}

async function fillInput(page, field, text, paste = false) {
    await page.mouse.click(field.x, field.y, { delay: 40 });
    await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
    if (paste) {
        await page.evaluate(text => navigator.clipboard.writeText(text), text);
        await page.keyboard.press('Control+v');
    } else {
        await page.keyboard.insertText(text);
    }
    await settleCanvas(page);
}

async function submitCredentials(page, { username, password }, pastePassword = false) {
    const fields = await visibleInputs(page, 110, viewport.height - 30);
    assert.equal(fields.length, 2, 'The entry dialog has two visible credential fields.');
    await fillInput(page, fields[0], username);
    await fillInput(page, fields[1], password, pastePassword);
    await page.keyboard.press('Enter');
}

async function submitBasePath(page, value) {
    const [field] = await visibleInputs(page, 110, viewport.height - 30);
    assert.ok(field, 'The base path dialog has a visible field.');
    if (value === undefined) {
        await page.mouse.click(field.x, field.y, { delay: 40 });
        await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
    } else {
        await fillInput(page, field, value);
    }
    await page.keyboard.press('Enter');
}

async function waitForShellExit(pid) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        try {
            process.kill(pid, 0);
        } catch (error) {
            assert.equal(error.code, 'ESRCH');
            return;
        }
        await delay(20);
    }
    assert.fail(`Console shell ${pid} is still running after sign-out.`);
}

test('sets a panel base path, authenticates an administrator and closes the terminal on sign-out', async t => {
    assert.ok(process.env.WAVE_SITE_DIR, 'Set WAVE_SITE_DIR to a built Wave frontend.');
    const fixture = await authFixture(t, process.env.WAVE_SITE_DIR, { encrypted: false, basePath: null });
    await fixture.start();
    const browser = await chromium.launch({ args: ['--enable-unsafe-swiftshader'] });
    t.after(() => browser.close());
    const context = await browser.newContext({ viewport });
    await context.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: fixture.origin });
    const page = await context.newPage();
    const base = '/control-room/wave';
    const credentials = { username: 'fixture-browser', password: 'fake browser test passphrase 5Vx2m 🔒' };
    const sessionGate = Promise.withResolvers();
    await page.routeWebSocket('**/ws/panel', socket => {
        socket.connectToServer().onMessage(async message => {
            await sessionGate.promise;
            socket.send(message);
        });
    });

    await page.goto(fixture.origin + '/');
    await waitForStatus(page, 'Panel base path. Setup.');
    await expectEntryTitleStrip(page, 'Setup');
    assert.equal(new URL(page.url()).pathname, '/_wave-setup/base-path');
    const baseEndpoint = '**/_wave-setup/api/auth/base-path';
    await page.route(baseEndpoint, route => route.fulfill({ status: 400 }), { times: 1 });
    const defaultPath = page.waitForResponse(baseEndpoint, { timeout });
    await submitBasePath(page);
    const rejectedPath = await defaultPath;
    assert.equal(rejectedPath.status(), 400);
    assert.deepEqual(rejectedPath.request().postDataJSON(), { 'panel-base-path': '/panel' });
    await waitForStatus(page, /^Panel base path\.(?! Operation in progress\.)/);
    const chosenPath = page.waitForResponse(baseEndpoint, { timeout });
    await submitBasePath(page, base);
    assert.equal((await chosenPath).status(), 201);
    await waitForStatus(page, 'Set up administrator. Setup.');
    await expectEntryTitleStrip(page, 'Setup');
    assert.equal(new URL(page.url()).pathname, base + '/setup');
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8'))['panel-base-path'], base);
    assert.equal((await context.request.get(fixture.origin + base + '/terminal/xterm.js')).status(), 401);
    for (const path of ['/', '/panel', '/api/session', '/_wave-setup/base-path', base + '-other/']) {
        const outside = await context.request.get(fixture.origin + path);
        assert.equal(outside.status(), 404, path);
        assert.match(await outside.text(), /<h1>404 Not Found<\/h1>/);
    }
    const setupEndpoint = '**/api/auth/setup';
    const initialSetupFrame = await entryDialogFrame(page);
    assert.equal(await entrySpinner(page, initialSetupFrame), false, 'The setup dialog starts idle.');
    const heldSetup = await holdFirstAuthRequest(page, setupEndpoint);
    let rejectedSetup;
    try {
        await submitCredentials(page, { ...credentials, password: 'Password1' });
        await waitForStatus(page, 'Set up administrator. Operation in progress. Setup.');
        assert.deepEqual(await entryDialogFrame(page), initialSetupFrame, 'Setup keeps its dialog size while pending.');
        assert.equal(await entrySpinner(page, initialSetupFrame), true, 'Setup progress appears in the lower-left footer.');
        await page.mouse.click(820, initialSetupFrame.bottom - 25, { delay: 40 });
        await page.keyboard.press('Enter');
        await settleCanvas(page);
        assert.equal(heldSetup.requests(), 1, 'Setup sends one request while it is pending.');
        rejectedSetup = page.waitForResponse(setupEndpoint, { timeout });
    } finally {
        heldSetup.release();
    }
    assert.equal((await rejectedSetup).status(), 422);
    await waitForStatus(page, 'Set up administrator. Choose a less predictable password. A longer password may help. Setup.');
    assert.equal(await entrySpinner(page, await entryDialogFrame(page)), false, 'The rejected setup returns to idle.');
    const setup = page.waitForRequest(setupEndpoint, { timeout });
    await submitCredentials(page, credentials, true);
    assert.deepEqual((await setup).postDataJSON(), credentials);

    await waitForStatus(page, 'Sign in to Wave. Welcome.');
    await expectEntryTitleStrip(page, 'Welcome');
    assert.equal(heldSetup.requests(), 2, 'Retrying the rejected setup reaches the backend.');
    const signInFrame = await entryDialogFrame(page);
    const signInEndpoint = '**/api/auth/sign-in';
    const rejectedSignIn = page.waitForResponse(signInEndpoint, { timeout });
    await submitCredentials(page, { ...credentials, password: 'different fixture passphrase 88' });
    assert.equal((await rejectedSignIn).status(), 401);
    const cooldown = delay(1100);
    const signInError = 'Sign in to Wave. The username or password was rejected. Welcome.';
    await waitForStatus(page, signInError);
    const rejectionFrame = await entryDialogFrame(page);
    await expectFooterError(page, rejectionFrame);
    assert.equal(await entrySpinner(page, rejectionFrame), false, 'The rejected sign-in returns to idle.');
    await page.setViewportSize({ width: 324, height: viewport.height });
    await waitForStatus(page, signInError);
    await expectEntryTitleStrip(page, 'Welcome');
    await expectFooterError(page, await entryDialogFrame(page));
    await page.setViewportSize(viewport);
    // The backend throttles attempts for one second following a rejected sign-in.
    await cooldown;

    const heldSignIn = await holdFirstAuthRequest(page, signInEndpoint);
    let acceptedSignIn;
    try {
        await submitCredentials(page, credentials);
        await waitForStatus(page, 'Sign in to Wave. Operation in progress. Welcome.');
        assert.deepEqual(await entryDialogFrame(page), signInFrame, 'Sign-in keeps its dialog size while pending.');
        assert.equal(await entrySpinner(page, signInFrame), true, 'Sign-in progress appears in the lower-left footer.');
        const progressText = await matchingPixels(page, {
            x: (viewport.width - 500) / 2 + 50, y: signInFrame.bottom - 42, width: 300, height: 34,
        }, [81, 165, 246]);
        assert.equal(progressText.length, 0, 'The sign-in footer shows progress only with the spinner.');
        acceptedSignIn = page.waitForResponse(signInEndpoint, { timeout });
    } finally {
        heldSignIn.release();
    }
    assert.equal((await acceptedSignIn).status(), 204);
    try {
        await waitForStatus(page, 'Checking the session…');
        await expectEntryTitleStrip(page);
        const indicator = await matchingPixels(page, { x: 12, y: 60, width: 24, height: 36 }, [81, 165, 246]);
        assert.equal(indicator.length, 0, 'The page body stays clear while checking the session.');
    } finally {
        sessionGate.resolve();
    }
    const workspace = /Wave Server Status.*CPU/;
    await waitForStatus(page, workspace);
    await expectEntryTitleStrip(page);
    assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /(?:Setup|Welcome)\.$/);
    await page.reload();
    await waitForStatus(page, workspace);

    let shellPid;
    if (process.platform === 'linux') {
        await page.getByRole('link', { name: 'Console', exact: true }).focus();
        await page.keyboard.press('Enter');
        await page.getByRole('button', { name: 'Open terminal', exact: true }).click({ timeout });
        const terminal = page.locator('#wave-console-terminal');
        await page.locator('#wave-console-status').getByText(/Connected to host shell/).waitFor({ timeout });
        await terminal.locator('textarea').focus();
        await page.evaluate(() => navigator.clipboard.writeText(
            'if [ -t 0 ] && [ -t 1 ]; then printf "PTY_%s:%s\\n" OK "$$"; fi'
        ));
        await page.keyboard.press('Control+Shift+v');
        await settleCanvas(page);
        await page.keyboard.press('Enter');
        const pidLine = terminal.locator('.xterm-rows').getByText(/^PTY_OK:\d+$/);
        await pidLine.waitFor({ timeout });
        shellPid = Number((await pidLine.textContent()).split(':')[1]);
        assert.ok(Number.isSafeInteger(shellPid) && shellPid > 0);
    }

    await page.mouse.click(134, 23, { delay: 40 });
    await settleCanvas(page);
    await page.mouse.click(172, 198, { delay: 40 });
    await page.waitForFunction(() => document.getElementById('panel-controls').inert, null, { timeout });
    const signOut = page.waitForResponse('**/api/auth/sign-out', { timeout });
    await page.mouse.click(757, 426, { delay: 40 });
    assert.equal((await signOut).status(), 204);
    await waitForStatus(page, 'Sign in to Wave. Welcome.');
    if (shellPid) await waitForShellExit(shellPid);
    await page.goto(fixture.origin + base + '/');
    await waitForStatus(page, 'Sign in to Wave. Welcome.');

    await fixture.stop();
    const state = JSON.parse(await readFile(fixture.database, 'utf8'));
    delete state['panel-base-path'];
    await writeFile(fixture.database, JSON.stringify(state));
    await fixture.start();
    await page.goto(fixture.origin + '/');
    await waitForStatus(page, 'Sign in to Wave. Welcome.');
    assert.equal(new URL(page.url()).pathname, '/_wave-setup/sign-in');
    await submitCredentials(page, credentials);
    await waitForStatus(page, 'Panel base path. Setup.');
    const recovered = '/recovered/wave';
    await submitBasePath(page, recovered);
    await waitForStatus(page, workspace);
    assert.equal(new URL(page.url()).pathname, recovered + '/');
    assert.equal((await context.request.get(fixture.origin + base + '/')).status(), 404);
    assert.equal(JSON.parse(await readFile(fixture.database, 'utf8'))['panel-base-path'], recovered);
});
