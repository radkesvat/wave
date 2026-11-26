const assert = require('node:assert/strict');
const { test } = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { chromium } = require('playwright-core');
const { authFixture, timeout } = require('../backend/auth_fixture.cjs');

const viewport = { width: 1280, height: 800 };

function waitForStatus(page, name) {
    return page.getByRole('img', { name }).waitFor({ timeout });
}

function settleCanvas(page) {
    return page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

async function visibleInputs(page, top, bottom) {
    await page.mouse.move(0, 0);
    await settleCanvas(page);
    const x = 780;
    const image = await page.screenshot({ clip: { x, y: top, width: 1, height: bottom - top } });
    return page.evaluate(async ({ content, x, top }) => {
        const bitmap = await createImageBitmap(new Blob([Uint8Array.from(atob(content), c => c.charCodeAt(0))]));
        const canvas = new OffscreenCanvas(1, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const pixels = context.getImageData(0, 0, 1, canvas.height).data;
        const positions = [];
        let start = -1;
        for (let y = 0; y <= canvas.height; y++) {
            const inset = pixels[4 * y] === 13 && pixels[4 * y + 1] === 11 && pixels[4 * y + 2] === 14;
            if (inset && start === -1) start = y;
            if (!inset && start !== -1) {
                if (y - start >= 18 && y - start < 36) positions.push({ x, y: top + Math.floor((y + start) / 2) });
                start = -1;
            }
        }
        return positions;
    }, { content: image.toString('base64'), x, top });
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

test('authenticates an administrator and closes the terminal on sign-out', async t => {
    assert.ok(process.env.WAVE_SITE_DIR, 'Set WAVE_SITE_DIR to a built Wave frontend.');
    const fixture = await authFixture(t, process.env.WAVE_SITE_DIR, { encrypted: false });
    await fixture.start();
    const browser = await chromium.launch({ args: ['--enable-unsafe-swiftshader'] });
    t.after(() => browser.close());
    const context = await browser.newContext({ viewport });
    await context.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: fixture.origin });
    const page = await context.newPage();
    const credentials = { username: 'fixture-browser', password: 'fake browser test passphrase 🔒' };

    await page.goto(fixture.origin + '/panel');
    await waitForStatus(page, 'Set up administrator.');
    assert.equal((await context.request.get(fixture.origin + '/terminal/xterm.js')).status(), 401);
    const setup = page.waitForRequest('**/api/auth/setup', { timeout });
    await submitCredentials(page, credentials, true);
    assert.deepEqual((await setup).postDataJSON(), credentials);

    await waitForStatus(page, 'Sign in to Wave.');
    await submitCredentials(page, credentials);
    const workspace = /Wave Server Status.*CPU/;
    await waitForStatus(page, workspace);
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
    await page.mouse.click(172, 135, { delay: 40 });
    await page.waitForFunction(() => document.getElementById('panel-controls').inert, null, { timeout });
    const signOut = page.waitForResponse('**/api/auth/sign-out', { timeout });
    await page.mouse.click(757, 426, { delay: 40 });
    assert.equal((await signOut).status(), 204);
    await waitForStatus(page, 'Sign in to Wave.');
    if (shellPid) await waitForShellExit(shellPid);
    await page.goto(fixture.origin + '/panel');
    await waitForStatus(page, 'Sign in to Wave.');
});
