const assert = require('node:assert/strict');
const { after, before, test } = require('node:test');
const { chromium } = require('playwright-core');

const url = 'http://127.0.0.1:8000/';
const desktop = { width: 1280, height: 800 };
const portrait = { width: 390, height: 844 };
let browser;

before(async () => {
    browser = await chromium.launch({
        args: ['--enable-unsafe-swiftshader', '--disable-smooth-scrolling'],
    });
    console.log(`Chromium ${browser.version()}`);
});

after(async () => {
    await browser?.close();
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
