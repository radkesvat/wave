const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
const { readFile } = require('node:fs/promises');
const { createServer } = require('node:http');
const { join } = require('node:path');
const { after, before, test } = require('node:test');
const { chromium } = require('playwright-core');

const desktop = { width: 1280, height: 800 };
const coreDesktop = { width: 1280, height: 1080 };
const portrait = { width: 390, height: 844 };
const donationSlotHeight = 63;
const timeout = 10000;

let url;
let previewUrl;
let sessionUrl;
let consoleUrl;
let server;
let browser;

before(async () => {
    const siteDir = process.env.WAVE_SITE_DIR;
    assert.ok(siteDir, 'Run this test through a Wave frontend CTest preset.');
    const assets = new Map();
    for (const [path, filename, contentType] of [
        ['/panel', 'index.html', 'text/html; charset=utf-8'],
        ['/index.js', 'index.js', 'text/javascript; charset=utf-8'],
        ['/index.wasm', 'index.wasm', 'application/wasm'],
        ['/console.js', 'console.js', 'text/javascript; charset=utf-8'],
        ['/terminal/xterm.js', 'terminal/xterm.js', 'text/javascript; charset=utf-8'],
        ['/terminal/addon-fit.js', 'terminal/addon-fit.js', 'text/javascript; charset=utf-8'],
        ['/terminal/xterm.css', 'terminal/xterm.css', 'text/css; charset=utf-8'],
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
    const host = `127.0.0.1:${server.address().port}`;
    url = `http://${host}/panel`;
    sessionUrl = `ws://${host}/ws/panel`;
    previewUrl = `ws://${host}/ws/preview`;
    consoleUrl = `ws://${host}/ws/console`;
    browser = await chromium.launch({ args: ['--enable-unsafe-swiftshader'] });
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

function fixture(coreState = 'running') {
    return {
        logs: [{ source: 'panel', ts: '09:41:02', level: 'info', message: 'Preview fixture is ready.' }],
        server: {
            name: 'wave-fixture', platform: 'Linux x86_64', version: '1.6.0',
            cpu_cores: 8, cpu_percent: 25, memory_used_gib: 4, memory_total_gib: 16,
            uptime_minutes: 1599, upload_rate: 2, download_rate: 6, upload_total: 12.5, download_total: 55.8,
            cpu_history: [15, 35, 25], memory_history: [1, 6, 4], upload_history: [1, 2, 2],
            download_history: [8, 4, 6], available_panel_versions: ['1.6.0'],
            core: {
                state: coreState,
                capabilities: { lifecycle: true, configs: true },
                configs: [{ id: 1, name: 'Default', enabled: true, active: true }],
            },
        },
        settings: {
            domain: 'wave.example.invalid',
            certificate_path: 'preview/server.crt', private_key_path: 'preview/server.key',
        },
        core_settings: { misc: { workers: 2 } },
    };
}

async function mockPreview(page, data, gate = Promise.resolve()) {
    await page.routeWebSocket(sessionUrl, socket => {
        socket.send(JSON.stringify({
            type: 'session', username: 'fixture-admin', expires_at: Math.floor(Date.now() / 1000) + 86400,
        }));
    });
    const connected = Promise.withResolvers();
    const events = new EventEmitter();
    const actions = [];
    let previewSocket;
    let sendSnapshot;
    await page.routeWebSocket(previewUrl, async socket => {
        const send = fields => socket.send(JSON.stringify({ type: 'snapshot', data, ...fields }));
        socket.onMessage(raw => {
            const request = JSON.parse(raw);
            const reply = ({ ok = true, feedback = '', result, feedback_in_logs_only,
                level = ok ? 'success' : 'error' } = {}) => {
                if (feedback_in_logs_only && feedback) {
                    data.logs.push({ source: 'panel', ts: '09:41:03', level, message: feedback });
                }
                send({ action: request.action, request_id: request.request_id, ok, feedback, result, feedback_in_logs_only });
            };
            actions.push({ request, reply });
            events.emit('action');
        });
        await gate;
        previewSocket = socket;
        sendSnapshot = send;
        send();
        connected.resolve();
    });
    return {
        async broadcast() {
            await connected.promise;
            sendSnapshot();
        },
        async disconnect() {
            await connected.promise;
            await previewSocket.close({ code: 1011, reason: 'Fixture connection closed' });
        },
        actions,
        async action(index) {
            const signal = AbortSignal.timeout(timeout);
            while (index >= actions.length) await once(events, 'action', { signal });
            return actions[index];
        },
    };
}

function status(page, name) {
    return page.getByRole('img', { name }).waitFor({ timeout });
}

async function expectLoggedOnce(page, message) {
    await status(page, `Latest log: ${message}`);
    const description = await page.locator('#canvas').getAttribute('aria-label');
    assert.equal(description.split(message).length - 1, 1, 'The action result appears only in Logs.');
}

async function openView(page, name) {
    await page.getByRole('link', { name, exact: true }).focus();
    await page.keyboard.press('Enter');
    await status(page, new RegExp(`^Wave ${name}\\.`));
}

function settleCanvas(page) {
    return page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

async function click(page, x, y) {
    await page.mouse.click(x, y, { delay: 40 });
    await settleCanvas(page);
}

async function selectPanelTheme(page, name, activate = (x, y) => click(page, x, y)) {
    const menu = page.locator('#menu');
    if (await menu.getAttribute('aria-expanded') !== 'true') {
        await activate(130, 24);
        await page.locator('#menu[aria-expanded="true"]').waitFor({ timeout });
    }
    const row = ['Default', 'Light', 'Classic', 'Dark', 'Darker'].indexOf(name);
    await activate(215, 94);
    await activate(193, 126 + row * 23);
}

function waitForTheme(page, color) {
    return page.waitForFunction(color =>
        document.querySelector('meta[name="theme-color"]').content.toLowerCase() === `#${color}`, color, { timeout });
}

async function acknowledgeThemeSave(preview, data, index, theme) {
    const save = await preview.action(index);
    assertRequest(save.request, { action: 'apply_theme', theme });
    data.settings.theme = theme;
    save.reply();
}

async function replaceText(page, x, y, value) {
    await click(page, x, y);
    await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
    if (value === '') {
        await page.keyboard.press('Backspace');
    } else {
        await page.keyboard.insertText(value);
    }
    await settleCanvas(page);
}

async function readUserField(page, x, y) {
    await page.evaluate(() => navigator.clipboard.writeText(''));
    await click(page, x, y);
    await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
    await page.keyboard.press('Control+c');
    await page.waitForFunction(async () => Boolean(await navigator.clipboard.readText()), undefined, { timeout });
    const value = await page.evaluate(() => navigator.clipboard.readText());
    await page.evaluate(() => navigator.clipboard.writeText(''));
    return value;
}

function assertRequest(request, expected) {
    const { request_id: id, ...fields } = request;
    assert.ok(Number.isSafeInteger(id) && id > 0);
    assert.deepEqual(fields, expected);
    return id;
}

// ImGui draws its controls on one canvas. Read a vertical strip to locate the visible rows.
async function verticalRuns(page, x, top, bottom, [r, g, b], minimum = 1) {
    const screenshot = await page.screenshot({ clip: { x, y: top, width: 1, height: bottom - top } });
    return page.evaluate(async ({ encoded, r, g, b, minimum, top }) => {
        const bytes = Uint8Array.from(atob(encoded), character => character.charCodeAt(0));
        const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
        const canvas = new OffscreenCanvas(1, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const { data } = context.getImageData(0, 0, 1, canvas.height);
        const runs = [];
        let start = null;
        for (let y = 0; y <= canvas.height; y++) {
            const matches = data[y * 4] === r && data[y * 4 + 1] === g && data[y * 4 + 2] === b;
            if (matches && start === null) start = y;
            if (!matches && start !== null) {
                if (y - start >= minimum) runs.push([top + start, top + y]);
                start = null;
            }
        }
        return runs;
    }, { encoded: screenshot.toString('base64'), r, g, b, minimum, top });
}

async function rectangleColorCount(page, clip, [r, g, b]) {
    const screenshot = await page.screenshot({ clip });
    return page.evaluate(async ({ encoded, r, g, b }) => {
        const bytes = Uint8Array.from(atob(encoded), character => character.charCodeAt(0));
        const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
        const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const { data } = context.getImageData(0, 0, bitmap.width, bitmap.height);
        let count = 0;
        for (let i = 0; i < data.length; i += 4) {
            if (data[i] === r && data[i + 1] === g && data[i + 2] === b) count++;
        }
        return count;
    }, { encoded: screenshot.toString('base64'), r, g, b });
}

async function rectangleContainsColor(page, clip, color) {
    return (await rectangleColorCount(page, clip, color)) > 0;
}

async function expectDonationBox(page, { left, right, top, bottom, stickerDiameter }) {
    const centerX = Math.floor((left + right) / 2);
    const centerY = Math.floor((top + bottom) / 2);
    const canvasColor = [8, 12, 16];
    const border = [138, 132, 149];
    const fill = [13, 11, 14];
    assert.deepEqual(await verticalRuns(page, centerX, top - 10, top, [61, 58, 67]), [[top - 9, top - 8]],
        'The navigation pane has a bottom border above the donation box.');
    assert.deepEqual(await verticalRuns(page, centerX, top - 8, top, canvasColor), [[top - 8, top]],
        'Eight pixels of the canvas separate the donation box from the navigation border.');
    assert.deepEqual(await verticalRuns(page, centerX, bottom, bottom + 4, canvasColor), [[bottom, bottom + 4]],
        'Four pixels of the canvas are visible below the donation box.');
    for (const y of [top, bottom - 1]) {
        const pixels = await rectangleColorCount(page, { x: centerX - 24, y, width: 48, height: 1 }, border);
        assert.ok(pixels > 10 && pixels < 38, 'The donation box has a dashed top and bottom outline.');
    }
    const sideHeight = bottom - top - 20;
    for (const x of [left, right - 1]) {
        const pixels = await rectangleColorCount(page, { x, y: top + 10, width: 1, height: sideHeight }, border);
        assert.ok(pixels > sideHeight / 3 && pixels < sideHeight * 0.8,
            'The donation box has a dashed outline aligned with both navigation edges.');
    }
    for (const x of [left, right - 2]) {
        assert.equal(await rectangleColorCount(page, { x, y: top, width: 2, height: 2 }, canvasColor), 4,
            'The donation box outline and fill curve away from its corners.');
    }
    for (const x of [left + 2, right - 4]) {
        assert.ok(await rectangleContainsColor(page, { x, y: centerY - 2, width: 2, height: 4 }, fill),
            'The donation box reaches both sides of the navigation pane.');
    }
    const stickerLeft = left + 12;
    const stickerFace = [255, 195, 106];
    assert.equal(await rectangleContainsColor(page, { x: left, y: centerY - 2, width: 12, height: 4 }, stickerFace),
        false, 'The donation sticker leaves twelve pixels of padding on the left.');
    assert.ok(await rectangleContainsColor(page, { x: stickerLeft, y: centerY - 2, width: 3, height: 4 }, stickerFace),
        'The donation sticker starts just after the left padding.');
    const sticker = await verticalRuns(page, stickerLeft + stickerDiameter / 2, top, bottom, stickerFace);
    assert.ok(sticker.length, 'The sad-face sticker is visible in the donation box.');
    const topInset = sticker[0][0] - top;
    const bottomInset = bottom - sticker.at(-1)[1];
    assert.ok(Math.min(topInset, bottomInset) >= 7 && Math.abs(topInset - bottomInset) <= 2,
        'The donation sticker is vertically centered with at least seven pixels of padding.');
    const visibleDiameter = bottom - top - topInset - bottomInset;
    assert.ok(Math.abs(visibleDiameter - stickerDiameter) <= 2,
        `Expected the donation sticker to span about ${stickerDiameter}px, got ${visibleDiameter}px.`);
}

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

async function dialogFrame(page, scanTop = 114) {
    const { width, height } = page.viewportSize();
    const deadline = Date.now() + timeout;
    do {
        await settleCanvas(page);
        // Chromium can briefly tint the canvas after a touch, changing the border pixels.
        const outline = await verticalRuns(page, Math.floor(width / 2), scanTop, height, [138, 132, 149]);
        if (outline.length >= 2) return { top: outline[0][0], bottom: outline.at(-1)[1] };
    } while (Date.now() < deadline);
    assert.fail('Expected a dialog outline.');
}

async function confirmDialog(page, activate, width = 380, scanTop = 114) {
    const { bottom } = await dialogFrame(page, scanTop);
    const viewport = page.viewportSize().width;
    const right = (viewport + Math.min(width, viewport - 24)) / 2 - 12;
    await activate(right - 42, bottom - 25);
}

async function saveGraphChanges(page) {
    await click(page, 224, 137);
}

async function graphMenuItem(page, menu, row) {
    const x = { nodes: 279, config: 335, view: 385 }[menu];
    await click(page, x, 137);
    await click(page, x, 169 + row * 23);
}

async function graphCanvasTop(page, x = 600, top = 155, bottom = 320) {
    const deadline = Date.now() + timeout;
    do {
        const [border] = await verticalRuns(page, x, top, bottom, [138, 132, 149]);
        if (border) return border[0];
        await settleCanvas(page);
    } while (Date.now() < deadline);
    assert.fail('Expected the top edge of the graph canvas.');
}

async function desktopGraphBounds(page) {
    const [topEdge, bottomEdge] = await verticalRuns(page, 600, 155, 600, [138, 132, 149]);
    assert.ok(topEdge && bottomEdge, 'Expected both edges of the graph canvas.');
    return { left: 199, top: topEdge[0], right: desktop.width - 21, bottom: bottomEdge[1] };
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
}

async function selectConfigJsonText(page) {
    const { top, bottom } = await dialogFrame(page);
    await click(page, 640, Math.floor((top + bottom) / 2));
    await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
}

async function pasteConfigJson(page, value) {
    await page.evaluate(value => navigator.clipboard.writeText(value), value);
    await selectConfigJsonText(page);
    await page.keyboard.press('Control+v');
    await settleCanvas(page);
    await confirmDialog(page, (x, y) => click(page, x, y), 740);
}

async function lifecycleButton(page, action) {
    const [title] = await verticalRuns(page, 510, 250, 650, [43, 39, 48], 14);
    assert.ok(title, 'Expected the Panel Controls title.');
    const x = action === 'stop' ? 675 : 510;
    await click(page, x, title[1] + (action === 'restart' ? 84 : 47));
}

test('opens the authenticated workspace, updates host metrics, and navigates the sidebar', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    const firstSnapshot = Promise.withResolvers();
    const preview = await mockPreview(page, data, firstSnapshot.promise);
    const canvasColor = [8, 12, 16];
    const paneColor = [31, 27, 34];
    const badgeBorder = [138, 132, 149];
    const serverRow = color => verticalRuns(page, 11, 68, 86, color);
    const configRow = color => verticalRuns(page, 11, 110, 128, color);
    try {
        assert.equal((await page.goto(url)).status(), 200);
        await status(page, /Wave Server Status.*Loading panel data/);
        firstSnapshot.resolve();
        await waitForServerStatus(page);

        await openView(page, 'Server Status');
        await status(page, /CPU 25\.0%\. Memory 4\.0 \/ 16\.0 GiB.*Upload 2\.0 MiB\/s\. Download 6\.0 MiB\/s/);
        const dividerScanEnd = desktop.height - 11 - donationSlotHeight;
        const dividers = await verticalRuns(page, 152, 91, dividerScanEnd, [61, 58, 67]);
        assert.deepEqual(dividers.map(([top, bottom]) => bottom - top), [2, 1, 1, 1]);
        for (const x of [11, 176]) {
            assert.deepEqual(await verticalRuns(page, x, 91, dividerScanEnd, [61, 58, 67]), dividers);
        }
        assert.deepEqual(await serverRow(canvasColor), [[68, 86]]);
        assert.deepEqual(await configRow(paneColor), [[110, 128]]);
        assert.deepEqual(await verticalRuns(page, 176, 60, 96, [243, 129, 74]), [[63, 93]]);
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
        assert.deepEqual(await serverRow(paneColor), [[68, 86]]);
        assert.deepEqual(await configRow(canvasColor), [[110, 128]]);
        assert.deepEqual(await verticalRuns(page, 176, 110, 128, [243, 129, 74]), [[110, 128]]);

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
        await click(page, 176, desktop.height - 35 - donationSlotHeight);
        await status(page, /^Wave About\./);
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

test('records About donation answers and shows the next-month reminder', async () => {
    const page = await browser.newPage({ viewport: desktop, hasTouch: true });
    const data = fixture();
    const preview = await mockPreview(page, data);
    const buttonFace = [69, 64, 79];
    const yellowFace = [255, 195, 106];
    const linkColor = [81, 165, 246];
    const desktopBadge = { x: 22, y: 743, width: 36, height: 36 };
    const choices = /did you donate anything to the project this month\? no\. yes, below 5\$\. yes, more than 5\$\./;
    const inlineChoices = page.getByRole('img', { name: choices });
    const picker = page.getByRole('img', { name: new RegExp(`Change answer\\. ${choices.source} Cancel\\.`) });
    const reminder = page.getByRole('img', { name: /The sidebar badge will remind you next month/ });
    const modal = page.getByRole('img', { name: /Donation response/ });
    const clickAt = (x, y) => click(page, x, y);
    const tapAt = async (x, y) => {
        await page.touchscreen.tap(x, y);
        await settleCanvas(page);
    };
    const activateDonateLink = async (left, top, bottom) => {
        await status(page, /Donate link\./);
        const description = await page.locator('#canvas').getAttribute('aria-label');
        assert.equal(description.split('Donate link.').length, 2, 'The donation link is announced once.');
        const openedBefore = await page.evaluate(() => window.openedLinks.length);
        await page.mouse.move(0, 0);
        await settleCanvas(page);
        const link = { x: left, y: top, width: 60, height: bottom - top };
        assert.ok(await rectangleContainsColor(page, link, linkColor), 'Donate is visible in the expected position.');
        await clickAt(left + 16, Math.floor((top + bottom) / 2));
        const opened = await page.evaluate(() => window.openedLinks);
        assert.deepEqual(opened.slice(openedBefore), [
            ['https://radkesvat-support.vercel.app/#support', '_blank', 'noopener,noreferrer'],
        ]);
    };
    const expectBadge = text => page.waitForFunction(text =>
        document.getElementById('navigation').getAttribute('aria-description') === text, text, { timeout });
    const dialogBodyLeft = () => {
        const { width } = page.viewportSize();
        return Math.floor((width - Math.min(420, width - 24)) / 2) + 12;
    };
    const expectCurrentAnswer = async label => {
        await status(page, `Saved answer is ${label}. Next reminder 27/01/01 (UTC). Change answer.`);
        assert.equal(await inlineChoices.count(), 0, 'A current positive answer hides the inline options.');
    };
    const openChangeAnswer = async (activate, x, scanTop) => {
        const [button] = await verticalRuns(page, x, scanTop, 580, buttonFace, 26);
        assert.ok(button, 'Change answer is available below the summary.');
        await activate(x, Math.floor((button[0] + button[1]) / 2));
        await picker.waitFor({ timeout });
        // The opening tap can leave a choice hovered at the same coordinates.
        await page.mouse.move(0, 0);
        const { top, bottom } = await dialogFrame(page);
        const choiceX = dialogBodyLeft() + 4;
        const rows = await verticalRuns(page, choiceX, top + 35, bottom - 45, buttonFace, 28);
        return { choiceX, rows };
    };
    const respond = async (index, answer, label, response, activate, x, y) => {
        await activate(x, y);
        const request = await preview.action(index);
        assertRequest(request.request, { action: 'apply_donation', answer });
        await modal.waitFor({ timeout });
        assert.equal(await picker.count(), 0, 'The donation response replaces the choice dialog.');
        await status(page, response);
        await status(page, /Operation in progress\./);
        assert.equal(await reminder.count(), 0);
        data.settings.donation = { answer, answered_at: '2026-12-31T23:30:00Z', reminder_due: false };
        request.reply();
        await status(page, `Saved answer is ${label}.`);
        if (answer === 'no') {
            assert.equal(await reminder.count(), 0);
        } else {
            await reminder.waitFor({ timeout });
        }
        const { top } = await dialogFrame(page);
        const left = dialogBodyLeft();
        const bodyTop = top + 35;
        const leading = { x: left, y: bodyTop, width: 24, height: 24 };
        if (answer === 'more_than_5') {
            const portraitRegion = { x: left, y: bodyTop, width: 64, height: 48 };
            assert.ok(await grayscalePixelCount(page, portraitRegion, 106, 145) > 750,
                'The highest-tier dialog shows the grayscale portrait at the start of its body.');
        } else if (answer === 'no') {
            const trailing = { x: left + 190, y: bodyTop, width: 170, height: 22 };
            assert.equal(await rectangleContainsColor(page, leading, yellowFace), false,
                'The No response starts with its message.');
            assert.ok(await rectangleColorCount(page, trailing, yellowFace) > 40,
                'The No response has a yellow face after the message on the same line.');
            await status(page, /Donation response\. the project is free, no problem Donate link\./);
            await activateDonateLink(left, bodyTop + 18, bodyTop + 45);
        } else {
            assert.ok(await rectangleContainsColor(page, leading, yellowFace),
                'The below-$5 dialog keeps its leading yellow face.');
        }
        await confirmDialog(page, activate, 420);
        await modal.waitFor({ state: 'hidden', timeout });
        await status(page, `Saved answer is ${label}.`);
    };

    try {
        await page.addInitScript(() => {
            window.openedLinks = [];
            window.open = (...args) => {
                window.openedLinks.push(args);
                return null;
            };
        });
        await page.goto(url);
        await waitForServerStatus(page);
        await expectBadge('no donation');
        await openView(page, 'About');
        await status(page, choices);
        assert.equal(preview.actions.length, 0, 'Opening About leaves the question unanswered.');

        const [row] = await verticalRuns(page, 203, 113, 310, buttonFace, 28);
        assert.ok(row, 'The donation choices are visible together on desktop.');
        const choiceY = Math.floor((row[0] + row[1]) / 2);
        const topChoiceSticker = { x: 407, y: choiceY - 12, width: 24, height: 24 };
        await activateDonateLink(199, row[1], row[1] + 32);
        assert.equal(await rectangleContainsColor(page, {
            x: 198, y: row[1] + 40, width: 150, height: 70,
        }, linkColor), true, 'Project links follow the Donate link.');
        for (const x of [206, 271]) {
            assert.ok(await rectangleContainsColor(page, { x, y: choiceY - 9, width: 18, height: 18 }, yellowFace),
                'The lower-tier button keeps its yellow face.');
        }
        assert.ok(await grayscalePixelCount(page, topChoiceSticker, 60, 105) > 225,
            'The highest-tier button shows the round grayscale image beside its label.');

        await respond(0, 'below_5', 'yes, below 5$', /Thank you for supporting the project!/,
            clickAt, 279, choiceY);
        await expectBadge('donated');
        await expectCurrentAnswer('yes, below 5$');
        assert.ok(await grayscalePixelCount(page, topChoiceSticker, 60, 105) < 20,
            'The highest-tier inline choice is no longer drawn beside the summary.');
        assert.ok(await rectangleContainsColor(page, desktopBadge, yellowFace),
            'The lower-tier donation badge keeps its yellow face.');

        const desktopPicker = await openChangeAnswer(clickAt, 203, 113);
        assert.equal(desktopPicker.rows.length, 1, 'The desktop change dialog shows the choices in a row.');
        await confirmDialog(page, clickAt, 420);
        await picker.waitFor({ state: 'hidden', timeout });
        await expectCurrentAnswer('yes, below 5$');
        assert.equal(preview.actions.length, 1, 'Cancel preserves the saved answer without submitting.');

        await page.reload();
        await waitForServerStatus(page);
        await expectBadge('donated');
        await openView(page, 'About');
        await expectCurrentAnswer('yes, below 5$');
        assert.equal(preview.actions.length, 1, 'Reloading restores the saved answer without submitting it.');

        const noPicker = await openChangeAnswer(clickAt, 203, 113);
        const [noTop, noBottom] = noPicker.rows[0];
        await respond(1, 'no', 'no', /Donation response\. the project is free, no problem(?= Operation in progress\. Donate link\.)/,
            clickAt, noPicker.choiceX, Math.floor((noTop + noBottom) / 2));
        await expectBadge('no donation');
        await inlineChoices.waitFor({ timeout });

        await respond(2, 'below_5', 'yes, below 5$', /Thank you for supporting the project!/,
            clickAt, 279, choiceY);
        await expectCurrentAnswer('yes, below 5$');

        await page.setViewportSize(portrait);
        await settleCanvas(page);
        const touchPicker = await openChangeAnswer(tapAt, 25, 245 + donationSlotHeight);
        assert.equal(touchPicker.rows.length, 3, 'The change dialog stacks the choices on a narrow screen.');
        const [lastTop, lastBottom] = touchPicker.rows[2];
        await respond(3, 'more_than_5', 'yes, more than 5$', /Thank you for supporting the project!/,
            tapAt, touchPicker.choiceX, Math.floor((lastTop + lastBottom) / 2));
        await expectBadge('donated');
        await expectCurrentAnswer('yes, more than 5$');
        await page.setViewportSize(desktop);
        await settleCanvas(page);
        assert.ok(await grayscalePixelCount(page, desktopBadge, 60, 105) > 550,
            'The highest-tier donation badge shows the round grayscale image beside its label.');

        data.settings.donation.reminder_due = true;
        await preview.broadcast();
        await expectBadge('donation?');
        await inlineChoices.waitFor({ timeout });
        await status(page, 'Last answer was yes, more than 5$.');
        assert.ok(await grayscalePixelCount(page, topChoiceSticker, 60, 105) > 225,
            'The next-month reminder restores the illustrated inline choices.');
        assert.equal(await page.getByRole('img', { name: /Next reminder/ }).count(), 0);
        assert.equal(preview.actions.length, 4, 'The next-month reminder does not submit an answer.');
    } finally {
        await page.close();
    }
});

test('shows the Tools demo section only when enabled in the build', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const showDemos = process.env.WAVE_EXPECT_UI_DEMOS === '1';
    try {
        await mockPreview(page, fixture());
        await page.goto(url);
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

test('loads workspace pages without passive spinners', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const config = { name: 'Default', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [{ id: 1, name: config.name, enabled: false, active: false, auth_nodes: ['staff'] }];
    const firstSnapshot = Promise.withResolvers();
    const preview = await mockPreview(page, data, firstSnapshot.promise);
    const expectNoSpinner = async (view, clip) => {
        await settleCanvas(page);
        assert.equal(await rectangleContainsColor(page, clip, [81, 165, 246]), false,
            `${view} waits for its initial data without a spinner.`);
    };
    try {
        await page.goto(url);
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
        const userControls = { x: 198, y: 121, width: 310, height: 60 };
        const userList = { users: [{ id: '1', name: 'Fixture account' }], editable: true,
            page: 0, page_count: 1, total: 1 };
        const users = await preview.action(1);
        assertRequest(users.request, { action: 'get_config_users', config_id: 1, node: 'staff', page: 0 });
        await status(page, /staff tab\. AuthenticationServer users\. Loading users\./);
        await expectNoSpinner('Offline users', userControls);
        users.reply({ result: userList });
        await status(page, /staff tab\. AuthenticationServer users\. 1 users?\./);

        await click(page, 343, 137);
        const refreshed = await preview.action(2);
        assertRequest(refreshed.request, { action: 'get_config_users', config_id: 1, node: 'staff', page: 0 });
        assert.equal(await rectangleContainsColor(page, userControls, [81, 165, 246]), true,
            'Refresh users shows a spinner while the explicit request is pending.');
        refreshed.reply({ result: userList });
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
        { name: 'Default', canvas: '080c10', terminal: '050506', track: '0d0b0e', thumb: '8a8495' },
        { name: 'Light', canvas: 'f0f2f7', terminal: 'f9fafd', track: 'ffffff', thumb: '687189' },
        { name: 'Classic', canvas: '1b2529', terminal: '121b21', track: '141a22', thumb: '778999' },
        { name: 'Dark', canvas: '171a20', terminal: '101217', track: '111318', thumb: '808ba1' },
        { name: 'Darker', canvas: '08090b', terminal: '030405', track: '050608', thumb: '787b84' },
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
            assert.deepEqual(await verticalRuns(page, 11, 68, 86, channels(theme.canvas)), [[68, 86]],
                `${theme.name} recolors the ImGui workspace.`);
        };

        await page.goto(url);
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
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    data.settings.theme = 'default';
    const preview = await mockPreview(page, data);
    try {
        await page.goto(url);
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
        await page.goto(url);
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
        await status(page, /This selects a release of the Wave panel/);
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
    const page = await browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
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
        await page.goto(url);
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
    const page = await browser.newPage({ viewport: coreDesktop });
    const data = fixture('missing');
    const core = data.server.core;
    const preview = await mockPreview(page, data);
    const press = action => lifecycleButton(page, action);
    const controlsTitle = () => verticalRuns(page, 510, 250, 650, [43, 39, 48], 14);
    const progress = /Operation in progress\./;
    const progressIndicator = page.getByRole('img', { name: progress });
    const spinnerColor = [81, 165, 246];
    const modalSpinner = frame => rectangleContainsColor(page, {
        x: (coreDesktop.width - 380) / 2 + 10, y: frame.bottom - 42, width: 40, height: 34,
    }, spinnerColor);
    try {
        await page.goto(url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core missing\. Panel version 1\.6\.0/);
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
        await status(page, /WaterWall core missing\. Panel version/);
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
        await press('restart');
        assert.equal(await page.locator('#panel-controls').evaluate(controls => controls.inert), false);
        await press('start');
        const start = await preview.action(2);
        assertRequest(start.request, { action: 'start_core' });
        await progressIndicator.waitFor({ timeout });
        core.state = 'starting';
        start.reply();
        await status(page, /WaterWall core starting.*Operation in progress\./);
        core.state = 'running';
        await preview.broadcast();
        await status(page, /WaterWall core running/);
        await progressIndicator.waitFor({ state: 'hidden', timeout });

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

test('opens settings field help by mouse, keyboard, and touch without reflowing inputs', async () => {
    const page = await browser.newPage({ viewport: desktop, hasTouch: true });
    const preview = await mockPreview(page, fixture());
    const description = /Use a hostname such as panel\.example\.test\./;
    const help = page.getByRole('img', { name: description });
    const inputRows = () => verticalRuns(page, 220, 158, 304, [13, 11, 14], 20);
    const marker = { x: 280, y: 148 };
    try {
        await page.goto(url);
        await openView(page, 'Settings');
        await status(page, /Panel tab\. Administrator settings/);
        await settleCanvas(page);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), description);
        const originalRows = await inputRows();
        assert.equal(originalRows.length, 3, 'Expected the preview TLS input rows.');

        await page.mouse.move(marker.x, marker.y);
        await help.waitFor({ timeout });
        assert.deepEqual(await inputRows(), originalRows, 'Help leaves the TLS fields in place.');
        await page.mouse.move(0, 0);
        await help.waitFor({ state: 'hidden', timeout });

        await click(page, 325, 175);
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
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
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
        await page.goto(url);
        await openView(page, 'Settings');
        await status(page, /Panel tab\. Administrator settings/);
        const panelFrame = await tabBodyFrame();
        const cardTop = async (left, right) => {
            const edge = x => verticalRuns(page, x, 101, 410, [61, 58, 67], 80);
            const bounds = await edge(left);
            assert.equal(bounds.length, 1, 'Expected the left edge of the Settings card.');
            assert.deepEqual(await edge(right), bounds, 'The Settings card stays within its column.');
            return bounds[0][0];
        };
        const tops = [await cardTop(205, 548), await cardTop(556, 899), await cardTop(907, 1250)];
        assert.deepEqual(tops, [106, 106, 106], 'All three compact Settings cards align at the top.');
        const domainInput = x => verticalRuns(page, x, 160, 190, [13, 11, 14], 15);
        assert.deepEqual(await domainInput(212), [], 'The Domain input leaves room beside the card border.');
        assert.equal((await domainInput(220)).length, 1, 'The Domain input begins inside the card padding.');
        const tls = { ...data.settings, domain: 'updated.example.invalid' };
        await replaceText(page, 325, 175, tls.domain);
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
        await status(page, /Panel tab\. Administrator settings/);
        await click(page, 245, 320);
        await confirmDialog(page, (x, y) => click(page, x, y), 370);
        const panelSave = await preview.action(0);
        assertRequest(panelSave.request, { action: 'apply_settings', settings: tls });
        data.settings = tls;
        panelSave.reply({ feedback: 'Applied sample TLS settings.', feedback_in_logs_only: true });
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

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

test('shows the footer spinner when reloading a config with unsaved changes', async () => {
    const page = await browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'Reload fixture', nodes: [{ name: 'saved', type: 'BlackHole', settings: {} }] };
    data.server.core.configs = [{ id: 7, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const confirmReload = () => confirmDialog(page, (x, y) => click(page, x, y), 460);
    const spinner = frame => rectangleContainsColor(page, {
        x: (desktop.width - 460) / 2 + 10, y: frame.bottom - 42, width: 36, height: 34,
    }, [81, 165, 246]);
    try {
        await page.goto(url);
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

test('edits and saves session expiry from Panel Settings', async () => {
    const page = await browser.newPage({ viewport: coreDesktop });
    const data = fixture();
    const preview = await mockPreview(page, data);
    try {
        await page.goto(url);
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

        await page.setViewportSize({ ...portrait, height: coreDesktop.height });
        await settleCanvas(page);
        const stacked = await verticalRuns(page, 27,
            251 + donationSlotHeight, 867 + donationSlotHeight, [61, 58, 67], 20);
        assert.equal(stacked.length, 3, 'The cards stack along the same left edge at phone width.');
    } finally {
        await page.close();
    }
});

test('renders a full-screen terminal, forwards keystrokes and resize, and closes on navigation', async () => {
    const viewport = { width: 1920, height: 978 };
    const page = await browser.newPage({ viewport });
    const preview = await mockPreview(page, fixture());
    const connected = Promise.withResolvers();
    const messages = new EventEmitter();
    const frames = [];
    let connections = 0;
    let closed = false;
    await page.routeWebSocket(consoleUrl, socket => {
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
        await page.goto(url);
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

function waitForServerStatus(page) {
    return status(page, /Wave Server Status.*CPU 25\.0%/);
}

test('reserves space for a whole paste until the terminal acknowledges input bytes', async () => {
    const page = await browser.newPage({ viewport: desktop });
    try {
        await mockPreview(page, fixture());
        const inputs = [];
        let initialResize;
        let socket;
        await page.routeWebSocket(consoleUrl, route => {
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
        await page.goto(url);
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

test('creates a config with authentication and API starter nodes selected', async () => {
    const page = await browser.newPage({ viewport: desktop });
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
        await page.goto(url);
        await waitForServerStatus(page);
        const name = 'Starter fixture';
        const top = await openCreate(name);
        const [checkbox] = await verticalRuns(page, 474, top + 87, top + 137, [13, 11, 14], 15);
        assert.ok(checkbox, 'Expected the unchecked starter nodes option.');
        await click(page, 474, (checkbox[0] + checkbox[1]) / 2);
        await status(page, /Add API and authentication database nodes checked\./);
        await confirmDialog(page, activate);

        const created = await preview.action(0);
        assertRequest(created.request, { action: 'create_core_config', name, add_auth_nodes: true });
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

test('uses the sidebar and creates, toggles, and deletes a config on touch', async () => {
    const page = await browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
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
        await page.goto(url);
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
        assert.deepEqual(await verticalRuns(page, rightEdge, footerY - 9, footerY + 7, [243, 129, 74]),
            [[footerY - 9, footerY + 7]]);
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
        const canvasTop = () => graphCanvasTop(page, 180, 376 + donationSlotHeight, 475 + donationSlotHeight);
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

test('downloads a backup and restores it through the browser file picker', async () => {
    const page = await browser.newPage({ viewport: coreDesktop });
    const data = fixture();
    data.server.core.configs.push({ id: 2, name: 'Standby', enabled: false, active: false });
    const preview = await mockPreview(page, data);
    try {
        await page.goto(url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core running/);
        await settleCanvas(page);
        const backupX = 510;
        const restoreX = 675;
        const backupRows = await verticalRuns(page, backupX, 400, 820, [69, 64, 79], 20);
        assert.ok(backupRows.length, 'Expected the Backup control.');
        const [top, bottom] = backupRows.at(-1);
        const rowY = Math.floor((top + bottom) / 2);
        const downloading = page.waitForEvent('download', { timeout });
        await click(page, backupX, rowY);
        const download = await downloading;
        const buffer = await readFile(await download.path());
        const backup = JSON.parse(buffer);
        assert.deepEqual(backup.settings, data.settings);
        assert.deepEqual(backup.configs, [{ name: 'Standby', enabled: false }]);

        await click(page, restoreX, rowY);
        await status(page, /Restore backup/);
        const chooseBackup = async buffer => {
            const choosing = page.waitForEvent('filechooser', { timeout });
            await click(page, 560, (await dialogFrame(page)).top + 80);
            await (await choosing).setFiles({ name: download.suggestedFilename(), mimeType: 'application/json', buffer });
        };
        const duplicate = Buffer.from(JSON.stringify(backup).replace('"configs":', '"configs":[],"configs":'));
        await chooseBackup(duplicate);
        await status(page, /Choose a valid Wave preview backup\./);
        await chooseBackup(buffer);
        await status(page, /Wave preview backup selected/);
        assert.equal(preview.actions.length, 0, 'Selecting a file waits for confirmation.');
        const restoreFrame = await dialogFrame(page);
        await confirmDialog(page, (x, y) => click(page, x, y));
        const restored = await preview.action(0);
        assertRequest(restored.request, { action: 'restore_preview_backup', backup });
        await status(page, /Restore backup\..*Operation in progress\./);
        assert.deepEqual(await dialogFrame(page), restoreFrame, 'Progress keeps the dialog at its original size.');
        restored.reply({ feedback: 'Restored backup.', feedback_in_logs_only: true });
        await status(page, /Latest log: Restored backup/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
    } finally {
        await page.close();
    }
});

test('edits a WaterWall node graph and reopens the saved JSON', async () => {
    const page = await browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
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
        await page.goto(url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 7 });
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Wave Config Lab graph. Nodes tab. Nodes. 1 node\./);

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

        const canvas = await desktopGraphBounds(page);
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
        await click(page, 437, canvasTop + 93);
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
        const branchArrow = await verticalRuns(page, 894, branchTop + 145, branchTop + 210, [255, 195, 106]);
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
        await page.evaluate(() => navigator.clipboard.writeText(''));
        await confirmDialog(page, activate, 740);
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0);
        assert.deepEqual(JSON.parse(await page.evaluate(() => navigator.clipboard.readText())), document);
    } finally {
        await page.close();
    }
});

test('uses the graph context menu and centers new nodes after panning and zooming', async () => {
    const page = await browser.newPage({ viewport: desktop });
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
        await page.goto(url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const loaded = await preview.action(0);
        assertRequest(loaded.request, { action: 'get_core_config', id: 17 });
        loaded.reply({ result: { config, revision: 'loaded', editable: true } });
        await status(page, /Wave Config Menu fixture\. Nodes tab\. Nodes\. 1 node\./);
        const canvas = await desktopGraphBounds(page);
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
    const page = await browser.newPage({ viewport });
    const data = fixture('stopped');
    const config = { name: 'Compact graph', nodes: [] };
    data.server.core.configs = [{ id: 18, name: config.name, enabled: false, active: false }];
    const preview = await mockPreview(page, data);
    const graphTop = () => graphCanvasTop(page, 58, 345 + donationSlotHeight, 430 + donationSlotHeight);
    try {
        await page.goto(url);
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

        await click(page, 47, 327 + donationSlotHeight);
        const saved = await preview.action(1);
        const position = saved.request.config.nodes[0]['panel-settings'];
        assertNodePosition(position, { x: viewport.width / 2 - 21 - 74, y: centerY - top - 29 });
        assertRequest(saved.request, { action: 'save_core_config', id: 18, revision: 'loaded', config: {
            ...config, nodes: [{ name: 'Compact center', type: 'BlackHole', settings: {}, 'panel-settings': position }],
        } });
    } finally {
        await page.close();
    }
});

test('preserves imported settings during form edits and locks both kinds of controls in Custom JSON', async () => {
    const page = await browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
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
        await page.goto(url);
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
    const page = await browser.newPage({ viewport: desktop, permissions: ['clipboard-read', 'clipboard-write'] });
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
        await page.goto(url);
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
        await status(page, /Wave Config Import fixture\..*1 node\./);
        saved.reply({ feedback: 'Saved Import fixture.', feedback_in_logs_only: true,
            result: { config: document, revision: 'after-import', editable: true } });
        await status(page, /Wave Config Import fixture\..*2 nodes\..*Latest log: Saved Import fixture\./);
    } finally {
        await page.close();
    }
});

test('shows only the connected side of each packet adapter and both sides before wiring', async () => {
    const page = await browser.newPage({ viewport: desktop });
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
    const graphLeft = 199;
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
        await page.goto(url);
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

async function selectConfigTab(page, name) {
    await page.getByRole('tab', { name, exact: true }).focus();
    await page.keyboard.press('Enter');
    await page.getByRole('tab', { name, exact: true, selected: true }).waitFor({ timeout });
}

test('edits WaterWall user access and limits and creates a user with an exact ID while offline', async () => {
    const page = await browser.newPage({ viewport: coreDesktop, permissions: ['clipboard-read', 'clipboard-write'] });
    const data = fixture('stopped');
    const config = { name: 'Offline fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [
        { id: 11, name: config.name, enabled: true, active: false, auth_nodes: ['staff'] },
    ];
    const user = {
        id: '9007199254740993', name: 'Fixture account', enabled: false, gid: '4', 'wireguard-allowed-ips': '',
        time: { 'created-at-ms': '1920000000000', 'expire-at-ms': '1924992000000' },
        limit: { traffic: { up: '9875654321', down: '1000', total: '18446744073709551615' } },
        stats: { traffic: { up: '20', down: '30' } },
    };
    const preview = await mockPreview(page, data);
    const selector = { config_id: 11, node: 'staff', page: 0 };
    const passwordSelector = { action: 'get_config_user_password', config_id: 11, node: 'staff', user_id: user.id };
    const storedPassword = 'fixture-saved-credential';
    const listing = record => ({ users: [record], editable: true, page: 0, page_count: 1, total: 1 });
    try {
        await page.goto(url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 11 });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });

        await selectConfigTab(page, 'staff');
        const read = await preview.action(1);
        assertRequest(read.request, { action: 'get_config_users', ...selector });
        read.reply({ result: listing(user) });
        await status(page, /staff tab\. AuthenticationServer users\..*1 users?\./);

        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const passwordRead = await preview.action(2);
        assertRequest(passwordRead.request, passwordSelector);
        await status(page, /Loading saved password\./);
        await selectConfigTab(page, 'Nodes');
        await selectConfigTab(page, 'staff');
        await status(page, /Edit user\. Account section\. Loading saved password\./);
        assert.equal(preview.actions.length, 3, 'Returning to the editor keeps its pending password lookup.');
        passwordRead.reply({ result: { password: storedPassword } });
        assert.ok(await readUserField(page, 300, 317) === storedPassword, 'The saved password is visible in the editor.');
        assert.ok(!(await page.locator('#canvas').getAttribute('aria-label')).includes(storedPassword));
        await click(page, 237, 747);
        assert.equal(preview.actions.length, 3, 'The saved password alone leaves the editor unchanged.');
        await replaceText(page, 300, 317, '');
        await click(page, 306, 166);
        await status(page, /Edit user\. Access section\./);
        await replaceText(page, 300, 279, '18446744073709551616');
        await status(page, /Group ID requires a whole number/);
        await click(page, 237, 747);
        assert.equal(preview.actions.length, 3);
        const gid = '18446744073709551615';
        const allowedIps = '192.0.2.15/32';
        await replaceText(page, 300, 279, gid);
        await replaceText(page, 820, 279, allowedIps);
        await replaceText(page, 820, 350, '');

        await click(page, 370, 166);
        await status(page, /Edit user\. Limits section\./);
        assert.equal(await readUserField(page, 300, 298), '9.875');
        assert.equal(await readUserField(page, 820, 298), '0.000');
        for (const invalid of ['10.5312', '18446744073.710']) {
            await replaceText(page, 300, 298, invalid);
            await status(page, /Upload traffic \(GB\).*three decimal places/);
            await click(page, 237, 747);
            assert.equal(preview.actions.length, 3, 'Invalid traffic quotas cannot be saved.');
        }
        await replaceText(page, 300, 298, '10.531');
        await replaceText(page, 820, 298, '');
        await click(page, 433, 166);
        await status(page, /Edit user\. Activity section\./);
        await click(page, 237, 747);
        const save = await preview.action(3);
        assertRequest(save.request, {
            action: 'edit_config_user', ...selector, user_id: user.id,
            user: { gid, 'wireguard-allowed-ips': allowedIps, time: { 'expire-at-ms': null },
                limit: { traffic: { up: '10531000000', down: null } } },
        });
        const updated = { ...user, gid, 'wireguard-allowed-ips': allowedIps,
            time: { 'created-at-ms': user.time['created-at-ms'] },
            limit: { traffic: { up: '10531000000', total: user.limit.traffic.total } } };
        await selectConfigTab(page, 'Nodes');
        save.reply({ feedback: 'Saved fixture user.', feedback_in_logs_only: true, result: listing(updated) });
        await selectConfigTab(page, 'staff');
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);
        await expectLoggedOnce(page, 'Saved fixture user.');
        assert.equal(preview.actions.length, 4, 'Returning to the tab preserves the pending save result.');

        await page.setViewportSize({ width: 1600, height: 900 });
        await settleCanvas(page);
        const cellText = [181, 180, 197];
        for (const [label, left, lastGlyph] of [['Group ID', 1152, 1341], ['Disabled state', 1358, 1428]]) {
            assert.ok(await rectangleContainsColor(page, {
                x: lastGlyph, y: 226, width: 1, height: 18,
            }, cellText), `${label} displays its final glyph on the first line.`);
            assert.equal(await rectangleContainsColor(page, {
                x: left, y: 248, width: lastGlyph - left + 2, height: 80,
            }, cellText), false, `${label} stays on one line.`);
        }
        await page.setViewportSize(coreDesktop);
        await settleCanvas(page);

        const replacementPassword = 'fixture-replacement-credential';
        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const previousPassword = await preview.action(4);
        assertRequest(previousPassword.request, passwordSelector);
        previousPassword.reply({ result: { password: storedPassword } });
        await replaceText(page, 300, 317, replacementPassword);
        await click(page, 237, 747);
        const replacement = await preview.action(5);
        const passwordChange = {
            action: 'edit_config_user', ...selector, user_id: user.id, user: { password: replacementPassword },
        };
        assertRequest(replacement.request, passwordChange);
        replacement.reply({ ok: false, feedback: 'Fixture password save failed.', feedback_in_logs_only: true });
        await expectLoggedOnce(page, 'Fixture password save failed.');
        assert.ok(await readUserField(page, 300, 317) === replacementPassword,
            'A rejected save leaves the replacement available to retry.');
        await click(page, 237, 747);
        const retryPassword = await preview.action(6);
        assertRequest(retryPassword.request, passwordChange);
        retryPassword.reply({ result: listing(updated) });
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);

        await click(page, 1163, 207);
        await status(page, /Edit user\. Account section\./);
        const savedPassword = await preview.action(7);
        assertRequest(savedPassword.request, passwordSelector);
        savedPassword.reply({ result: { password: replacementPassword } });
        assert.ok(await readUserField(page, 300, 317) === replacementPassword,
            'Reopening the editor shows the replacement password.');
        await click(page, 344, 747);
        await status(page, /staff tab\. AuthenticationServer users\. 1 users\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-new-credential';
        const id = '9007199254740995';
        await replaceText(page, 300, 317, password);
        await replaceText(page, 820, 317, id);
        await click(page, 242, 747);
        const create = await preview.action(8);
        assertRequest(create.request, {
            action: 'create_config_user', ...selector, user: { id, password, enabled: true },
        });
        create.reply({ result: { ...listing(updated), users: [updated, { id, enabled: true }], total: 2 } });
        await status(page, /staff tab\. AuthenticationServer users\. 2 users\./);

        await page.setViewportSize(portrait);
        await settleCanvas(page);
        await click(page, 326, 420 + donationSlotHeight);
        await status(page, /Delete user\./);
        assert.equal(preview.actions.length, 9, 'The mobile Delete action opens its confirmation without deleting the user.');
    } finally {
        await page.close();
    }
});

test('lists and refreshes AuthenticationServer live users while offline editing is locked', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture('stopped');
    const config = { name: 'Live fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    const id = 2147483646;
    data.server.core.configs = [
        { id, name: config.name, enabled: true, active: false, auth_nodes: ['staff'], live_users: true },
    ];
    const preview = await mockPreview(page, data);
    const listRequest = { action: 'get_live_config_users', config_id: id, node: 'staff' };
    const cursor = '12'.repeat(32);
    const user = { id: '9007199254740993', name: 'Fixture account', enabled: true, usage: { u: '2048', d: '4096' } };
    try {
        await page.goto(url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id });
        opened.reply({ result: { config, revision: 'fixture', editable: true } });
        await status(page, /Nodes tab\. Nodes\. 1 node\./);
        data.server.core.state = 'running';
        data.server.core.configs[0].active = true;
        data.logs[0].message = 'Fixture config started.';
        await preview.broadcast();
        await status(page, /Latest log: Fixture config started\./);
        await graphMenuItem(page, 'nodes', 0);
        assert.doesNotMatch(await page.locator('#canvas').getAttribute('aria-label'), /Add node\./,
            'An active config with a large ID locks its graph.');

        await selectConfigTab(page, 'staff');
        await status(page, /staff tab\. AuthenticationServer users\..*Stop WaterWall to access this users database\./);
        assert.equal(preview.actions.length, 1, 'An active process blocks offline user access.');

        await selectConfigTab(page, 'staff Live');
        const first = await preview.action(1);
        assertRequest(first.request, listRequest);
        first.reply({ result: { available: true, total: 2, users: [user], next_cursor: cursor } });
        await status(page, /staff Live tab\. AuthenticationServer live users\..*2 users\..*Page 1\./);

        await click(page, 300, 137);
        const next = await preview.action(2);
        assertRequest(next.request, { ...listRequest, cursor });
        next.reply({ result: { available: true, total: 2, users: [{ ...user, id: '2' }], next_cursor: null } });
        await status(page, /staff Live tab\..*2 users\..*Page 2\./);
        const refresh = await preview.action(3);
        assertRequest(refresh.request, { ...listRequest, cursor });
        refresh.reply({ ok: false, feedback: 'The live cursor is no longer current.', result: { reset_cursor: true } });

        const restarted = await preview.action(4);
        assertRequest(restarted.request, listRequest);
        restarted.reply({ result: { available: true, total: 1, users: [user], next_cursor: null } });
        await status(page, /staff Live tab\..*1 users?\./);
    } finally {
        await page.close();
    }
});
