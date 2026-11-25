const assert = require('node:assert/strict');
const { once } = require('node:events');
const { readFile } = require('node:fs/promises');
const { createServer } = require('node:http');
const { join } = require('node:path');
const { after, before, test } = require('node:test');
const { chromium } = require('playwright-core');

const desktop = { width: 1280, height: 800 };
const portrait = { width: 390, height: 844 };
let url;
let server;
let browser;

before(async () => {
    const siteDir = process.env.WAVE_SITE_DIR;
    assert.ok(siteDir, 'Run this test through a Wave CTest preset.');

    const assets = new Map();
    for (const [path, filename, contentType] of [
        ['/', 'index.html', 'text/html; charset=utf-8'],
        ['/index.js', 'index.js', 'text/javascript; charset=utf-8'],
        ['/index.wasm', 'index.wasm', 'application/wasm'],
    ]) {
        const contents = await readFile(join(siteDir, filename));
        assets.set(path, { contentType, contents });
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

    browser = await chromium.launch({
        args: ['--enable-unsafe-swiftshader', '--disable-smooth-scrolling'],
    });
    console.log(`Chromium ${browser.version()}`);
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

async function waitForCanvas(page, viewport, scrolls) {
    await page.waitForFunction(({ viewport, scrolls }) => {
        const canvas = document.querySelector('canvas[data-ready]');
        if (!canvas) return false;
        const { width, height } = canvas.getBoundingClientRect();
        const gl = canvas.getContext('webgl2');
        return width === viewport.width &&
            (scrolls ? height > viewport.height : height === viewport.height) &&
            gl?.drawingBufferWidth === Math.floor(width * devicePixelRatio) &&
            gl.drawingBufferHeight === Math.floor(height * devicePixelRatio);
    }, { viewport, scrolls });
}

test('Overview loads static assets and resizes and scrolls in portrait', async () => {
    const page = await browser.newPage({ viewport: desktop, deviceScaleFactor: 2 });
    const errors = [];
    const requests = new Set();
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => {
        if (message.type() === 'error') errors.push(message.text());
    });
    page.on('request', request => requests.add(request.url()));

    try {
        assert.equal((await page.goto(url)).status(), 200);
        await waitForCanvas(page, desktop, false);
        const label = await page.getByRole('img').getAttribute('aria-label');
        assert.match(label, /No WaterWall instance connected\. Sample data\./);
        assert.match(label, /CPU utilization 28 percent\. Memory 3\.2 of 8\.0 GiB\. Uptime 4 days, 7 hours, 23 minutes\./);
        assert.match(label, /Download traffic .* Latest 7\.4 MiB per second\./);

        await page.setViewportSize(portrait);
        await waitForCanvas(page, portrait, true);
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth), portrait.width);

        await page.mouse.move(190, 700);
        await page.mouse.wheel(0, 400);
        await page.waitForFunction(() => window.scrollY > 0);
        await page.keyboard.press('Home');
        await page.waitForFunction(() => window.scrollY === 0);
        await page.keyboard.press('PageDown');
        await page.waitForFunction(() => window.scrollY > 0);

        await page.setViewportSize(desktop);
        await waitForCanvas(page, desktop, false);
        assert.equal(await page.evaluate(() => window.scrollY), 0);
        assert.deepEqual(errors, []);
        assert.deepEqual(requests, new Set([url, `${url}index.js`, `${url}index.wasm`]));
    } finally {
        await page.close();
    }
});

test('Overview shows loading feedback and reports a missing Wasm asset', async () => {
    const page = await browser.newPage();
    let releaseWasm;
    const loading = new Promise(resolve => { releaseWasm = resolve; });
    await page.route('**/index.wasm', async route => {
        await loading;
        await route.abort();
    });

    try {
        await page.goto(url, { waitUntil: 'domcontentloaded' });
        const status = page.getByRole('status');
        assert.match(await status.innerText(), /Loading Wave/);
        releaseWasm();
        await page.getByText(/Wave could not start/).waitFor({ state: 'visible' });
        assert.equal(await page.locator('canvas').isVisible(), false);
    } finally {
        releaseWasm();
        await page.close();
    }
});
