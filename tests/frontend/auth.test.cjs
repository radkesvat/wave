const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const { mkdir } = require('node:fs/promises');
const { join } = require('node:path');
const { test } = require('node:test');
const { promisify } = require('node:util');
const { chromium } = require('playwright-core');
const { authFixture, timeout } = require('../backend/auth_fixture.cjs');

const run = promisify(execFile);

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

test('the HTTPS workspace restores an unexpired browser session and returns to sign-in on logout', async t => {
    assert.ok(process.env.WAVE_SITE_DIR, 'Set WAVE_SITE_DIR to a built Wave frontend.');
    const fixture = await authFixture(t, process.env.WAVE_SITE_DIR);
    const browserHome = join(fixture.cwd, 'browser-home');
    const trustDir = join(browserHome, '.pki', 'nssdb');
    await mkdir(trustDir, { recursive: true });
    await run('certutil', ['-N', '-d', `sql:${trustDir}`, '--empty-password'], { timeout });
    await run('certutil', ['-A', '-d', `sql:${trustDir}`, '-a', '-i', fixture.rootPath,
        '-t', 'C,,', '-n', 'Wave fixture root'], { timeout });
    const launch = () => chromium.launchPersistentContext(join(fixture.cwd, 'profile'), {
        env: { ...process.env, HOME: browserHome, XDG_DATA_HOME: join(browserHome, '.local', 'share') },
        args: ['--enable-unsafe-swiftshader', '--no-proxy-server',
            `--host-resolver-rules=MAP ${fixture.hostname} ${fixture.bindHost}`],
    });
    const credentials = { username: 'fixture-browser', password: 'fake browser test passphrase' };
    await fixture.start();
    let browser = await launch();
    try {
        let page = browser.pages()[0];
        const requests = [];
        page.on('websocket', socket => requests.push(new URL(socket.url()).pathname));
        await page.goto(fixture.origin);
        await page.getByRole('img', { name: 'Welcome to Wave. Step 1 of 3.' }).waitFor({ timeout });
        assert.equal(new URL(page.url()).pathname, '/setup');
        assert.equal(await authPost(page, 'setup', { ...credentials, bootstrap: fixture.bootstrap, acknowledged: true }), 201);

        await page.goto(fixture.origin);
        await page.getByRole('img', { name: 'Sign in to Wave.' }).waitFor({ timeout });
        assert.equal(await authPost(page, 'sign-in', credentials), 204);
        await page.goto(fixture.origin);
        const signedIn = { name: 'Wave Users. Signed in as fixture-browser.' };
        await page.getByRole('img', signedIn).waitFor({ timeout });
        assert.equal(new URL(page.url()).pathname, '/panel');
        assert.deepEqual(requests, ['/ws/panel']);
        assert.equal(await page.evaluate(() => document.cookie.includes('__Host-wave_session')), false);
        const [cookie] = await browser.cookies(fixture.origin);
        assert.equal(cookie.name, '__Host-wave_session');
        assert.ok(cookie.secure && cookie.httpOnly && cookie.expires > Date.now() / 1000);

        await page.reload();
        await page.getByRole('img', signedIn).waitFor({ timeout });
        await browser.close();
        browser = undefined;
        await fixture.stop();
        await fixture.start();
        browser = await launch();
        page = browser.pages()[0];
        await page.goto(fixture.origin + '/panel');
        await page.getByRole('img', signedIn).waitFor({ timeout });
        const logoutPage = await browser.newPage();
        assert.equal((await logoutPage.goto(fixture.origin + '/api/session')).status(), 200);
        assert.equal(await authPost(logoutPage, 'sign-out', {}), 204);
        await page.getByRole('img', { name: 'Sign in to Wave.' }).waitFor({ timeout });
        assert.equal((await browser.cookies(fixture.origin)).length, 0);
    } finally {
        await browser?.close();
    }
});
