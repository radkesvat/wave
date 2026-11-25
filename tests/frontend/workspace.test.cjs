const assert = require('node:assert/strict');
const { once } = require('node:events');
const { readFile } = require('node:fs/promises');
const { createServer } = require('node:http');
const { join } = require('node:path');
const { after, before, test } = require('node:test');
const { chromium } = require('playwright-core');

const desktop = { width: 1280, height: 800 };
const portrait = { width: 390, height: 844 };
const overviewSnapshot = {
    cpu_percent: 63,
    memory_used_gib: 4.5,
    memory_total_gib: 16,
    uptime_minutes: 1599,
    download_history: [2.4, 1.2, 3, 2.8, 5.5, 4.6, 7.9, 6.8, 10.2, 8.3, 8.9, 5.7, 6.2, 9.6],
};
const user = { enabled: true, upload_rate: 0.2, download_rate: 0.5, upload_total: 8, download_total: 12,
    remaining_minutes: 430, notes: '', options: {}, configuration: 'wave-preview://user/example' };

let url;
let previewUrl;
let server;
let browser;

before(async () => {
    const siteDir = process.env.WAVE_SITE_DIR;
    assert.ok(siteDir, 'Run this test through a Wave frontend CTest preset.');
    const assets = new Map();
    for (const [path, filename, contentType] of [
        ['/', 'index.html', 'text/html; charset=utf-8'],
        ['/index.js', 'index.js', 'text/javascript; charset=utf-8'],
        ['/index.wasm', 'index.wasm', 'application/wasm'],
    ]) {
        assets.set(path, { contentType, contents: await readFile(join(siteDir, filename)) });
    }
    server = createServer((request, response) => {
        const asset = assets.get(request.url);
        if (!asset) {
            response.writeHead(404).end();
            return;
        }
        response.writeHead(200, { 'Content-Type': asset.contentType }).end(asset.contents);
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    url = `http://127.0.0.1:${server.address().port}/`;
    previewUrl = `ws://127.0.0.1:${server.address().port}/ws/preview`;
    browser = await chromium.launch({ args: ['--enable-unsafe-swiftshader', '--disable-smooth-scrolling'] });
});

after(async () => {
    try {
        await browser?.close();
    } finally {
        if (server?.listening) {
            const closed = once(server, 'close');
            server.close();
            await closed;
        }
    }
});

function fixture() {
    return {
        users: [{ ...user, id: 1, name: 'Avery Brooks' }, { ...user, id: 2, name: 'Bella Chen' }],
        users_live: true,
        logs: [{ ts: '09:41:02', level: 'info', message: 'Preview fixture is ready.' }],
        server: overviewSnapshot, settings: {}, topology: {}, bot: {}, console: {},
    };
}

async function mockPreview(page, { gate = Promise.resolve(), handle = () => '' } = {}) {
    const data = fixture();
    await page.routeWebSocket(previewUrl, async socket => {
        socket.onMessage(raw => {
            const request = JSON.parse(raw);
            const feedback = handle(data, request);
            socket.send(JSON.stringify({ type: 'snapshot', data, action: request.action, ok: true, feedback }));
        });
        await gate;
        socket.send(JSON.stringify({ type: 'snapshot', data }));
    });
}

async function waitForCanvas(page, viewport, scrolls) {
    await page.waitForFunction(({ viewport, scrolls }) => {
        const canvas = document.querySelector('canvas[data-ready]');
        if (!canvas) return false;
        const { width, height } = canvas.getBoundingClientRect();
        const gl = canvas.getContext('webgl2');
        return innerWidth === viewport.width && width === document.body.clientWidth &&
            (scrolls ? height > viewport.height : height === viewport.height) &&
            gl?.drawingBufferWidth === Math.floor(width * devicePixelRatio) &&
            gl.drawingBufferHeight === Math.floor(height * devicePixelRatio);
    }, { viewport, scrolls });
}

async function openView(page, name) {
    const link = page.getByRole('link', { name, exact: true });
    await link.focus();
    await page.keyboard.press('Enter');
    await page.waitForFunction(name => document.querySelector('[aria-current="page"]')?.textContent === name, name);
}

test('opens Users, supports keyboard navigation, and resizes at 2× DPI', async () => {
    const page = await browser.newPage({ viewport: desktop, deviceScaleFactor: 2 });
    let releaseSnapshot;
    const gate = new Promise(resolve => { releaseSnapshot = resolve; });
    const errors = [];
    const requests = new Set();
    page.on('pageerror', error => errors.push(error.message));
    page.on('request', request => requests.add(request.url()));
    await mockPreview(page, { gate });

    try {
        assert.equal((await page.goto(url)).status(), 200);
        await waitForCanvas(page, desktop, false);
        await page.getByRole('img', { name: /Wave Users.*Loading sample data/ }).waitFor();
        releaseSnapshot();
        await page.getByRole('img', { name: /Wave Users.*WaterWall disconnected.*2 users.*Preview fixture is ready/ }).waitFor();
        assert.equal(await page.getByRole('link', { name: 'Users' }).getAttribute('aria-current'), 'page');

        for (const name of ['Server Status', 'Panel Settings', 'Console', 'Nodes & Routers', 'Telegram Bot', 'About']) {
            await openView(page, name);
            await page.getByRole('img', { name: new RegExp(`^Wave ${name}\\.`) }).waitFor();
        }
        await openView(page, 'Overview');
        await page.getByRole('img', { name: /CPU utilization 63 percent.*Memory 4\.5 of 16\.0 GiB.*Latest 9\.6 MiB/ }).waitFor();
        await openView(page, 'Users');

        await page.setViewportSize(portrait);
        await waitForCanvas(page, portrait, true);
        assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
        await page.mouse.move(190, 700);
        await page.mouse.wheel(0, 1500);
        await page.waitForFunction(() => Math.ceil(scrollY + innerHeight) >= document.body.scrollHeight);

        await page.setViewportSize(desktop);
        await waitForCanvas(page, desktop, false);
        assert.equal(await page.evaluate(() => scrollY), 0);
        assert.deepEqual(errors, []);
        assert.deepEqual(requests, new Set([url, `${url}index.js`, `${url}index.wasm`]));
    } finally {
        releaseSnapshot();
        await page.close();
    }
});

test('pastes into user search on HTTP and mutates the matching row and Logs', async () => {
    const page = await browser.newPage({ viewport: desktop });
    await page.context().grantPermissions(['clipboard-read', 'clipboard-write'], { origin: url });
    const actions = [];
    await mockPreview(page, { handle(data, action) {
        actions.push(action);
        const changed = data.users.find(row => row.id === action.id);
        if (!changed || action.action !== 'set_user_enabled') return 'Unexpected request.';
        changed.enabled = action.enabled;
        const message = `Disabled ${changed.name} in the preview.`;
        data.logs.push({ ts: '09:41:13', level: 'success', message });
        return message;
    } });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await page.mouse.click(540, 104);
        await page.locator('#wave-text-input').waitFor({ state: 'visible' });
        await page.keyboard.type('unmatched');
        await page.keyboard.press('Control+a');
        await page.evaluate(() => navigator.clipboard.writeText('Bella'));
        await page.keyboard.press('Control+v');
        await page.mouse.click(569, 165);
        await page.getByRole('img', { name: /2 log entries. Latest log: Disabled Bella Chen/ }).waitFor();
        assert.deepEqual(actions, [{ action: 'set_user_enabled', id: 2, enabled: false }]);
    } finally {
        await page.close();
    }
});

test('narrow touch selects the workspace and swipes starting on Menu scroll without opening it', async () => {
    const page = await browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
    await mockPreview(page);
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await waitForCanvas(page, portrait, true);

        await page.touchscreen.tap(70, 105);
        await page.getByRole('img', { name: /Wave Server Status/ }).waitFor();
        await page.touchscreen.tap(230, 105);
        await page.getByRole('img', { name: /Wave Users/ }).waitFor();

        const touch = await page.context().newCDPSession(page);
        const x = 130;
        await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y: 34 }] });
        for (const y of [22, 13, 5]) {
            await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x, y }] });
        }
        await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        await page.waitForFunction(() => scrollY > 0);
        assert.equal(await page.getByRole('button', { name: 'Menu' }).getAttribute('aria-expanded'), 'false');
        assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    } finally {
        await page.close();
    }
});

test('first-run setup shows progress, validates input, and completes through the preview socket', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const actions = [];
    await mockPreview(page, { handle(_data, action) {
        actions.push(action);
        return 'Applied demo setup.';
    } });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await page.mouse.click(132, 23);
        await page.waitForFunction(() => document.getElementById('menu').getAttribute('aria-expanded') === 'true');
        await page.mouse.click(170, 160);
        await page.getByRole('img', { name: /Wave first-run setup. Step 1 of 3/ }).waitFor();
        await page.mouse.click(449, 244);
        await page.getByRole('img', { name: /Step 2 of 3/ }).waitFor();
        await page.mouse.click(495, 294);
        await page.getByRole('img', { name: /Step 3 of 3/ }).waitFor();
        await page.mouse.click(511, 327);
        await page.getByRole('img', { name: /Enter a demo username and password/ }).waitFor();
        assert.equal(actions.length, 0);
        await page.mouse.click(458, 294);
        await page.mouse.click(511, 327);
        await page.getByRole('img', { name: /Applied demo setup/ }).waitFor();
        assert.deepEqual(actions, [{ action: 'setup', name: 'Wave Preview', domain: 'wave.example',
            username: 'demo', password: 'wave-demo' }]);
    } finally {
        await page.close();
    }
});

test('reports unavailable sample data after a malformed preview snapshot', async () => {
    const page = await browser.newPage();
    await page.routeWebSocket(previewUrl, socket => socket.send('{"type":"snapshot","data":{}}'));
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*Sample data unavailable/ }).waitFor();
        assert.equal(await page.getByRole('link', { name: 'Users' }).getAttribute('aria-current'), 'page');
    } finally {
        await page.close();
    }
});

test('shows startup feedback and reports a missing Wasm asset', async () => {
    const page = await browser.newPage();
    let releaseWasm;
    const loading = new Promise(resolve => { releaseWasm = resolve; });
    await page.route('**/index.wasm', async route => {
        await loading;
        await route.abort();
    });
    try {
        await page.goto(url, { waitUntil: 'domcontentloaded' });
        assert.match(await page.getByRole('status').innerText(), /Loading Wave/);
        releaseWasm();
        await page.getByText(/Wave could not start/).waitFor({ state: 'visible' });
        assert.equal(await page.locator('canvas').isVisible(), false);
    } finally {
        releaseWasm();
        await page.close();
    }
});
