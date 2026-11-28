const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
const { readFile } = require('node:fs/promises');
const { createServer } = require('node:http');
const { join } = require('node:path');
const { after, before } = require('node:test');
const { chromium } = require('playwright-core');

const desktop = { width: 1280, height: 800 };
const coreDesktop = { width: 1280, height: 1080 };
// Click positions inside the users table at the coreDesktop viewport.
const rosterColumnX = {
    enable: 555, options: 635, trafficUsage: 756, uploadSpeed: 891, downloadSpeed: 1052, daysLeft: 1198,
};
const portrait = { width: 390, height: 844 };
const donationSlotHeight = 63;
const timeout = 10000;
const basePath = '/fixture/wave';

function setupWorkspace() {
    let url;
    let previewUrl;
    let sessionUrl;
    let consoleUrl;
    let server;
    let browser;
    let waterwallSchematic;

    before(async () => {
        const siteDir = process.env.WAVE_SITE_DIR;
        assert.ok(siteDir, 'Run this test through a Wave frontend CTest preset.');
        const schematicText = await readFile(join(__dirname, '../../backend/schematics/waterwall-1.6.0.json'), 'utf8');
        waterwallSchematic = JSON.parse(schematicText, (_key, value, context) =>
            Number.isInteger(value) && !Number.isSafeInteger(value) && /^-?\d+$/.test(context.source)
                ? BigInt(context.source) : value);
        const assets = new Map();
        for (const [path, filename, contentType] of [
            ['/', 'index.html', 'text/html; charset=utf-8'],
            ['/index.js', 'index.js', 'text/javascript; charset=utf-8'],
            ['/index.wasm', 'index.wasm', 'application/wasm'],
            ['/console.js', 'console.js', 'text/javascript; charset=utf-8'],
            ['/json-editor.js', 'json-editor.js', 'text/javascript; charset=utf-8'],
            ['/json-editor/vanilla-jsoneditor.js', 'json-editor/vanilla-jsoneditor.js', 'text/javascript; charset=utf-8'],
            ['/json-editor/lossless-json.js', 'json-editor/lossless-json.js', 'text/javascript; charset=utf-8'],
            ['/json-editor/jse-theme-dark.css', 'json-editor/jse-theme-dark.css', 'text/css; charset=utf-8'],
            ['/terminal/xterm.js', 'terminal/xterm.js', 'text/javascript; charset=utf-8'],
            ['/terminal/addon-fit.js', 'terminal/addon-fit.js', 'text/javascript; charset=utf-8'],
            ['/terminal/xterm.css', 'terminal/xterm.css', 'text/css; charset=utf-8'],
        ]) {
            assets.set(basePath + path, { contentType, contents: await readFile(join(siteDir, filename)) });
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
        url = `http://${host}${basePath}/`;
        sessionUrl = `ws://${host}${basePath}/ws/panel`;
        previewUrl = `ws://${host}${basePath}/ws/workspace`;
        consoleUrl = `ws://${host}${basePath}/ws/console`;
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

    async function mockPreview(page, data, gate = Promise.resolve(), schematic = waterwallSchematic,
        usersRoster = true) {
        let sessionId = 'fixture-sign-in';
        let identitySocket;
        await page.routeWebSocket(sessionUrl, socket => {
            identitySocket = socket;
            socket.send(JSON.stringify({
                type: 'session', username: 'fixture-admin', session_id: sessionId,
                expires_at: Math.floor(Date.now() / 1000) + 86400,
            }));
        });
        const connected = Promise.withResolvers();
        const events = new EventEmitter();
        const actions = [];
        const rosterActions = [];
        let nextAttempt = 1;
        let previewSocket;
        let sendSnapshot;
        await page.routeWebSocket(previewUrl, async socket => {
            const send = fields => socket.send(JSON.stringify({ type: 'snapshot', data, ...fields },
                (_key, value) => typeof value === 'bigint' ? JSON.rawJSON(value.toString()) : value));
            socket.onMessage(raw => {
                const request = JSON.parse(raw);
                if (request.action.startsWith('users_roster_')) {
                    rosterActions.push(request);
                    events.emit('roster-action');
                    return;
                }
                const reply = ({ ok = true, feedback = '', result, feedback_in_logs_only,
                    level = ok ? 'success' : 'error' } = {}) => {
                    if (feedback_in_logs_only && feedback) {
                        data.logs.push({ source: 'panel', ts: '09:41:03', level, message: feedback });
                    }
                    send({ action: request.action, request_id: request.request_id, ok, feedback, result, feedback_in_logs_only });
                };
                actions.push({ request, raw, reply });
                events.emit('action');
            });
            await gate;
            previewSocket = socket;
            sendSnapshot = send;
            send({ users_roster: usersRoster, ...(schematic === null ? {} : { schematic }) });
            connected.resolve();
        });
        return {
            async broadcast(fields) {
                await connected.promise;
                sendSnapshot(fields);
            },
            async disconnect() {
                await connected.promise;
                await previewSocket.close({ code: 1011, reason: 'Fixture connection closed' });
            },
            async disconnectBoth(nextSessionId = sessionId) {
                await connected.promise;
                sessionId = nextSessionId;
                await Promise.all([previewSocket, identitySocket].map(socket =>
                    socket.close({ code: 1011, reason: 'Fixture connection closed' })));
            },
            async rosterAction(index) {
                const signal = AbortSignal.timeout(timeout);
                while (index >= rosterActions.length) await once(events, 'roster-action', { signal });
                return rosterActions[index];
            },
            async rosterFrame(subscription, fields) {
                await connected.promise;
                previewSocket.send(JSON.stringify({ type: 'users_roster', subscription, ...fields }));
            },
            async roster(subscription, rows, { sequence = '1', base_sequence, removed = [],
                total = rows.length, chunkSize = 250, host_time = '1795000000000',
                accounting_time = '1794000000000', attempt = String(nextAttempt++) } = {}) {
                nextAttempt = Math.max(nextAttempt, Number(attempt) + 1);
                await this.rosterFrame(subscription, { event: 'begin', attempt, sequence, base_sequence, total });
                let index = 0;
                for (let offset = 0; offset < rows.length; offset += chunkSize) {
                    await this.rosterFrame(subscription, { event: 'chunk', attempt, index: index++,
                        rows: rows.slice(offset, offset + chunkSize), removed: [] });
                }
                for (let offset = 0; offset < removed.length; offset += chunkSize) {
                    await this.rosterFrame(subscription, { event: 'chunk', attempt, index: index++,
                        rows: [], removed: removed.slice(offset, offset + chunkSize) });
                }
                await this.rosterFrame(subscription, { event: 'end', attempt, chunks: index, host_time, accounting_time });
            },
            actions,
            rosterActions,
            async action(index) {
                const signal = AbortSignal.timeout(timeout);
                while (index >= actions.length) await once(events, 'action', { signal });
                return actions[index];
            },
        };
    }

    return {
        get url() { return url; },
        get consoleUrl() { return consoleUrl; },
        get browser() { return browser; },
        get waterwallSchematic() { return waterwallSchematic; },
        mockPreview,
    };
}

function fixture(coreState = 'running') {
    return {
        logs: [{ source: 'panel', ts: '09:41:02', level: 'info', message: 'Preview fixture is ready.' }],
        server: {
            name: 'wave-fixture', platform: 'Linux x86_64', version: '0.0.1 beta',
            checking_updates: false,
            cpu_cores: 8, cpu_percent: 25, memory_used_gib: 4, memory_total_gib: 16,
            uptime_minutes: 1599, upload_rate: 2, download_rate: 6, upload_total: 12.5, download_total: 55.8,
            cpu_history: [15, 35, 25], memory_history: [1, 6, 4], upload_history: [1, 2, 2],
            download_history: [8, 4, 6],
            panel: { checking_updates: false, update_check_generation: 0, update_available: false },
            core: {
                state: coreState, version: '1.6.0', available_versions: ['1.6.0'],
                checking_updates: false, latest_version: '1.6.0', update_available: false,
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

function tlsFixture() {
    return {
        enabled: true,
        domain: 'wave.example.invalid',
        certificate_chain: '/etc/wave-fixture/fullchain.pem',
        private_key: '/etc/wave-fixture/privkey.pem',
        supported: true,
        restart_required: false,
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

function assertRequest(request, expected) {
    const { request_id: id, ...fields } = request;
    assert.ok(Number.isSafeInteger(id) && id > 0);
    assert.deepEqual(fields, expected);
    return id;
}

// ImGui draws its controls on one canvas. Scan a one-pixel strip to locate their edges.
async function colorRuns(page, clip, [r, g, b], minimum, origin) {
    const screenshot = await page.screenshot({ clip });
    return page.evaluate(async ({ encoded, r, g, b, minimum, origin }) => {
        const bytes = Uint8Array.from(atob(encoded), character => character.charCodeAt(0));
        const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
        const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
        const context = canvas.getContext('2d');
        context.drawImage(bitmap, 0, 0);
        const { data } = context.getImageData(0, 0, canvas.width, canvas.height);
        const runs = [];
        let start = null;
        for (let i = 0; i <= data.length / 4; i++) {
            const matches = data[i * 4] === r && data[i * 4 + 1] === g && data[i * 4 + 2] === b;
            if (matches && start === null) start = i;
            if (!matches && start !== null) {
                if (i - start >= minimum) runs.push([origin + start, origin + i]);
                start = null;
            }
        }
        return runs;
    }, { encoded: screenshot.toString('base64'), r, g, b, minimum, origin });
}

function verticalRuns(page, x, top, bottom, color, minimum = 1) {
    return colorRuns(page, { x, y: top, width: 1, height: bottom - top }, color, minimum, top);
}

function horizontalRuns(page, y, left, right, color, minimum = 1) {
    return colorRuns(page, { x: left, y, width: right - left, height: 1 }, color, minimum, left);
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

async function graphToolbar(page, x = 600, top = 100, bottom = 180) {
    const [row] = await verticalRuns(page, x, top, bottom, [26, 23, 28], 18);
    assert.ok(row, 'Expected the Nodes toolbar.');
    return row;
}

async function graphMenuItem(page, menu, row) {
    const x = { nodes: 269, config: 325, view: 375 }[menu];
    const [top, bottom] = await graphToolbar(page);
    await click(page, x, (top + bottom) / 2);
    await click(page, x, bottom + 18 + row * 23);
}

async function graphCanvasBounds(page, x = 600, top = 100, bottom = 320) {
    const deadline = Date.now() + timeout;
    do {
        const bounds = await page.evaluate(({ x, top, bottom }) => {
            const canvas = document.getElementById('canvas').getBoundingClientRect();
            // A zero-step wheel zoom checks the hit area without moving the camera.
            const overGraph = (x, y) => Module['_wave_zoom_node_graph'](x - canvas.left, y - canvas.top, 0);
            let graphTop = top;
            while (graphTop < bottom && !overGraph(x, graphTop)) graphTop++;
            if (graphTop === bottom) return null;

            let left = x;
            let right = x;
            let graphBottom = graphTop;
            while (overGraph(left - 1, graphTop)) left--;
            while (overGraph(right, graphTop)) right++;
            while (graphBottom < innerHeight && overGraph(x, graphBottom)) graphBottom++;
            return { left, top: graphTop, right, bottom: graphBottom };
        }, { x, top, bottom });
        if (bounds) return bounds;
        await settleCanvas(page);
    } while (Date.now() < deadline);
    assert.fail('Expected the graph canvas.');
}

async function graphCanvasTop(page, x, top, bottom) {
    return (await graphCanvasBounds(page, x, top, bottom)).top;
}

function waitForServerStatus(page) {
    return status(page, /Wave Server Status.*CPU 25\.0%/);
}

async function selectConfigTab(page, name) {
    await page.getByRole('tab', { name, exact: true }).focus();
    await page.keyboard.press('Enter');
    await page.getByRole('tab', { name, exact: true, selected: true }).waitFor({ timeout });
}

module.exports = {
    setupWorkspace,
    desktop,
    coreDesktop,
    rosterColumnX,
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
    graphToolbar,
    graphMenuItem,
    graphCanvasBounds,
    graphCanvasTop,
    waitForServerStatus,
    selectConfigTab,
};
