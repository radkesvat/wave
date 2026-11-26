const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const { mkdir } = require('node:fs/promises');
const { join } = require('node:path');
const { test } = require('node:test');
const { promisify } = require('node:util');
const { chromium } = require('playwright-core');
const { authFixture, timeout } = require('../backend/auth_fixture.cjs');

const run = promisify(execFile);
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

function authPost(page, action, input) {
    return page.evaluate(async ({ action, input }) => {
        const response = await fetch(`/api/auth/${action}`, {
            method: 'POST', credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json', 'X-Wave-CSRF': '1' },
            body: JSON.stringify(input),
        });
        return response.status;
    }, { action, input });
}

for (const encrypted of [false, true]) {
    const protocol = encrypted ? 'HTTPS' : 'HTTP';
    test(`${protocol} sets up an administrator, requires login, and restores the browser JWT after a restart`, async t => {
        assert.ok(process.env.WAVE_SITE_DIR, 'Set WAVE_SITE_DIR to a built Wave frontend.');
        const fixture = await authFixture(t, process.env.WAVE_SITE_DIR, { encrypted });
        const browserHome = join(fixture.cwd, 'browser-home');
        const args = ['--enable-unsafe-swiftshader', '--no-proxy-server'];
        if (encrypted) {
            const trustDir = join(browserHome, '.pki', 'nssdb');
            await mkdir(trustDir, { recursive: true });
            await run('certutil', ['-N', '-d', `sql:${trustDir}`, '--empty-password'], { timeout });
            await run('certutil', ['-A', '-d', `sql:${trustDir}`, '-a', '-i', fixture.rootPath,
                '-t', 'C,,', '-n', 'Wave fixture root'], { timeout });
            args.push(`--host-resolver-rules=MAP ${fixture.hostname} ${fixture.bindHost}`);
        }
        const launch = () => chromium.launchPersistentContext(join(fixture.cwd, 'profile'), {
            env: { ...process.env, HOME: browserHome, XDG_DATA_HOME: join(browserHome, '.local', 'share') },
            args, viewport,
        });
        let credentials = { username: 'fixture-browser', password: 'fake browser test passphrase 🔒' };
        await fixture.start();
        let browser = await launch();
        try {
            let page = browser.pages()[0];
            const sockets = [];
            page.on('websocket', socket => sockets.push(new URL(socket.url()).pathname));
            await page.goto(fixture.origin + '/panel');
            await waitForStatus(page, 'Set up administrator.');
            assert.equal(new URL(page.url()).pathname, '/setup');
            assert.deepEqual(sockets, []);
            if (!encrypted) {
                await browser.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: fixture.origin });
            }
            const setup = page.waitForRequest('**/api/auth/setup', { timeout });
            await submitCredentials(page, credentials, !encrypted);
            assert.deepEqual((await setup).postDataJSON(), credentials);

            await waitForStatus(page, 'Sign in to Wave.');
            assert.equal(new URL(page.url()).pathname, '/sign-in');
            assert.deepEqual(sockets, []);
            await submitCredentials(page, credentials);
            const signedIn = /Wave Users.*Sample data\./;
            await waitForStatus(page, signedIn);
            assert.equal(new URL(page.url()).pathname, '/panel');
            assert.deepEqual(sockets, ['/ws/panel', '/ws/preview']);

            const [cookie] = await browser.cookies(fixture.origin);
            assert.equal(cookie.name, encrypted ? '__Host-wave_session' : 'wave_session');
            assert.equal(cookie.secure, encrypted);
            assert.ok(cookie.httpOnly && cookie.expires > Date.now() / 1000);
            assert.match(cookie.value, /^[\w-]+\.[\w-]+\.[\w-]+$/);
            assert.equal(await page.evaluate(() => document.cookie), '');

            if (!encrypted) {
                const settings = page.getByRole('link', { name: 'Panel Settings', exact: true });
                await settings.focus();
                await page.keyboard.press('Enter');
                await waitForStatus(page, /Wave Panel Settings.*Administrator settings/);
                const fields = await visibleInputs(page, 60, 510);
                assert.equal(fields.length, 6, 'Panel Settings shows TLS settings and three administrator inputs.');
                const change = { username: 'fixture-replacement', password: 'fake replacement test passphrase' };
                await fillInput(page, fields[3], change.username);
                await fillInput(page, fields[4], credentials.password);
                await fillInput(page, fields[5], change.password);
                const submitted = page.waitForRequest('**/api/auth/admin', { timeout });
                await page.mouse.click(275, fields[5].y + 30, { delay: 40 });
                const request = await submitted;
                assert.deepEqual(request.postDataJSON(), { ...change, current_password: credentials.password });
                await waitForStatus(page, 'Sign in to Wave.');
                credentials = change;
                await submitCredentials(page, credentials);
                await waitForStatus(page, signedIn);
            }

            await page.reload();
            await waitForStatus(page, signedIn);
            await browser.close();
            browser = undefined;
            await fixture.stop();
            await fixture.start();
            browser = await launch();
            page = browser.pages()[0];
            await page.goto(fixture.origin + '/panel');
            await waitForStatus(page, signedIn);
            const logoutPage = await browser.newPage();
            assert.equal((await logoutPage.goto(fixture.origin + '/api/session')).status(), 200);
            assert.equal(await authPost(logoutPage, 'sign-out', {}), 204);
            await waitForStatus(page, 'Sign in to Wave.');
            assert.equal((await browser.cookies(fixture.origin)).length, 0);
        } finally {
            await browser?.close();
        }
    });
}
