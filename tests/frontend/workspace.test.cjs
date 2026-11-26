const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
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
    browser = await chromium.launch({
        args: ['--enable-unsafe-swiftshader', '--disable-smooth-scrolling'],
        ignoreDefaultArgs: ['--hide-scrollbars'],
    });
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
        server: overviewSnapshot,
        settings: {
            name: 'Wave Preview', admin_username: 'demo', domain: 'wave.example.invalid', port: 2030,
            websocket_path: '/ws/preview', site_template: 'Wave Standard', certificate_path: 'preview/server.crt',
            private_key_path: 'preview/server.key', multiplex: true,
        },
        topology: {
            nodes: [
                { id: 'inside', label: 'Office', type: 'Client', address: '192.0.2.10', x: 0.5, y: 0.5, map: 'inside' },
                { id: 'outside', label: 'Relay', type: 'Router', address: '192.0.2.20', x: 0.5, y: 0.5, map: 'outside' },
            ],
            links: [], routes: [],
        },
        bot: {
            connected: false, name: 'Wave Preview Bot', admin_link: 'wave-preview://bot/admin',
            notifications: true, allow_status: true, allow_config: false,
            notification_rules: { high_cpu: { enabled: true, sound: false } },
            permissions: { status: true },
            overrides: { domain: 'relay.example.invalid', sni: 'tls.example.invalid', ws_host: '', port: 443 },
        },
        console: {},
    };
}

async function mockPreview(page, { data = fixture(), gate = Promise.resolve(), handle } = {}) {
    const connected = Promise.withResolvers();
    const events = new EventEmitter();
    const actions = [];
    await page.routeWebSocket(previewUrl, async socket => {
        const send = fields => socket.send(JSON.stringify({ type: 'snapshot', data, ...fields }));
        socket.onMessage(raw => {
            const request = JSON.parse(raw);
            const reply = ({ ok = true, feedback = '' } = {}) =>
                send({ action: request.action, request_id: request.request_id, ok, feedback });
            actions.push({ request, reply });
            events.emit('action');
            if (!handle) reply();
            else handle(data, request, reply);
        });
        await gate;
        send();
        connected.resolve({ send, socket });
    });
    return {
        broadcast: async () => (await connected.promise).send(),
        close: async () => (await connected.promise).socket.close({ code: 1000 }),
        actions,
        async action(index) {
            const signal = AbortSignal.timeout(5000);
            while (index >= actions.length) await once(events, 'action', { signal });
            return actions[index];
        },
    };
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

async function settleCanvas(page) {
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

async function screenshotPixels(page, clip) {
    const screenshot = await page.screenshot({ clip, scale: 'device' });
    return page.evaluate(async encoded => {
        const bytes = Uint8Array.from(atob(encoded), character => character.charCodeAt(0));
        const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
        const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const { data } = context.getImageData(0, 0, canvas.width, canvas.height);
        return { width: canvas.width, height: canvas.height, pixels: Array.from(data) };
    }, screenshot.toString('base64'));
}

async function canvasClick(page, x, y) {
    await page.mouse.click(x, y, { delay: 40 });
    await settleCanvas(page);
}

async function replaceText(page, x, y, value) {
    await canvasClick(page, x, y);
    await page.locator('#wave-text-input').waitFor({ state: 'visible' });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
    if (value) await page.keyboard.insertText(value);
    else await page.keyboard.press('Backspace');
    await settleCanvas(page);
}

function assertRequest(request, expected) {
    const { request_id: id, ...fields } = request;
    assert.ok(Number.isSafeInteger(id) && id > 0, `Expected a positive request ID, got ${id}`);
    assert.deepEqual(fields, expected);
    return id;
}

async function openSetup(page, click = (x, y) => canvasClick(page, x, y)) {
    await click(132, 23);
    await page.waitForFunction(() => document.getElementById('menu').getAttribute('aria-expanded') === 'true');
    await click(170, 160);
    await page.getByRole('img', { name: 'Welcome to Wave. Step 1 of 3.' }).waitFor();
}

// Entry feedback changes the height of the centered dialog. Locate its thin outline before clicking.
async function entryFrame(page) {
    await settleCanvas(page);
    const { width, height } = page.viewportSize();
    const start = 114;
    const { pixels } = await screenshotPixels(page, { x: Math.floor(width / 2), y: start, width: 1,
        height: height - start });
    const edge = [];
    for (let i = 0; i < pixels.length; i += 4) {
        const [r, g, b] = pixels.slice(i, i + 3);
        if (r === 138 && g === 132 && b === 149) edge.push(start + i / 4);
    }
    assert.ok(edge.length >= 2, 'Expected an entry dialog outline');
    return { top: edge[0], bottom: edge.at(-1) };
}

async function clickEntryFooter(page, x) {
    const { bottom } = await entryFrame(page);
    await canvasClick(page, x, bottom - 25);
}

async function openPanelDetails(page) {
    await openSetup(page);
    await clickEntryFooter(page, 860);
    await page.getByRole('img', { name: 'Preview notes. Step 2 of 3.' }).waitFor();
    await clickEntryFooter(page, 915);
    await page.getByRole('img', { name: 'Panel details. Step 3 of 3.' }).waitFor();
}

async function useExampleCredentials(page) {
    const { top } = await entryFrame(page);
    await canvasClick(page, 460, top + 149);
}

for (const scale of [1, 2]) {
    test(`draws a sharp header divider at ${scale}× device scale`, async () => {
        const page = await browser.newPage({ viewport: desktop, deviceScaleFactor: scale });
        await mockPreview(page);
        try {
            await page.goto(url);
            await waitForCanvas(page, desktop, false);
            await settleCanvas(page);

            // A strip between the header controls and the status text spans both header edges.
            const clip = { x: 900, y: 0, width: 2, height: 50 };
            const { width, height, pixels } = await screenshotPixels(page, clip);
            assert.equal(width, clip.width * scale);
            assert.equal(height, clip.height * scale);
            const expectedRows = [
                [0, [243, 129, 74, 255]], [1, [243, 129, 74, 255]],
                [2, [8, 7, 9, 255]], [46, [8, 7, 9, 255]],
                [47, [61, 58, 67, 255]],
                [48, [8, 12, 16, 255]], [49, [8, 12, 16, 255]],
            ];
            for (const [y, color] of expectedRows) {
                const expected = Array.from({ length: width }, () => color).flat();
                for (let offset = 0; offset < scale; offset++) {
                    const start = (y * scale + offset) * width * 4;
                    assert.deepEqual(pixels.slice(start, start + width * 4), expected,
                        `Expected a solid color at logical row ${y}, device row offset ${offset}`);
                }
            }
        } finally {
            await page.close();
        }
    });
}

for (const { scale, highContrast, normal, hovered } of [
    { scale: 1, highContrast: false, normal: [138, 132, 149], hovered: [181, 180, 197] },
    { scale: 2, highContrast: true, normal: [181, 176, 196], hovered: [211, 210, 229] },
]) {
    test(`draws an inset, rounded Logs scrollbar at ${scale}×${highContrast ? ' in High contrast' : ''}`, async () => {
        const page = await browser.newPage({ viewport: desktop, deviceScaleFactor: scale });
        const data = fixture();
        data.logs = Array.from({ length: 30 }, (_, index) => ({
            ts: '09:41:02', level: 'info', message: `Preview log entry ${index + 1}.`,
        }));
        await mockPreview(page, { data });
        const track = { x: 1243, y: 654, width: 17, height: 126 };
        const trackColor = [13, 11, 14];
        const active = [243, 129, 74];

        async function thumb(color) {
            await settleCanvas(page);
            const { pixels, width, height } = await screenshotPixels(page, track);
            const at = (x, y, sample) => pixelMatches(pixels, (y * width + x) * 4, sample);
            const rows = Array.from({ length: height }, (_, y) =>
                Array.from({ length: width }, (_, x) => x).filter(x => at(x, y, color)));
            const top = rows.findIndex(row => row.length > 0);
            const bottom = rows.findLastIndex(row => row.length > 0);
            assert.ok(top >= 0 && bottom > top, `Expected a scrollbar thumb with color ${color}.`);

            const center = Math.floor((top + bottom) / 2);
            const middle = rows[center];
            const [left, right] = [middle[0], middle.at(-1)];
            assert.ok(middle.length >= 7 * scale && middle.length <= 9 * scale,
                'The visible grip fits inside the 13px track.');
            assert.ok(at(left - scale, center, trackColor) && at(right + scale, center, trackColor),
                'The dark track stays visible beside the thumb.');
            return { top, bottom, center, left, right, middleWidth: middle.length,
                firstWidth: rows[top].length, lastWidth: rows[bottom].length };
        }

        try {
            await page.goto(url);
            await page.getByRole('img', { name: /Wave Users.*30 log entries/ }).waitFor();
            if (highContrast) {
                await canvasClick(page, 132, 23);
                await canvasClick(page, 165, 87);
            }

            await page.mouse.move(900, 708);
            await page.mouse.wheel(0, 2400);
            const atBottom = await thumb(normal);
            assert.ok(atBottom.firstWidth < atBottom.middleWidth && atBottom.lastWidth < atBottom.middleWidth,
                'Both ends taper into rounded caps.');

            await page.mouse.wheel(0, -2400);
            const atTop = await thumb(normal);
            assert.ok(atTop.top < atBottom.top - 50 * scale, 'The wheel moves the thumb up the track.');
            const x = track.x + (atTop.left + atTop.right) / (2 * scale);
            const y = track.y + atTop.center / scale;
            await page.mouse.move(x, y);
            assert.equal((await thumb(hovered)).top, atTop.top);

            await page.mouse.down();
            await page.mouse.move(x, y + 30, { steps: 4 });
            assert.ok((await thumb(active)).top > atTop.top + 20 * scale, 'The dragged thumb moves down the track.');
            await page.mouse.up();
        } finally {
            await page.close();
        }
    });
}

test('updates the painted page scrollbar when High contrast changes', async () => {
    const viewport = { ...desktop, height: 560 };
    const page = await browser.newPage({ viewport });
    await mockPreview(page);
    const geometry = () => page.evaluate(() => ({
        width: document.documentElement.clientWidth,
        bodyWidth: document.body.clientWidth,
        canvasWidth: document.getElementById('canvas').clientWidth,
        offset: scrollY,
    }));

    try {
        await page.goto(url);
        await waitForCanvas(page, viewport, true);
        await page.getByRole('img', { name: /Wave Users.*Sample data/ }).waitFor();
        await page.evaluate(() => scrollTo(0, 10));
        await settleCanvas(page);
        const before = await geometry();
        assert.equal(before.offset, 10);
        assert.equal(before.canvasWidth, before.width);
        assert.ok(before.width < viewport.width, 'The page has a classic scrollbar.');
        const clip = { x: Math.floor((before.width + viewport.width) / 2), y: 40, width: 1, height: 480 };
        const thumb = async color => colorRuns((await screenshotPixels(page, clip)).pixels, [color], 30);
        const standard = await thumb([138, 132, 149]);
        assert.equal(standard.length, 1);

        await canvasClick(page, 132, 23 - before.offset);
        await canvasClick(page, 165, 87 - before.offset);
        // Capture the browser-painted thumb with the pointer still over the header.
        assert.deepEqual(await thumb([181, 176, 196]), standard);
        assert.deepEqual(await geometry(), before);
    } finally {
        await page.close();
    }
});

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
        const canvas = page.locator('#canvas');
        assert.equal(await canvas.evaluate(element => element.getBoundingClientRect().right), desktop.width);
        await page.getByRole('img', { name: /Wave Users.*Loading sample data/ }).waitFor();
        releaseSnapshot();
        await page.getByRole('img', { name: /Wave Users.*WaterWall disconnected.*2 users.*Preview fixture is ready/ }).waitFor();
        const pages = ['Server Status', 'Users', 'Panel Settings', 'Console',
            'Nodes & Routers', 'Telegram Bot', 'About', 'Overview'];
        const currentLink = page.locator('#navigation [aria-current="page"]');
        const assertPage = async name => {
            await page.getByRole('img', { name: new RegExp(`^Wave ${name}\\.`) }).waitFor();
            assert.equal(await currentLink.count(), 1);
            assert.equal(await currentLink.textContent(), name);
            assert.equal(await page.title(), `${name} · Wave`);
        };
        await assertPage('Users');
        assert.deepEqual(await page.getByRole('navigation', { name: 'Wave' }).getByRole('link').allTextContents(), pages);

        const headerEdge = { x: 900, y: 0, width: 2, height: 3 };
        const brandTile = { x: 10, y: 8, width: 32, height: 32 };
        const unfocusedEdge = await page.screenshot({ clip: headerEdge });
        const unfocusedBrand = await page.screenshot({ clip: brandTile });
        await canvasClick(page, 400, 30);
        assert.equal(await page.evaluate(() => document.activeElement.id), 'canvas');
        assert.ok((await page.screenshot({ clip: headerEdge })).equals(unfocusedEdge),
            'Pointer focus leaves the canvas edge unchanged.');
        assert.ok((await page.screenshot({ clip: brandTile })).equals(unfocusedBrand),
            'Pointer focus leaves the brand tile unchanged.');
        const selectionRow = await screenshotPixels(page, { x: 11, y: 110, width: 166, height: 1 });
        const selectionPixels = selectionRow.pixels.slice(0, selectionRow.width * 4);
        assert.deepEqual(colorRuns(selectionPixels, [[243, 129, 74]]), [[322, 326]],
            'The selected Users row has a two-logical-pixel accent on its right edge.');

        const menu = page.getByRole('button', { name: 'Menu' });
        const tools = page.getByRole('button', { name: 'Tools' });
        const sidebar = { x: 8, y: 54, width: 174, height: 744 };
        const unfocusedSidebar = await page.screenshot({ clip: sidebar });
        const activeControl = () => page.evaluate(() => document.activeElement.id);
        await menu.focus();
        await page.keyboard.press('Tab');
        assert.equal(await activeControl(), 'tools');
        await page.keyboard.press('Tab');
        assert.equal(await activeControl(), 'canvas', 'Tab enters the workspace after Tools.');
        await settleCanvas(page);
        assert.ok((await page.screenshot({ clip: headerEdge })).equals(unfocusedEdge));
        assert.ok((await page.screenshot({ clip: brandTile })).equals(unfocusedBrand),
            'The logo keeps its appearance when the canvas receives focus.');

        for (let step = 0; step < 3; step++) {
            await page.keyboard.press('Tab');
            await settleCanvas(page);
        }
        assert.ok((await page.screenshot({ clip: sidebar })).equals(unfocusedSidebar),
            'Tab passes the sidebar inside the canvas.');
        await page.keyboard.press('Enter');
        await settleCanvas(page);
        for (let step = 0; step < 3; step++) {
            await page.keyboard.press('Tab');
            await settleCanvas(page);
        }
        await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout: 5000 });
        await page.keyboard.press('Escape');
        await settleCanvas(page);

        await canvasClick(page, 65, 110);
        await page.mouse.move(400, 30);
        await page.keyboard.press('Tab');
        await settleCanvas(page);
        assert.ok((await page.screenshot({ clip: sidebar })).equals(unfocusedSidebar),
            'Tab passes the sidebar after a click on the selected page.');

        for (const [button, other] of [[menu, tools], [tools, menu]]) {
            await button.focus();
            await page.keyboard.press('Enter');
            await page.locator(`#${await button.getAttribute('id')}[aria-expanded="true"]`).waitFor();
            assert.equal(await other.getAttribute('aria-expanded'), 'false');
            await button.focus();
            await page.keyboard.press('Enter');
            await page.locator(`#${await button.getAttribute('id')}[aria-expanded="false"]`).waitFor();
        }

        for (const [index, name] of pages.entries()) {
            await canvasClick(page, 65, 76 + 34 * index);
            await assertPage(name);
        }
        await page.getByRole('img', { name: /CPU utilization 63 percent.*Memory 4\.5 of 16\.0 GiB.*Latest 9\.6 MiB/ }).waitFor();

        await page.setViewportSize(portrait);
        await waitForCanvas(page, portrait, true);
        await canvasClick(page, 225, 110);
        await assertPage('Users');
        await canvasClick(page, 225, 212);
        await assertPage('Overview');
        await canvasClick(page, 225, 110);
        await assertPage('Users');
        assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
        await page.mouse.move(190, 700);
        await page.mouse.wheel(0, 1500);
        await page.waitForFunction(() => Math.ceil(scrollY + innerHeight) >= document.body.scrollHeight);

        const breakpoint = { width: 760, height: desktop.height };
        await page.setViewportSize(breakpoint);
        await waitForCanvas(page, breakpoint, false);
        assert.equal(await canvas.evaluate(element => element.getBoundingClientRect().right), breakpoint.width);
        assert.equal(await page.evaluate(() => scrollY), 0);
        await canvasClick(page, 65, 76);
        await assertPage('Server Status');
        assert.deepEqual(errors, []);
        assert.deepEqual(requests, new Set([url, `${url}index.js`, `${url}index.wasm`]));
    } finally {
        releaseSnapshot();
        await page.close();
    }
});

const statusTitleColor = [43, 39, 48];
const statusBodyColor = [31, 27, 34];
const statusOutlineColor = [61, 58, 67];
const uploadPlotColor = [81, 165, 246];
const downloadPlotColor = [243, 129, 74];

function statusFixture() {
    const data = fixture();
    data.server = { ...data.server, name: 'Wave preview with an intentionally long server name that wraps '
        + 'onto multiple rows inside the Server Information card', region: 'Frankfurt, DE',
        platform: 'Linux x86_64', version: '0.1.0-preview', cpu_cores: 8, cpu_percent: 25,
        memory_used_gib: 4, memory_total_gib: 16, users_total: 2, users_enabled: 2, users_online: 1,
        upload_rate: 2, download_rate: 6, upload_total: 12.5, download_total: 55.8,
        cpu_history: [15, 35, 25], memory_history: [1, 6, 4], upload_history: [1, 2, 2],
        download_history: [8, 4, 6] };
    return data;
}

function pixelMatches(pixels, offset, color) {
    return color.every((channel, index) => pixels[offset + index] === channel);
}

function colorRuns(pixels, colors, minimum = 1) {
    const runs = [];
    let start = null;
    for (let position = 0; position <= pixels.length / 4; position++) {
        const match = colors.some(color => pixelMatches(pixels, position * 4, color));
        if (match && start === null) start = position;
        if (!match && start !== null) {
            if (position - start >= minimum) runs.push([start, position]);
            start = null;
        }
    }
    return runs;
}

async function statusCards(page, x, top, bottom) {
    const { pixels } = await screenshotPixels(page, { x, y: top, width: 1, height: bottom - top });
    const titles = colorRuns(pixels, [statusTitleColor], 14);
    const surfaces = colorRuns(pixels, [statusTitleColor, statusBodyColor, statusOutlineColor], 50);
    return titles.flatMap(([from, to]) => {
        const surface = surfaces.find(([start, end]) => start <= from && end > to);
        return surface ? [{ top: from + top, titleEnd: to + top, bottom: surface[1] + top }] : [];
    });
}

async function desktopStatusGrid(page, viewport) {
    const left = viewport.width >= 1600 ? 234 : 188;
    const bottom = viewport.height - 10 - Math.floor(Math.max(195, Math.min(244, viewport.height * 0.25)));
    const column = await statusCards(page, left + 18, 58, bottom);
    assert.equal(column.length, 2, 'The left column has two visible cards.');

    const bands = [];
    for (const card of column) {
        const clip = { x: left, y: card.top + 2, width: viewport.width - left - 10, height: 1 };
        const { pixels } = await screenshotPixels(page, clip);
        const runs = colorRuns(pixels, [statusTitleColor], 120).map(run => run.map(x => x + left));
        assert.equal(runs.length, 2, 'Each title band fills a separate column.');
        assert.ok(Math.abs((runs[0][1] - runs[0][0]) - (runs[1][1] - runs[1][0])) <= 1);
        assert.ok(runs[1][0] > runs[0][1], 'A gutter separates the two columns.');
        bands.push(runs);
    }
    assert.deepEqual(bands[1], bands[0], 'The second row uses the same column widths and gutter.');
    const rightColumn = await statusCards(page, bands[0][1][1] - 8, 58, bottom);
    assert.equal(rightColumn.length, 2, 'The right column has two visible cards.');
    for (const [row, right] of rightColumn.entries()) {
        // The strips sample rounded corners at slightly different offsets.
        assert.ok(Math.abs(right.top - column[row].top) <= 2);
        assert.equal(right.titleEnd, column[row].titleEnd);
        assert.ok(Math.abs(right.bottom - column[row].bottom) <= 2);
    }
    return { cards: column, columns: bands[0] };
}

async function plotColors(page, clip) {
    const { pixels, width } = await screenshotPixels(page, clip);
    return [uploadPlotColor, downloadPlotColor].map(color => {
        let right = -1;
        let y = -1;
        for (let offset = 0; offset < pixels.length; offset += 4) {
            const x = (offset / 4) % width;
            if (x > right && pixelMatches(pixels, offset, color)) {
                right = x;
                y = Math.floor(offset / 4 / width);
            }
        }
        return y;
    });
}

test('Server Status aligns both grid rows, updates a live plot, and reflows both rows together', async () => {
    const wide = { width: 1920, height: 960 };
    const page = await browser.newPage({ viewport: wide });
    const data = statusFixture();
    const preview = await mockPreview(page, { data });
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Server Status');
        await page.getByRole('img', {
            name: /CPU 25\.0%. Memory 4\.0 \/ 16\.0 GiB. Upload 2\.0 MiB\/s. Download 6\.0 MiB\/s\./,
        }).waitFor();
        await settleCanvas(page);
        await desktopStatusGrid(page, wide);

        await page.setViewportSize(desktop);
        await waitForCanvas(page, desktop, false);
        await settleCanvas(page);
        const initial = await desktopStatusGrid(page, desktop);
        const [cpu, info] = initial.cards;
        const [cpuColumn, trafficColumn] = initial.columns;
        const cpuClip = { x: cpuColumn[1] - 120, y: cpu.titleEnd + 35, width: 115,
            height: cpu.bottom - cpu.titleEnd - 38 };
        const trafficClip = { x: trafficColumn[0] + 60, y: info.titleEnd + 40, width: 20,
            height: info.bottom - info.titleEnd - 46 };
        const [, cpuBefore] = await plotColors(page, cpuClip);
        assert.ok(cpuBefore >= 0, 'The CPU line is visible near the current sample.');
        assert.ok((await plotColors(page, trafficClip)).every(y => y >= 0),
            'Upload and download have distinct lines inside the same plot.');

        data.server.cpu_percent = 75;
        data.server.cpu_history.push(75);
        data.logs.push({ ts: '09:41:07', level: 'info', message: 'Received the next metrics sample.' });
        await preview.broadcast();
        await page.getByRole('img', { name: /Received the next metrics sample/ }).waitFor();
        assert.match(await page.getByRole('img').getAttribute('aria-label'), /CPU 75\.0%\./);
        await settleCanvas(page);
        const [, cpuAfter] = await plotColors(page, cpuClip);
        assert.ok(cpuAfter >= 0 && cpuAfter < cpuBefore - 20, 'The latest CPU sample rose in the fixed-scale plot.');
        assert.deepEqual(await desktopStatusGrid(page, desktop), initial, 'A new sample leaves the grid in place.');

        const twoColumns = { ...desktop, width: 920 };
        await page.setViewportSize(twoColumns);
        await waitForCanvas(page, twoColumns, false);
        await settleCanvas(page);
        await desktopStatusGrid(page, twoColumns);

        const transition = { ...desktop, width: 892 };
        const canvasSize = () => page.locator('canvas').evaluate(canvas => [canvas.clientWidth, canvas.clientHeight]);
        await page.setViewportSize(transition);
        await settleCanvas(page);
        const fromWide = await canvasSize();

        const oneColumn = { ...desktop, width: 860 };
        await page.setViewportSize(oneColumn);
        await waitForCanvas(page, oneColumn, true);
        await settleCanvas(page);
        const upper = await statusCards(page, 206, 58, oneColumn.height);
        await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
        await settleCanvas(page);
        const scroll = await page.evaluate(() => scrollY);
        const lower = (await statusCards(page, 206, 0, oneColumn.height - 250))
            .filter(card => card.top > 0).map(card => card.top + scroll);
        assert.equal(new Set([...upper.map(card => card.top), ...lower]).size, 4,
            'At the narrower pane width all four cards occupy their own row.');

        await page.setViewportSize(transition);
        await settleCanvas(page);
        assert.deepEqual(await canvasSize(), fromWide, 'The same viewport uses the same layout after widening.');
    } finally {
        await page.close();
    }
});

test('Server Status stacks the four cards in reading order and swipes through to Logs on a phone', async () => {
    const page = await browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
    const preview = await mockPreview(page, { data: statusFixture() });
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await page.touchscreen.tap(70, 105);
        await page.getByRole('img', { name: /^Wave Server Status/ }).waitFor();
        await settleCanvas(page);
        const scrollbar = await page.evaluate(() => ({
            canvasWidth: document.querySelector('canvas').clientWidth,
            thumbBorder: getComputedStyle(document.documentElement, '::-webkit-scrollbar-thumb').borderTopWidth,
        }));
        assert.deepEqual(scrollbar, { canvasWidth: portrait.width, thumbBorder: '0px' },
            'Overlay scrollbars keep their native styling and the full canvas width.');

        const touch = await page.context().newCDPSession(page);
        const titles = new Set();
        for (let swipe = 0; swipe < 5; swipe++) {
            const { offset, height } = await page.evaluate(() => ({ offset: scrollY, height: document.body.scrollHeight }));
            const top = Math.max(0, 244 - offset);
            const bottom = Math.min(portrait.height, height - offset - 250);
            const cards = await statusCards(page, 28, top, bottom);
            for (const card of cards) {
                if (card.top > top) titles.add(card.top + offset);
            }
            if (Math.ceil(offset + portrait.height) >= height) break;
            await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: 175, y: 706 }] });
            for (const y of [570, 420, 267, 173]) {
                await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: 175, y }] });
            }
            await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
            await settleCanvas(page);
        }
        assert.ok(await page.evaluate(() => Math.ceil(scrollY + innerHeight) >= document.body.scrollHeight));
        await page.touchscreen.tap(115, portrait.height - 195);
        assert.deepEqual((await preview.action(0)).request, { action: 'clear_logs' });

        const positions = [...titles].sort((left, right) => left - right);
        assert.equal(positions.length, 4);
        const series = [];
        for (const [index, top] of positions.entries()) {
            await page.evaluate(y => window.scrollTo(0, y - 36), top);
            await settleCanvas(page);
            const relativeTop = top - await page.evaluate(() => scrollY);
            const next = positions[index + 1] ?? top + 200;
            const y = relativeTop + 67;
            const height = Math.min(next - top - 75, portrait.height - y);
            const colors = await plotColors(page, { x: 192, y, width: 24, height });
            series.push(colors.map(position => position >= 0));
        }
        assert.deepEqual(series, [[false, true], [true, false], [false, false], [true, true]],
            'CPU, Memory, Server Information, and Traffic appear in order above Logs.');
    } finally {
        await page.close();
    }
});

test('Server Status keeps the Traffic plot and Logs in place when phone legends update', async () => {
    const viewport = { ...portrait, width: 350 };
    const page = await browser.newPage({ viewport, hasTouch: true, isMobile: true });
    const data = statusFixture();
    const setRates = (upload, download) => {
        data.server.upload_rate = upload;
        data.server.download_rate = download;
        data.server.upload_history.push(upload);
        data.server.download_history.push(download);
    };
    setRates(31.1, 61.1);
    const preview = await mockPreview(page, { data });
    async function trafficLayout() {
        // The canvas can grow after switching views. Wait for the bottom of the current page.
        await page.waitForFunction(() => {
            const end = document.body.scrollHeight;
            if (Math.ceil(scrollY + innerHeight) >= end) return true;
            window.scrollTo(0, end);
            return false;
        });
        await settleCanvas(page);
        const { height, offset } = await page.evaluate(() => ({
            height: document.querySelector('canvas').clientHeight, offset: scrollY,
        }));
        assert.ok(Math.ceil(offset + viewport.height) >= height, 'The Traffic plot is sampled at the page end.');
        const { pixels } = await screenshotPixels(page, { x: 32, y: 0, width: 1, height: viewport.height - 250 });
        const plot = colorRuns(pixels, [[13, 11, 14]], 80).at(-1);
        assert.ok(plot, 'The Traffic plot is visible above Logs.');
        return { height, plotTop: plot[0] + offset };
    }

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Server Status');
        await waitForCanvas(page, viewport, true);
        const initial = await trafficLayout();
        setRates(38.8, 68.8);
        await preview.broadcast();
        await page.getByRole('img', { name: /Upload 38\.8 MiB\/s\. Download 68\.8 MiB\/s/ }).waitFor();
        await settleCanvas(page);
        assert.deepEqual(await trafficLayout(), initial);
    } finally {
        await page.close();
    }
});

test('pastes into user search and toggles the matching switch without shifting the table header', async () => {
    const page = await browser.newPage({ viewport: desktop });
    await page.context().grantPermissions(['clipboard-read', 'clipboard-write'], { origin: url });
    const actions = [];
    await mockPreview(page, { handle(data, action, reply) {
        actions.push(action);
        const changed = data.users.find(row => row.id === action.id);
        assert.ok(changed && action.action === 'set_user_enabled');
        changed.enabled = action.enabled;
        const message = `${action.enabled ? 'Enabled' : 'Disabled'} ${changed.name} in the preview.`;
        data.logs.push({ ts: '09:41:13', level: 'success', message });
        reply({ feedback: message });
    } });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await page.mouse.click(610, 104);
        await page.locator('#wave-text-input').waitFor({ state: 'visible' });
        await page.keyboard.type('unmatched');
        await page.keyboard.press('Control+a');
        await page.evaluate(() => navigator.clipboard.writeText('Bella'));
        await page.keyboard.press('Control+v');
        await settleCanvas(page);
        await page.mouse.move(401, 51);
        await settleCanvas(page);
        const header = { x: 199, y: 123, width: 1040, height: 28 };
        const before = await page.screenshot({ clip: header });
        await canvasClick(page, 500, 171);
        await page.getByRole('img', { name: /2 log entries. Latest log: Disabled Bella Chen/ }).waitFor();
        await page.mouse.move(401, 51);
        await settleCanvas(page);
        assert.deepEqual(await page.screenshot({ clip: header }), before);
        assert.equal(actions.length, 1);
        assertRequest(actions[0], { action: 'set_user_enabled', id: 2, enabled: false });

        await page.keyboard.press('ArrowRight');
        await settleCanvas(page);
        await page.keyboard.press('ArrowLeft');
        await settleCanvas(page);
        await page.keyboard.press('Space');
        await page.getByRole('img', { name: /3 log entries. Latest log: Enabled Bella Chen/ }).waitFor();
        assert.equal(actions.length, 2);
        assertRequest(actions[1], { action: 'set_user_enabled', id: 2, enabled: true });
    } finally {
        await page.close();
    }
});

test('resizes a Users column across rows and keeps its width through sorting and snapshots', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    data.users[0].name = 'Zed Brooks';
    data.users[1].name = 'Avery Chen';
    const preview = await mockPreview(page, { data });
    async function dividers(y) {
        const clip = { x: 222, y, width: 399, height: 1 };
        const { pixels } = await screenshotPixels(page, clip);
        const boundaries = [];
        for (let i = 0; i < pixels.length; i += 4) {
            if (pixels[i] === 61 && pixels[i + 1] === 58 && pixels[i + 2] === 67) {
                boundaries.push(clip.x + i / 4);
            }
        }
        return boundaries;
    }

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        const [idDivider, nameDivider, stateDivider, actionsDivider] = await dividers(126);
        assert.ok(idDivider < nameDivider && nameDivider < stateDivider && stateDivider < actionsDivider);
        const actionsCell = { x: stateDivider + 3, y: 154, width: actionsDivider - stateDivider - 3, height: 36 };
        const beforeResize = await page.screenshot({ clip: actionsCell });

        await page.mouse.move(stateDivider, 137);
        await page.mouse.down();
        await settleCanvas(page);
        await page.mouse.move(nameDivider + 6, 137, { steps: 4 });
        await settleCanvas(page);
        for (let frame = 0; frame < 8; frame++) {
            await page.evaluate(() => new Promise(resolve => requestAnimationFrame(resolve)));
            assert.ok((await page.screenshot({ clip: actionsCell })).equals(beforeResize),
                'The State column keeps its minimum width while the divider is held.');
        }
        await page.mouse.up();
        await settleCanvas(page);

        await page.mouse.move(nameDivider, 137);
        await page.mouse.down();
        await settleCanvas(page);
        await page.mouse.move(nameDivider + 48, 137, { steps: 4 });
        await settleCanvas(page);
        await page.mouse.up();
        await page.mouse.move(401, 51);
        await settleCanvas(page);
        const [resizedId, resizedName] = await dividers(126);
        assert.equal(resizedId, idDivider);
        assert.ok(resizedName > nameDivider + 35, 'Dragging expands the Name column.');
        assert.deepEqual((await dividers(154)).slice(0, 2), [resizedId, resizedName]);

        await canvasClick(page, idDivider + 30, 137);
        await canvasClick(page, resizedName + 33, 171);
        assertRequest((await preview.action(0)).request,
            { action: 'set_user_enabled', id: 2, enabled: false });
        await replaceText(page, 610, 104, 'Avery');
        data.logs.push({ ts: '09:41:03', level: 'info', message: 'Received the latest rates.' });
        await preview.broadcast();
        await page.getByRole('img', { name: /Received the latest rates/ }).waitFor();
        assert.deepEqual((await dividers(126)).slice(0, 2), [resizedId, resizedName]);
    } finally {
        await page.close();
    }
});

test('keeps wide Console controls inside the command bar', async () => {
    const page = await browser.newPage({ viewport: { width: 1920, height: 978 } });
    const data = fixture();
    data.console = { running: true, output: [], history: ['status'] };
    await mockPreview(page, { data });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Console');
        await settleCanvas(page);

        const { pixels } = await screenshotPixels(page, { x: 1600, y: 440, width: 304, height: 1 });
        let edges = 0;
        for (let i = 0; i < pixels.length; i += 4) {
            if (pixels[i] === 138 && pixels[i + 1] === 132 && pixels[i + 2] === 149) {
                edges++;
            }
        }
        // The scan crosses the input's right edge and both edges of each button.
        assert.equal(edges, 7, 'Run, Previous, and Next fit inside the Console pane.');
    } finally {
        await page.close();
    }
});

test('keeps an open Users edit draft while replacing rows and their configuration', async () => {
    const page = await browser.newPage({ viewport: { width: 1320, height: 840 } });
    await page.context().grantPermissions(['clipboard-read', 'clipboard-write'], { origin: url });
    const data = fixture();
    const original = { ...user, id: 2, name: 'Albright', notes: 'Original notes', remaining_minutes: -1 };
    delete original.options;
    data.users = [
        { ...user, id: 1, name: 'Alpine' },
        original,
        { ...user, id: 3, name: 'Aardvark' },
        { ...user, id: 4, name: '' },
    ];
    const configuration = 'wave-preview://user/replaced';
    const preview = await mockPreview(page, { data, handle(_data, _request, reply) {
        reply({ feedback: 'Applied the Users draft.' });
    } });
    const description = name => page.getByRole('img', { name });

    try {
        await page.goto(url);
        await description(/Wave Users.*4 users/).waitFor();
        await canvasClick(page, 270, 137);
        await replaceText(page, 610, 104, 'Al');
        await canvasClick(page, 590, 171);
        await canvasClick(page, 610, 188);
        await replaceText(page, 495, 220, 'Albright local');

        data.users = [
            { ...user, id: 5, name: 'Alpine new' },
            { ...user, id: 2, name: 'Albright remote', notes: 'Other notes', remaining_minutes: 120, configuration },
        ];
        await preview.broadcast();
        await description(/Wave Users.*2 users/).waitFor();
        await canvasClick(page, 820, 667);
        await description(/Applied the Users draft/).waitFor();
        assertRequest((await preview.action(0)).request, {
            action: 'edit_user', id: 2, name: 'Albright local',
        });

        await canvasClick(page, 590, 171);
        await canvasClick(page, 625, 235);
        await canvasClick(page, 710, 572);
        await page.waitForFunction(async text => (await navigator.clipboard.readText()) === text, configuration);
    } finally {
        await page.close();
    }
});

test('merges a Users edit with changes from a second tab', async () => {
    const context = await browser.newContext({ viewport: { width: 1320, height: 840 } });
    const editor = await context.newPage();
    const other = await context.newPage();
    const data = fixture();
    const row = data.users[0];
    row.options = { upload_limit: 3, download_limit: 5 };
    function handle(_data, request, reply) {
        assert.equal(request.id, row.id);
        if (request.action === 'set_user_enabled') {
            row.enabled = request.enabled;
        } else {
            assert.equal(request.action, 'edit_user');
            const { name, enabled, options } = request;
            if (name !== undefined) row.name = name;
            if (enabled !== undefined) row.enabled = enabled;
            Object.assign(row.options, options);
        }
        data.logs.push({ ts: '09:41:03', level: 'info',
            message: `${row.name}, ${row.enabled ? 'enabled' : 'disabled'}, ${row.options.upload_limit}/${row.options.download_limit}.` });
        reply();
    }
    const firstPreview = await mockPreview(editor, { data, handle });
    const secondPreview = await mockPreview(other, { data, handle });
    async function openFirstEdit(page) {
        await canvasClick(page, 590, 171);
        await canvasClick(page, 610, 188);
        await page.locator('#wave-text-input').waitFor({ state: 'visible' });
    }

    try {
        await Promise.all([editor.goto(url), other.goto(url)]);
        for (const page of [editor, other]) {
            await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        }
        await openFirstEdit(editor);
        await replaceText(editor, 495, 220, 'Avery Local');
        await replaceText(editor, 480, 375, '25');

        await canvasClick(other, 514, 171);
        assertRequest((await secondPreview.action(0)).request,
            { action: 'set_user_enabled', id: 1, enabled: false });
        await firstPreview.broadcast();
        await editor.getByRole('img', { name: /Avery Brooks, disabled, 3\/5/ }).waitFor();

        await openFirstEdit(other);
        await replaceText(other, 480, 408, '18');
        await canvasClick(other, 820, 667);
        assertRequest((await secondPreview.action(1)).request, {
            action: 'edit_user', id: 1, options: { download_limit: 18 },
        });
        await firstPreview.broadcast();
        await editor.getByRole('img', { name: /Avery Brooks, disabled, 3\/18/ }).waitFor();

        await canvasClick(editor, 820, 667);
        assertRequest((await firstPreview.action(0)).request, {
            action: 'edit_user', id: 1, name: 'Avery Local', options: { upload_limit: 25 },
        });
        await secondPreview.broadcast();
        for (const page of [editor, other]) {
            await page.getByRole('img', { name: /Avery Local, disabled, 25\/18/ }).waitFor();
        }
    } finally {
        await context.close();
    }
});

test('uses the compact Users table when a sidebar leaves too little room for names', async () => {
    const page = await browser.newPage({ viewport: { width: 985, height: 800 } });
    const preview = await mockPreview(page);
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await replaceText(page, 300, 136, 'Avery');
        await canvasClick(page, 900, 266);
        assertRequest((await preview.action(0)).request,
            { action: 'set_user_enabled', id: 1, enabled: false });

        const wider = { width: 1010, height: 800 };
        await page.setViewportSize(wider);
        await waitForCanvas(page, wider, false);
        await replaceText(page, 300, 136, '');
        await canvasClick(page, 380, 168);
        await canvasClick(page, 925, 266);
        assertRequest((await preview.action(1)).request,
            { action: 'set_user_enabled', id: 2, enabled: false });
    } finally {
        await page.close();
    }
});

test('paginates the compact Users table and targets the replacement row', async () => {
    const page = await browser.newPage({ viewport: { width: 320, height: 680 } });
    const data = fixture();
    data.users = Array.from({ length: 7 }, (_, index) => ({ ...user, id: index + 1, name: index ? `User ${index + 1}` : '' }));
    const preview = await mockPreview(page, { data });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*7 users/ }).waitFor();
        data.users.splice(5, 2,
            { ...user, id: 8, name: 'Replacement', enabled: false },
            { ...user, id: 9, name: 'Newest' },
        );
        data.users_live = false;
        data.logs.push({ ts: '09:41:03', level: 'info', message: 'Replaced a compact row.' });
        await preview.broadcast();
        await page.getByRole('img', { name: /Replaced a compact row/ }).waitFor();
        await canvasClick(page, 144, 289);
        assert.deepEqual((await preview.action(0)).request, { action: 'set_users_live', live: true });
        await canvasClick(page, 121, 423);
        await canvasClick(page, 234, 496);
        assertRequest((await preview.action(1)).request, { action: 'set_user_enabled', id: 8, enabled: true });
    } finally {
        await page.close();
    }
});

test('narrow touch activates a switch and header controls and scrolls on a swipe', async () => {
    const page = await browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
    const preview = await mockPreview(page);
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await waitForCanvas(page, portrait, true);

        await page.touchscreen.tap(70, 105);
        await page.getByRole('img', { name: /Wave Server Status/ }).waitFor();
        await page.touchscreen.tap(230, 105);
        await page.getByRole('img', { name: /Wave Users/ }).waitFor();

        await page.touchscreen.tap(318, 463);
        assertRequest((await preview.action(0)).request,
            { action: 'set_user_enabled', id: 1, enabled: false });
        for (const [id, x] of [['menu', 130], ['tools', 182]]) {
            await page.touchscreen.tap(x, 24);
            await page.locator(`#${id}[aria-expanded="true"]`).waitFor();
            await page.touchscreen.tap(x, 24);
            await page.locator(`#${id}[aria-expanded="false"]`).waitFor();
        }

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

test('first-run setup validates input, preserves the draft, and completes through the preview socket', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const preview = await mockPreview(page, { handle(_data, _action, reply) {
        reply({ feedback: 'Applied demo setup.' });
    } });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openPanelDetails(page);
        const { top } = await entryFrame(page);
        await replaceText(page, 700, top + 49, '');
        await clickEntryFooter(page, 830);
        await page.getByRole('img', { name: /Enter a panel name/ }).waitFor();
        await replaceText(page, 700, (await entryFrame(page)).top + 49, 'Demo Control Panel');
        await clickEntryFooter(page, 830);
        await page.getByRole('img', { name: /Enter a demo username and password/ }).waitFor();
        assert.equal(preview.actions.length, 0);
        await useExampleCredentials(page);
        await clickEntryFooter(page, 700);
        await page.getByRole('img', { name: 'Preview notes. Step 2 of 3.' }).waitFor();
        await clickEntryFooter(page, 915);
        await page.getByRole('img', { name: 'Panel details. Step 3 of 3.' }).waitFor();
        await clickEntryFooter(page, 830);
        await page.getByRole('img', { name: 'Setup complete. Applied demo setup.' }).waitFor();
        assert.equal(preview.actions.length, 1);
        assertRequest(preview.actions[0].request, {
            action: 'setup', name: 'Demo Control Panel', username: 'demo', password: 'wave-demo',
        });
        await clickEntryFooter(page, 734);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
    } finally {
        await page.close();
    }
});

for (const acknowledged of [false, true]) {
    const order = acknowledged ? 'after' : 'before';
    test(`setup reports the result when the socket closes ${order} acknowledgement`, async () => {
        const page = await browser.newPage({ viewport: desktop });
        await page.clock.install({ time: 0 });
        const preview = await mockPreview(page, { handle() {} });
        try {
            await page.goto(url);
            await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
            await openPanelDetails(page);
            await useExampleCredentials(page);
            await clickEntryFooter(page, 830);
            const { reply } = await preview.action(0);

            // Hold rendering so both socket events can precede the form's next frame.
            await page.clock.pauseAt((await page.evaluate(() => Date.now())) + 500);
            if (acknowledged) reply({ feedback: 'Applied demo setup.' });
            await preview.close();
            await page.clock.resume();
            await settleCanvas(page);

            const expected = acknowledged ? 'Setup complete. Applied demo setup.'
                : 'Panel details. Step 3 of 3. The preview connection closed. Try again after reconnecting.';
            const description = await page.getByRole('img').getAttribute('aria-label');
            assert.equal(description, expected);
        } finally {
            await page.close();
        }
    });
}

test('a reopened setup waits for its own reply and retries an active rejection', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const preview = await mockPreview(page, { handle() {} });
    await page.addInitScript(() => {
        const NativeWebSocket = window.WebSocket;
        window.WebSocket = class extends NativeWebSocket {
            constructor(...args) {
                super(...args);
                this.addEventListener('message', ({ data }) => {
                    window.lastPreviewRequestId = JSON.parse(data).request_id;
                });
            }
        };
    });
    const pending = 'Panel details. Step 3 of 3. Applying demo setup…';
    const description = name => page.getByRole('img', { name });
    async function submit(index) {
        await clickEntryFooter(page, 830);
        await description(pending).waitFor();
        return preview.action(index);
    }

    try {
        await page.goto(url);
        await description(/Wave Users.*2 users/).waitFor();
        await openPanelDetails(page);
        await useExampleCredentials(page);
        const first = await submit(0);
        await clickEntryFooter(page, 830);
        assert.equal(preview.actions.length, 1);
        await clickEntryFooter(page, 540);
        await description(/Wave Users.*2 users/).waitFor();

        await openPanelDetails(page);
        await clickEntryFooter(page, 830);
        await description(/Enter a demo username and password/).waitFor();
        await useExampleCredentials(page);
        const second = await submit(1);
        first.reply({ feedback: 'The first setup completed.' });
        await page.waitForFunction(id => window.lastPreviewRequestId === id, first.request.request_id);
        await settleCanvas(page);
        assert.equal(await description(pending).count(), 1);
        second.reply({ ok: false, feedback: 'The active setup was rejected.' });
        await description(/The active setup was rejected/).waitFor();
        const retry = await submit(2);
        retry.reply({ feedback: 'The setup retry completed.' });
        await description('Setup complete. The setup retry completed.').waitFor();

        const ids = [first, second, retry].map(({ request }) => assertRequest(request, {
            action: 'setup', name: 'Wave Preview', username: 'demo', password: 'wave-demo',
        }));
        assert.equal(new Set(ids).size, ids.length);
    } finally {
        await page.close();
    }
});

test('a timed-out setup keeps the draft available for retry', async () => {
    const page = await browser.newPage({ viewport: desktop });
    await page.clock.install({ time: 0 });
    const preview = await mockPreview(page, { handle() {} });
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openPanelDetails(page);
        await useExampleCredentials(page);
        await clickEntryFooter(page, 830);
        const first = await preview.action(0);
        await page.clock.fastForward(5100);
        await page.getByRole('img', { name: 'Panel details. Step 3 of 3. The preview server did not reply. Try again.' }).waitFor();

        await clickEntryFooter(page, 830);
        const retry = await preview.action(1);
        assert.notEqual(first.request.request_id, retry.request.request_id);
        assertRequest(retry.request, { action: 'setup', name: 'Wave Preview', username: 'demo', password: 'wave-demo' });
        retry.reply({ feedback: 'Applied demo setup.' });
        await page.getByRole('img', { name: 'Setup complete. Applied demo setup.' }).waitFor();
    } finally {
        await page.close();
    }
});

test('setup fits a narrow phone and supports touch and keyboard form navigation', async () => {
    const phone = { width: 320, height: 568 };
    const page = await browser.newPage({ viewport: phone, isMobile: true, hasTouch: true });
    const preview = await mockPreview(page, { handle() {} });
    async function tap(x, y) {
        await page.touchscreen.tap(x, y);
        await settleCanvas(page);
    }
    async function next() {
        const { bottom } = await entryFrame(page);
        await tap(248, bottom - 29);
    }
    async function expectFeedbackColor(red, green, blue) {
        const { top, bottom } = await entryFrame(page);
        const start = top + 26;
        const { pixels } = await screenshotPixels(page,
            { x: 24, y: start, width: 272, height: bottom - start - 91 });
        assert.ok(pixels.some((value, i) => i % 4 === 0 && value === red &&
            pixels[i + 1] === green && pixels[i + 2] === blue), 'Expected feedback inside the visible dialog body');
    }

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openSetup(page, tap);
        const welcome = await entryFrame(page);
        assert.ok(Math.abs((welcome.top + welcome.bottom) / 2 - (phone.height + 107) / 2) <= 2);
        await next();
        await page.getByRole('img', { name: 'Preview notes. Step 2 of 3.' }).waitFor();
        await next();
        await page.getByRole('img', { name: 'Panel details. Step 3 of 3.' }).waitFor();

        const details = await entryFrame(page);
        assert.ok(details.bottom < phone.height - 12, 'Wrapped footer stays inside the viewport');
        await tap(90, details.top + 72);
        const input = page.locator('#wave-text-input');
        await input.waitFor({ state: 'visible' });
        for (let i = 0; i < 2; i++) {
            const before = await input.evaluate(element => parseFloat(element.style.top));
            await page.keyboard.press('Tab');
            await page.waitForFunction(previous => {
                const field = document.getElementById('wave-text-input');
                return field === document.activeElement && parseFloat(field.style.top) > previous + 40;
            }, before);
        }
        await page.keyboard.insertText('phone-demo-pass');
        await settleCanvas(page);
        await page.setViewportSize({ width: phone.width, height: 360 });
        await next();
        const { request, reply } = await preview.action(0);
        const expected = { action: 'setup', name: 'Wave Preview', username: 'demo', password: 'phone-demo-pass' };
        assertRequest(request, expected);
        await page.getByRole('img', { name: /Applying demo setup/ }).waitFor();
        await expectFeedbackColor(81, 165, 246);
        reply({ ok: false, feedback: 'The preview declined the request.' });
        await page.getByRole('img', { name: /The preview declined the request/ }).waitFor();
        await expectFeedbackColor(241, 127, 111);
        await next();
        const retry = await preview.action(1);
        assertRequest(retry.request, expected);
        retry.reply({ feedback: 'Applied demo setup.' });
        await page.getByRole('img', { name: 'Setup complete. Applied demo setup.' }).waitFor();
    } finally {
        await page.close();
    }
});

test('setup fields and actions are reachable at 320 × 256', async () => {
    const page = await browser.newPage({ viewport: { width: 320, height: 256 }, isMobile: true, hasTouch: true });
    const preview = await mockPreview(page, { handle() {} });
    const description = name => page.getByRole('img', { name });
    const touch = await page.context().newCDPSession(page);
    async function tap(x, y) {
        await page.touchscreen.tap(x, y);
        await settleCanvas(page);
    }
    async function tapFooter(x) {
        const { top, bottom } = await entryFrame(page);
        for (let swipe = 0; swipe < 4; swipe++) {
            await touch.send('Input.dispatchTouchEvent', {
                type: 'touchStart', touchPoints: [{ x: 160, y: bottom - 20 }],
            });
            for (const y of [bottom - 40, top + 50, top + 39]) {
                await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: 160, y }] });
                await settleCanvas(page);
            }
            await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        }
        await tap(x, bottom - 29);
    }

    try {
        await page.goto(url);
        await description(/Wave Users.*2 users/).waitFor();
        await openSetup(page, tap);
        for (const title of ['Preview notes. Step 2 of 3.', 'Panel details. Step 3 of 3.']) {
            await tapFooter(248);
            await description(title).waitFor();
        }

        const { top } = await entryFrame(page);
        await tap(90, top + 72);
        const input = page.locator('#wave-text-input');
        await input.waitFor({ state: 'visible', timeout: 5000 });
        const expected = { action: 'setup', name: 'Compact Wave', username: 'compact-admin', password: 'compact-demo' };
        for (const [index, value] of [expected.name, expected.username, expected.password].entries()) {
            if (index > 0) {
                await page.keyboard.press('Tab');
                await settleCanvas(page);
            }
            await page.keyboard.press('Control+a');
            await settleCanvas(page);
            await page.keyboard.insertText(value);
            await settleCanvas(page);
        }

        await tapFooter(145);
        await description('Preview notes. Step 2 of 3.').waitFor();
        await tapFooter(248);
        await description('Panel details. Step 3 of 3.').waitFor();
        await tapFooter(248);
        const { request, reply } = await preview.action(0);
        assertRequest(request, expected);
        reply({ feedback: 'Applied demo setup.' });
        await description('Setup complete. Applied demo setup.').waitFor();
    } finally {
        await page.close();
    }
});

test('applies a populated Settings draft after a broadcast and a page change', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    const original = { ...data.settings };
    const preview = await mockPreview(page, { data, handle(current, request, reply) {
        Object.assign(current.settings, request.settings);
        reply({ feedback: 'Applied sample panel settings.' });
    } });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Panel Settings');
        await replaceText(page, 325, 141, 'Harbor QA');
        data.settings.name = 'Another client';
        data.logs.push({ ts: '09:41:03', level: 'info', message: 'Received another client’s settings.' });
        await preview.broadcast();
        await page.getByRole('img', { name: /Received another client’s settings/ }).waitFor();
        await openView(page, 'Server Status');
        await openView(page, 'Panel Settings');
        await canvasClick(page, 245, 414);
        await canvasClick(page, 754, 436);
        await page.getByRole('img', { name: /Applied sample panel settings/ }).waitFor();
        assert.equal(preview.actions.length, 1);
        assertRequest(preview.actions[0].request, { action: 'apply_settings', settings: { ...original, name: 'Harbor QA' } });
    } finally {
        await page.close();
    }
});

test('a later Settings save preserves a remote change received while the page was inactive', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    const original = { ...data.settings };
    const preview = await mockPreview(page, { data, handle() {} });
    const apply = async () => {
        await canvasClick(page, 245, 414);
        await canvasClick(page, 754, 436);
    };

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Panel Settings');
        await replaceText(page, 325, 141, 'Harbor QA');
        await apply();
        const first = await preview.action(0);
        assertRequest(first.request, { action: 'apply_settings', settings: { ...original, name: 'Harbor QA' } });

        await canvasClick(page, 640, 445);
        await openView(page, 'Server Status');
        data.settings = first.request.settings;
        first.reply({ feedback: 'Applied the new panel name.' });
        data.settings = original;
        data.logs.push({ ts: '09:41:03', level: 'info', message: 'Another client restored the panel name.' });
        await preview.broadcast();
        await page.getByRole('img', { name: /Another client restored the panel name/ }).waitFor();

        await openView(page, 'Panel Settings');
        await replaceText(page, 325, 194, 'harbor-admin');
        await apply();
        const second = await preview.action(1);
        assertRequest(second.request, { action: 'apply_settings', settings: { ...original, admin_username: 'harbor-admin' } });
    } finally {
        await page.close();
    }
});

for (const order of [['Settings', 'Overrides'], ['Overrides', 'Settings']]) {
    test(`both bot edits survive stale snapshots when ${order[0]} saves first`, async () => {
        const data = fixture();
        const initial = structuredClone(data.bot);
        const tabs = new Map();
        let rejected = false;

        try {
            for (const name of order) {
                const page = await browser.newPage({ viewport: desktop });
                tabs.set(name, { page });
                const preview = await mockPreview(page, { data, handle(current, request, reply) {
                    if (name === 'Overrides' && order[0] === name && !rejected) {
                        rejected = true;
                        reply({ ok: false, feedback: 'Rejected the sample overrides.' });
                        return;
                    }
                    Object.assign(current.bot, request.settings);
                    reply({ feedback: `Saved sample bot ${name}.` });
                } });
                tabs.get(name).preview = preview;
                await page.goto(url);
                await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
                await openView(page, 'Telegram Bot');
            }

            // Both pages received the same initial snapshot. The fixture publishes no
            // intermediate broadcast, so the other page saves from that snapshot too.
            for (const name of order) {
                const { page, preview } = tabs.get(name);
                await page.bringToFront();
                if (name === 'Settings') {
                    await canvasClick(page, 320, 80);
                    await canvasClick(page, 212, 113);
                    await canvasClick(page, 220, 211);
                    await canvasClick(page, 250, 278);
                } else {
                    await canvasClick(page, 404, 80);
                    await replaceText(page, 230, 326, '8443');
                    await canvasClick(page, 250, 360);
                    if (order[0] === name) {
                        await page.getByRole('img', { name: /Rejected the sample overrides/ }).waitFor();
                        await canvasClick(page, 250, 360);
                        const first = (await preview.action(0)).request;
                        const retry = (await preview.action(1)).request;
                        assert.deepEqual(retry.settings, first.settings);
                        assert.notEqual(retry.request_id, first.request_id);
                    }
                }

                await page.getByRole('img', { name: new RegExp(`Saved sample bot ${name}`) }).waitFor();
                const { request } = preview.actions.at(-1);
                if (name === 'Settings') {
                    assert.deepEqual(Object.keys(request.settings).sort(), [
                        'allow_config', 'allow_status', 'notification_rules', 'notifications', 'permissions',
                    ]);
                    assert.equal(request.settings.notifications, false);
                    assert.equal(request.settings.allow_status, initial.allow_status);
                    assert.equal(request.settings.allow_config, initial.allow_config);
                    assert.deepEqual(request.settings.notification_rules.high_cpu, initial.notification_rules.high_cpu);
                    assert.equal(request.settings.permissions.status, initial.permissions.status);
                    assertRequest(request, { action: 'apply_bot_settings', settings: request.settings });
                } else {
                    assertRequest(request, {
                        action: 'apply_bot_settings', settings: { overrides: { ...initial.overrides, port: 8443 } },
                    });
                }
            }

            assert.equal(data.bot.notifications, false);
            assert.equal(data.bot.overrides.port, 8443);
        } finally {
            await Promise.all([...tabs.values()].map(({ page }) => page.close()));
        }
    });
}

test('preserves selection and zoom for each topology map when switching pages', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    const unnamedNode = { id: '', label: '', map: 'inside', x: 0.25, y: 0.5 };
    data.topology.nodes.push(unnamedNode);
    const preview = await mockPreview(page, { data });
    async function capture() {
        await page.mouse.move(185, 50);
        await settleCanvas(page);
        return page.screenshot({ clip: { x: 198, y: 100, width: 1040, height: 420 } });
    }

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Nodes & Routers');
        const insideInitial = await capture();
        delete unnamedNode.id;
        delete unnamedNode.label;
        data.logs.push({ ts: '09:41:03', level: 'info', message: 'Received an unnamed node.' });
        await preview.broadcast();
        await page.getByRole('img', { name: /Received an unnamed node/ }).waitFor();
        assert.ok((await capture()).equals(insideInitial), 'A node with missing text renders with an empty label.');
        await canvasClick(page, 720, 301);
        const insideSelected = await capture();
        await canvasClick(page, 240, 114);
        const insideZoomed = await capture();
        assert.ok(!insideSelected.equals(insideInitial), 'Inside map selection changed the drawing.');
        assert.ok(!insideZoomed.equals(insideSelected), 'Inside map zoom changed the drawing.');

        await canvasClick(page, 340, 80);
        const outsideInitial = await capture();
        await canvasClick(page, 720, 301);
        const outsideSelected = await capture();
        await canvasClick(page, 210, 114);
        const outsideZoomed = await capture();
        assert.ok(!outsideSelected.equals(outsideInitial), 'Outside map selection changed the drawing.');
        assert.ok(!outsideZoomed.equals(outsideSelected), 'Outside map zoom changed the drawing.');

        await openView(page, 'Server Status');
        await openView(page, 'Nodes & Routers');
        await canvasClick(page, 250, 80);
        assert.ok((await capture()).equals(insideZoomed), 'Inside map kept its selection and zoom.');
        await canvasClick(page, 340, 80);
        assert.ok((await capture()).equals(outsideZoomed), 'Outside map kept its selection and zoom.');
    } finally {
        await page.close();
    }
});

test('touch drags stay on their topology map while swipes outside it scroll the page', async () => {
    const page = await browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
    await mockPreview(page);
    const touch = await page.context().newCDPSession(page);
    async function swipe(points) {
        await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [points[0]] });
        for (const point of points.slice(1)) {
            await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [point] });
        }
        await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        await settleCanvas(page);
    }
    async function nodePosition() {
        const clip = { x: 75, y: 385, width: 245, height: 115 };
        const { width, pixels } = await screenshotPixels(page, clip);
        let x = width;
        let y = clip.height;
        for (let i = 0; i < pixels.length; i += 4) {
            if (pixels[i] === 43 && pixels[i + 1] === 39 && pixels[i + 2] === 48) {
                x = Math.min(x, (i / 4) % width);
                y = Math.min(y, Math.floor(i / 4 / width));
            }
        }
        assert.ok(x < width, 'The raised node fill is visible.');
        return { x: x + clip.x, y: y + clip.y };
    }

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Nodes & Routers');
        await waitForCanvas(page, portrait, true);
        const start = await nodePosition();
        await swipe([{ x: 49, y: 365 }, { x: 65, y: 353 }, { x: 95, y: 337 }]);
        const moved = await nodePosition();
        assert.deepEqual(moved, { x: start.x + 46, y: start.y - 28 });
        assert.equal(await page.evaluate(() => scrollY), 0);

        const edge = { x: moved.x + 30, y: moved.y - 2, width: 8, height: 5 };
        const unselected = await page.screenshot({ clip: edge });
        await page.touchscreen.tap(moved.x + 35, moved.y + 15);
        await settleCanvas(page);
        assert.notDeepEqual(await page.screenshot({ clip: edge }), unselected, 'A tap selects the node.');
        const selected = await nodePosition();

        await page.touchscreen.tap(158, 266);
        await settleCanvas(page);
        assert.deepEqual(await nodePosition(), start, 'The outside map has its own pan state.');
        await swipe([{ x: 97, y: 400 }, { x: 125, y: 416 }]);
        const outside = await nodePosition();
        assert.deepEqual(outside, { x: start.x + 28, y: start.y + 16 });
        assert.equal(await page.evaluate(() => scrollY), 0);

        await page.mouse.move(85, 395);
        await page.mouse.down();
        await settleCanvas(page);
        await page.mouse.move(60, 385);
        await settleCanvas(page);
        await page.mouse.up();
        assert.deepEqual(await nodePosition(), { x: outside.x - 25, y: outside.y - 10 });

        await page.touchscreen.tap(66, 266);
        await settleCanvas(page);
        assert.deepEqual(await nodePosition(), selected, 'The inside map kept its pan state and selection.');
        await swipe([{ x: 150, y: 690 }, { x: 150, y: 640 }, { x: 150, y: 560 }]);
        await page.waitForFunction(() => scrollY > 0);
    } finally {
        await page.close();
    }
});

test('Overview shows loading and unavailable data, and About opens without preview data', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const gate = Promise.withResolvers();
    await page.addInitScript(() => { window.open = url => { window.openedUrl = url; }; });
    await page.routeWebSocket(previewUrl, async socket => {
        await gate.promise;
        socket.send('{"type":"snapshot","data":{}}');
    });
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*Loading sample data/ }).waitFor();
        await openView(page, 'Overview');
        await page.getByRole('img', { name: /Wave Overview.*Loading sample data/ }).waitFor();
        gate.resolve();
        await page.getByRole('img', { name: /Wave Overview.*Sample data unavailable/ }).waitFor();
        await openView(page, 'Users');
        await page.getByRole('img', { name: /Wave Users.*Sample data unavailable/ }).waitFor();
        assert.equal(await page.getByRole('link', { name: 'Users' }).getAttribute('aria-current'), 'page');
        await openView(page, 'About');
        await canvasClick(page, 329, 225);
        assert.equal(await page.evaluate(() => window.openedUrl), '/licenses/wave.txt');
        await canvasClick(page, 300, 273);
        assert.equal(await page.evaluate(() => window.openedUrl), '/licenses/poppins.txt');
    } finally {
        gate.resolve();
        await page.close();
    }
});

test('shows startup feedback and reports a missing Wasm asset', async () => {
    const page = await browser.newPage();
    const startup = page.getByRole('status');
    const assertStartupTheme = async () => {
        const colors = await startup.evaluate(element => ({
            background: getComputedStyle(document.documentElement).backgroundColor,
            text: getComputedStyle(element).color,
            themeColor: document.querySelector('meta[name="theme-color"]').content.toLowerCase(),
        }));
        assert.deepEqual(colors, { background: 'rgb(8, 12, 16)', text: 'rgb(234, 235, 255)', themeColor: '#080c10' });
        assert.equal(await page.locator('canvas').isVisible(), false);
    };
    let releaseWasm;
    const loading = new Promise(resolve => { releaseWasm = resolve; });
    await page.route('**/index.wasm', async route => {
        await loading;
        await route.abort();
    });
    try {
        await page.goto(url, { waitUntil: 'domcontentloaded' });
        assert.match(await startup.innerText(), /Loading Wave/);
        await assertStartupTheme();
        releaseWasm();
        await page.getByText(/Wave could not start/).waitFor({ state: 'visible' });
        await assertStartupTheme();
    } finally {
        releaseWasm();
        await page.close();
    }
});

test('cancels the demo sign-out by keyboard and opens sign-in on confirmation', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const preview = await mockPreview(page);
    const openConfirmation = async () => {
        await canvasClick(page, 134, 23);
        await canvasClick(page, 172, 185);
        await page.waitForFunction(() => document.getElementById('panel-controls').inert);
    };
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*Sample data\./ }).waitFor();
        await openConfirmation();
        await page.keyboard.press('Tab');
        await settleCanvas(page);
        await page.keyboard.press('Space');
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(preview.actions.length, 0);

        await openConfirmation();
        await canvasClick(page, 757, 426);
        assert.deepEqual((await preview.action(0)).request, { action: 'sign_out' });
        await page.getByRole('img', { name: 'Demo sign-in.' }).waitFor();
    } finally {
        await page.close();
    }
});

test('centers a dialog on a short scrolled phone and keeps its footer reachable during a touch swipe', async () => {
    const viewport = { width: 390, height: 342 };
    const page = await browser.newPage({ viewport, hasTouch: true });
    const preview = await mockPreview(page);
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*Sample data\./ }).waitFor();
        await waitForCanvas(page, viewport, true);
        await page.evaluate(() => window.scrollTo(0, 135));
        await settleCanvas(page);
        await page.touchscreen.tap(63, 155);
        await page.waitForFunction(() => document.getElementById('panel-controls').inert);
        await settleCanvas(page);

        // The border occupies the safe margin from top to bottom, including both square corners.
        const outline = await screenshotPixels(page, { x: 12, y: 0, width: 1, height: viewport.height });
        const edges = [];
        for (let y = 0; y < outline.height; ++y) {
            const pixel = outline.pixels.slice(4 * y, 4 * y + 3);
            if (pixel.every((channel, index) => channel === [138, 132, 149][index])) edges.push(y);
        }
        assert.deepEqual([edges[0], edges.at(-1)], [12, 329]);
        assert.equal(edges.length, 318);

        const footer = { x: 149, y: 274, width: 203, height: 54 };
        const body = { x: 28, y: 115, width: 220, height: 135 };
        const initialFooter = await screenshotPixels(page, footer);
        const initialBody = await screenshotPixels(page, body);
        const touch = await page.context().newCDPSession(page);
        await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: 145, y: 245 }] });
        for (const y of [210, 170, 126, 79, 54]) {
            await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: 145, y }] });
            await settleCanvas(page);
        }
        await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        await settleCanvas(page);
        assert.equal(await page.evaluate(() => scrollY), 135);
        assert.notDeepEqual(await screenshotPixels(page, body), initialBody);
        assert.deepEqual(await screenshotPixels(page, footer), initialFooter);

        await page.touchscreen.tap(205, 302);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(preview.actions.length, 0);
    } finally {
        await page.close();
    }
});
