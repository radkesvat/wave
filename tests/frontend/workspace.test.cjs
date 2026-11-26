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
    await page.routeWebSocket(previewUrl, async socket => {
        const send = fields => socket.send(JSON.stringify({ type: 'snapshot', data, ...fields }));
        socket.onMessage(raw => {
            const request = JSON.parse(raw);
            const reply = ({ ok = true, feedback = '', result, feedback_in_logs_only } = {}) => {
                if (feedback_in_logs_only) {
                    data.logs.push({ source: 'panel', ts: '09:41:03', level: 'success', message: feedback });
                }
                send({ action: request.action, request_id: request.request_id, ok, feedback, result, feedback_in_logs_only });
            };
            actions.push({ request, reply });
            events.emit('action');
        });
        await gate;
        send();
        connected.resolve(send);
    });
    return {
        broadcast: async () => (await connected.promise)(),
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

async function dialogFrame(page) {
    const { width, height } = page.viewportSize();
    const deadline = Date.now() + timeout;
    do {
        await settleCanvas(page);
        // Chromium can briefly tint the canvas after a touch, changing the border pixels.
        const outline = await verticalRuns(page, Math.floor(width / 2), 114, height, [138, 132, 149]);
        if (outline.length >= 2) return { top: outline[0][0], bottom: outline.at(-1)[1] };
    } while (Date.now() < deadline);
    assert.fail('Expected a dialog outline.');
}

async function confirmDialog(page, activate, width = 380) {
    const { bottom } = await dialogFrame(page);
    const viewport = page.viewportSize().width;
    const right = (viewport + Math.min(width, viewport - 24)) / 2 - 12;
    await activate(right - 42, bottom - 25);
}

async function graphMenuItem(page, menu, row) {
    const x = { nodes: 230, config: 286, view: 336 }[menu];
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

async function openConfigJsonEditor(page) {
    await graphMenuItem(page, 'config', 1);
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
    try {
        assert.equal((await page.goto(url)).status(), 200);
        await status(page, /Wave Server Status.*Loading panel data/);
        firstSnapshot.resolve();
        await waitForServerStatus(page);

        await openView(page, 'Server Status');
        await status(page, /CPU 25\.0%\. Memory 4\.0 \/ 16\.0 GiB.*Upload 2\.0 MiB\/s\. Download 6\.0 MiB\/s/);
        const dividers = await verticalRuns(page, 152, 91, desktop.height - 18, [61, 58, 67]);
        assert.deepEqual(dividers.map(([top, bottom]) => bottom - top), [2, 1, 1, 1]);
        for (const x of [11, 176]) {
            assert.deepEqual(await verticalRuns(page, x, 91, desktop.height - 18, [61, 58, 67]), dividers);
        }
        assert.deepEqual(await verticalRuns(page, 11, 68, 86, [43, 39, 48]), [[68, 86]]);
        assert.deepEqual(await verticalRuns(page, 176, 60, 96, [243, 129, 74]), [[63, 93]]);
        await page.mouse.move(12, 115);
        await settleCanvas(page);
        for (const x of [11, 176]) {
            assert.deepEqual(await verticalRuns(page, x, 110, 128, [55, 49, 59]), [[110, 128]]);
        }
        Object.assign(data.server, {
            cpu_percent: null, cpu_history: [25, null],
            upload_rate: 0.5 / 1024, download_rate: 1.5 / 1024,
            upload_history: [0.5 / 1024], download_history: [1.5 / 1024],
        });
        await preview.broadcast();
        await status(page, /CPU Unavailable.*Upload 0\.5 KiB\/s\. Download 1\.5 KiB\/s/);
        await click(page, 176, desktop.height - 35);
        await status(page, /^Wave About\./);
        const shortHeight = 500;
        await page.setViewportSize({ ...desktop, height: shortHeight });
        await settleCanvas(page);
        await click(page, 80, shortHeight - 103);
        await status(page, /^Wave Console\./);
        await click(page, 80, shortHeight - 69);
        await status(page, /^Wave Settings\./);
        await click(page, 80, shortHeight - 35);
        await status(page, /^Wave About\./);
    } finally {
        firstSnapshot.resolve();
        await page.close();
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
    try {
        await page.goto(url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core missing\. Panel version 1\.6\.0/);
        const titleBeforeError = await controlsTitle();

        await press('install');
        const install = await preview.action(0);
        const firstId = assertRequest(install.request, { action: 'install_core' });
        core.state = 'installing';
        install.reply();
        await status(page, /WaterWall core installing/);
        await press('install');
        assert.equal(preview.actions.length, 1, 'Installation cannot restart while in progress.');
        core.state = 'missing';
        core.error = 'The WaterWall archive failed its SHA-256 check.' + '\nCheck the installation logs.'.repeat(24);
        await preview.broadcast();
        await status(page, /WaterWall core missing\. Core error\. The WaterWall archive failed/);
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
        core.state = 'starting';
        start.reply();
        await status(page, /WaterWall core starting/);
        core.state = 'running';
        await preview.broadcast();
        await status(page, /WaterWall core running/);

        await press('restart');
        await status(page, /Confirm restart\./);
        assert.equal(preview.actions.length, 3, 'Opening the confirmation leaves WaterWall running.');
        await click(page, 670, (await dialogFrame(page)).bottom - 25);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(preview.actions.length, 3, 'Cancelling does not restart WaterWall.');
        await press('restart');
        await status(page, /Confirm restart\./);
        await confirmDialog(page, (x, y) => click(page, x, y));
        const restart = await preview.action(3);
        assertRequest(restart.request, { action: 'restart_core' });
        core.state = 'stopping';
        restart.reply();
        await status(page, /WaterWall core stopping/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        await press('restart');
        assert.equal(await page.locator('#panel-controls').evaluate(controls => controls.inert), false);
        core.state = 'starting';
        await preview.broadcast();
        await status(page, /WaterWall core starting/);
        core.state = 'running';
        await preview.broadcast();
        await status(page, /WaterWall core running/);

        await press('stop');
        await status(page, /Confirm stop/);
        assert.equal(preview.actions.length, 4, 'Opening the confirmation leaves WaterWall running.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const stop = await preview.action(4);
        assertRequest(stop.request, { action: 'stop_core' });
        core.state = 'stopped';
        stop.reply();
        await status(page, /WaterWall core stopped/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
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
    try {
        await page.goto(url);
        await openView(page, 'Settings');
        await status(page, /Panel tab\. Administrator settings/);
        const tls = { ...data.settings, domain: 'updated.example.invalid' };
        await replaceText(page, 325, 138, tls.domain);
        await click(page, 281, 82);
        await status(page, /Core tab\. WaterWall core settings\. Runtime\. Version 1\.6\.0/);
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
        await click(page, 245, 283);
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

test('uses the sidebar and creates, toggles, and deletes a config on touch', async () => {
    const page = await browser.newPage({ viewport: portrait, hasTouch: true, isMobile: true });
    const data = fixture();
    const preview = await mockPreview(page, data);
    const tap = async (x, y) => {
        await page.touchscreen.tap(x, y);
        await settleCanvas(page);
    };
    const menuItem = async y => {
        await tap(130, 24);
        await page.locator('#menu[aria-expanded="true"]').waitFor({ timeout });
        await tap(164, y);
    };
    const configHeaderY = 279;
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
        await tap(60, 180);
        await status(page, /^Wave Console\./);
        await tap(185, 180);
        await status(page, /^Wave Settings\./);
        const rightEdge = Math.floor(width) - 12;
        await tap(rightEdge, 180);
        await status(page, /^Wave About\./);
        assert.deepEqual(await verticalRuns(page, rightEdge, 171, 187, [243, 129, 74]), [[171, 187]]);
        await menuItem(167);
        await status(page, /Create new config/);
        await tap(80, (await dialogFrame(page)).top + 69);
        await page.locator('#wave-text-input').waitFor({ state: 'visible', timeout });
        await page.keyboard.insertText('Lab');
        await settleCanvas(page);
        await confirmDialog(page, tap);
        const created = await preview.action(0);
        assertRequest(created.request, { action: 'create_core_config', name: 'Lab' });
        const config = { id: 2, name: 'Lab', enabled: false, active: false };
        data.server.core.configs.push(config);
        created.reply({ feedback: 'Created config Lab (disabled).', feedback_in_logs_only: true });
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        const labLink = page.getByRole('link', { name: 'Lab', exact: true });
        await labLink.waitFor({ timeout });
        await tap(80, 180);
        const loaded = await preview.action(1);
        assertRequest(loaded.request, { action: 'get_core_config', id: 2 });
        loaded.reply({ result: { config: { name: 'Lab', nodes: [] }, revision: 'loaded', editable: true } });
        await status(page, /^Wave Config Lab\..*0 nodes\./);
        const canvasTop = () => graphCanvasTop(page, 180, 376, 475);
        const initialCanvasTop = await canvasTop();

        const toggleStates = [true, false, true];
        for (const [index, enabled] of toggleStates.entries()) {
            await toggleConfig();
            const changed = await preview.action(2 + index);
            assertRequest(changed.request, { action: 'set_core_config_enabled', id: 2, enabled });
            config.enabled = enabled;
            const feedback = `${enabled ? 'Enabled' : 'Disabled'} config Lab.`;
            changed.reply({ feedback, feedback_in_logs_only: true });
            await status(page, `Latest log: ${feedback}`);
            const description = await page.locator('#canvas').getAttribute('aria-label');
            assert.equal(description.split(feedback).length - 1, 1, 'The successful toggle displays only in Logs.');
            assert.equal(await canvasTop(), initialCanvasTop, 'The graph stays in place after a successful toggle.');
        }

        await toggleConfig();
        const rejected = await preview.action(5);
        assertRequest(rejected.request, { action: 'set_core_config_enabled', id: 2, enabled: false });
        rejected.reply({ ok: false, feedback: 'Stop WaterWall before changing this config.' });
        await status(page, /Stop WaterWall before changing this config\. \d+ log entries\./);

        await deleteConfig();
        await status(page, /Confirm deletion of Lab and its config\.json file\./);
        await tap(220, (await dialogFrame(page)).bottom - 25);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
        assert.equal(await labLink.getAttribute('aria-current'), 'page');
        assert.equal(preview.actions.length, 6, 'Cancelling leaves the config in place.');

        await deleteConfig();
        await status(page, /Confirm deletion of Lab and its config\.json file\./);
        await confirmDialog(page, tap);
        const removed = await preview.action(6);
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
        const choosing = page.waitForEvent('filechooser', { timeout });
        await click(page, 560, (await dialogFrame(page)).top + 80);
        await (await choosing).setFiles({ name: download.suggestedFilename(), mimeType: 'application/json', buffer });
        await status(page, /Wave preview backup selected/);
        assert.equal(preview.actions.length, 0, 'Selecting a file waits for confirmation.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const restored = await preview.action(0);
        assertRequest(restored.request, { action: 'restore_preview_backup', backup });
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

        await menuItem('nodes', 0);
        await status(page, /Add node\./);
        const { top: addTop } = await dialogFrame(page);
        await replaceText(page, 640, addTop + 72, 'outlet');
        await click(page, 893, addTop + 128);
        await click(page, 420, addTop + 275);
        await confirmDialog(page, (x, y) => click(page, x, y), 560);
        await status(page, /2 nodes. Selected outlet. Type BlackHole/);

        await page.mouse.dblclick(300, (await graphTop()) + 143, { delay: 40 });
        await settleCanvas(page);
        await status(page, /Edit node\./);
        const { top: editTop } = await dialogFrame(page);
        await replaceText(page, 640, editTop + 72, 'discard');
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
        await click(page, 300, (await graphTop()) + 143);
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
        assert.deepEqual(draft, {
            ...config,
            nodes: [
                { ...config.nodes[0], next: 'discard', 'panel-settings': inlet['panel-settings'] },
                { name: 'discard', type: 'BlackHole', settings: {} },
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

        await menuItem('config', 0);
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

        await menuItem('config', 2);
        await status(page, /Saved configuration JSON\./);
        await page.evaluate(() => navigator.clipboard.writeText(''));
        await confirmDialog(page, activate, 740);
        await page.waitForFunction(async () => (await navigator.clipboard.readText()).length > 0);
        assert.deepEqual(JSON.parse(await page.evaluate(() => navigator.clipboard.readText())), document);
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

        await pasteConfigJson(page, `{
            "name": "Source fixture",
            "author": "Source metadata",
            "commits": [{"message": "Fixture metadata"}],
            "nodes": [
                // The variable tokens are outside JSON strings.
                {"name":"dummy-in","type":"TcpListener","settings":{"address":$fixture_address$,"port":$fixture_port$},"next":$fixture_target$},
                {"name":"dummy-out","type":"BlackHole","settings":{"marker":"literal // $fixture_port$"}}
            ],
            "variables": {"fixture_address":"192.0.2.10","fixture_port":31870,"fixture_target":"dummy-out"}
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
    const page = await browser.newPage({ viewport: coreDesktop });
    const data = fixture('stopped');
    const config = { name: 'Offline fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [
        { id: 11, name: config.name, enabled: true, active: false, auth_nodes: ['staff'] },
    ];
    const user = {
        id: '9007199254740993', name: 'Fixture account', enabled: true, gid: '4', 'wireguard-allowed-ips': '',
        time: { 'created-at-ms': '1920000000000', 'expire-at-ms': '1924992000000' },
        limit: { traffic: { up: '500', down: '1000' } }, stats: { traffic: { up: '20', down: '30' } },
    };
    const preview = await mockPreview(page, data);
    const selector = { config_id: 11, node: 'staff', page: 0 };
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
        await click(page, 306, 166);
        await status(page, /Edit user\. Access section\./);
        await replaceText(page, 300, 279, '18446744073709551616');
        await status(page, /Group ID requires a whole number/);
        await click(page, 237, 747);
        assert.equal(preview.actions.length, 2);
        const gid = '18446744073709551615';
        const allowedIps = '192.0.2.15/32';
        await replaceText(page, 300, 279, gid);
        await replaceText(page, 820, 279, allowedIps);
        await replaceText(page, 820, 350, '');

        await click(page, 370, 166);
        await status(page, /Edit user\. Limits section\./);
        await replaceText(page, 300, 298, '0');
        await replaceText(page, 820, 298, '');
        await click(page, 433, 166);
        await status(page, /Edit user\. Activity section\./);
        await click(page, 237, 747);
        const save = await preview.action(2);
        assertRequest(save.request, {
            action: 'edit_config_user', ...selector, user_id: user.id,
            user: { gid, 'wireguard-allowed-ips': allowedIps, time: { 'expire-at-ms': null },
                limit: { traffic: { up: '0', down: null } } },
        });
        const updated = { ...user, gid, 'wireguard-allowed-ips': allowedIps,
            time: { 'created-at-ms': user.time['created-at-ms'] }, limit: { traffic: { up: '0' } } };
        save.reply({ feedback: 'Saved fixture user.', result: listing(updated) });
        await status(page, /Saved fixture user\./);

        await click(page, 235, 137);
        await status(page, /Add user\. Account section\./);
        const password = 'fixture-new-credential';
        const id = '9007199254740995';
        await replaceText(page, 300, 317, password);
        await replaceText(page, 820, 317, id);
        await click(page, 242, 747);
        const create = await preview.action(3);
        assertRequest(create.request, {
            action: 'create_config_user', ...selector, user: { id, password, enabled: true },
        });
        create.reply({ result: { ...listing(updated), users: [updated, { id, enabled: true }], total: 2 } });
        await status(page, /staff tab\. AuthenticationServer users\. 2 users\./);
    } finally {
        await page.close();
    }
});

test('lists and refreshes AuthenticationServer live users while offline editing is locked', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture('running');
    const config = { name: 'Live fixture', nodes: [{ name: 'staff', type: 'AuthenticationServer' }] };
    data.server.core.configs = [
        { id: 11, name: config.name, enabled: true, active: true, auth_nodes: ['staff'], live_users: true },
    ];
    const preview = await mockPreview(page, data);
    const listRequest = { action: 'get_live_config_users', config_id: 11, node: 'staff' };
    const cursor = '12'.repeat(32);
    const user = { id: '9007199254740993', name: 'Fixture account', enabled: true, usage: { u: '2048', d: '4096' } };
    try {
        await page.goto(url);
        await page.getByRole('link', { name: config.name, exact: true }).focus();
        await page.keyboard.press('Enter');
        const opened = await preview.action(0);
        assertRequest(opened.request, { action: 'get_core_config', id: 11 });
        opened.reply({ result: { config, revision: 'active', editable: false } });

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
