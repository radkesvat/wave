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
        users: [
            { id: 1, name: 'Avery Brooks', enabled: true },
            { id: 2, name: 'Bella Chen', enabled: true },
        ],
        users_live: true,
        logs: [{ ts: '09:41:02', level: 'info', message: 'Preview fixture is ready.' }],
        server: {
            name: 'wave-fixture', platform: 'Linux x86_64', version: '1.6.0',
            cpu_cores: 8, cpu_percent: 25, memory_used_gib: 4, memory_total_gib: 16,
            uptime_minutes: 1599, upload_rate: 2, download_rate: 6, upload_total: 12.5, download_total: 55.8,
            users_total: 2, users_enabled: 2, users_online: 1,
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
        topology: { nodes: [], links: [], routes: [] },
        bot: {},
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
            const reply = ({ ok = true, feedback = '' } = {}) =>
                send({ action: request.action, request_id: request.request_id, ok, feedback });
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
    await page.keyboard.insertText(value);
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
    await settleCanvas(page);
    const { width, height } = page.viewportSize();
    const outline = await verticalRuns(page, Math.floor(width / 2), 114, height, [138, 132, 149]);
    assert.ok(outline.length >= 2, 'Expected a dialog outline.');
    return { top: outline[0][0], bottom: outline.at(-1)[1] };
}

async function confirmDialog(page, activate, width = 380) {
    const { bottom } = await dialogFrame(page);
    const viewport = page.viewportSize().width;
    const right = (viewport + Math.min(width, viewport - 24)) / 2 - 12;
    await activate(right - 42, bottom - 25);
}

async function lifecycleButton(page, row) {
    const x = 510;
    const [title] = await verticalRuns(page, x, 250, 650, [43, 39, 48], 14);
    assert.ok(title, 'Expected the Panel Controls title.');
    await click(page, x, title[1] + 47 + 37 * row);
}

test('opens the authenticated workspace and updates host metrics during keyboard navigation', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    const firstSnapshot = Promise.withResolvers();
    const preview = await mockPreview(page, data, firstSnapshot.promise);
    try {
        assert.equal((await page.goto(url)).status(), 200);
        await status(page, /Wave Users.*Loading sample data/);
        firstSnapshot.resolve();
        await status(page, /Wave Users.*2 users/);

        await openView(page, 'Server Status');
        await status(page, /CPU 25\.0%\. Memory 4\.0 \/ 16\.0 GiB.*Upload 2\.0 MiB\/s\. Download 6\.0 MiB\/s/);
        Object.assign(data.server, {
            cpu_percent: null, cpu_history: [25, null],
            upload_rate: 0.5 / 1024, download_rate: 1.5 / 1024,
            upload_history: [0.5 / 1024], download_history: [1.5 / 1024],
        });
        await preview.broadcast();
        await status(page, /CPU Unavailable.*Upload 0\.5 KiB\/s\. Download 1\.5 KiB\/s/);
        await openView(page, 'About');
    } finally {
        firstSnapshot.resolve();
        await page.close();
    }
});

test('installs, retries, starts, and confirms stopping WaterWall from Server Status', async () => {
    const page = await browser.newPage({ viewport: coreDesktop });
    const data = fixture('missing');
    const core = data.server.core;
    const preview = await mockPreview(page, data);
    const press = row => lifecycleButton(page, row);
    try {
        await page.goto(url);
        await openView(page, 'Server Status');
        await status(page, /WaterWall core missing\. Panel version 1\.6\.0/);

        await press(0);
        const install = await preview.action(0);
        const firstId = assertRequest(install.request, { action: 'install_core' });
        core.state = 'installing';
        install.reply();
        await status(page, /WaterWall core installing/);
        await press(0);
        assert.equal(preview.actions.length, 1, 'Installation cannot restart while in progress.');
        core.state = 'missing';
        core.error = 'The WaterWall archive failed its SHA-256 check.';
        await preview.broadcast();
        await status(page, /WaterWall core missing\. Core error\. The WaterWall archive failed/);

        await press(0);
        const retry = await preview.action(1);
        assert.notEqual(assertRequest(retry.request, { action: 'install_core' }), firstId);
        delete core.error;
        core.state = 'stopped';
        retry.reply();
        await status(page, /WaterWall core stopped/);
        await press(0);
        const start = await preview.action(2);
        assertRequest(start.request, { action: 'start_core' });
        core.state = 'starting';
        start.reply();
        await status(page, /WaterWall core starting/);
        core.state = 'running';
        await preview.broadcast();
        await status(page, /WaterWall core running/);

        await press(1);
        await status(page, /Confirm stop/);
        assert.equal(preview.actions.length, 3, 'Opening the confirmation leaves WaterWall running.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const stop = await preview.action(3);
        assertRequest(stop.request, { action: 'stop_core' });
        core.state = 'stopped';
        stop.reply();
        await status(page, /WaterWall core stopped/);
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);
    } finally {
        await page.close();
    }
});

test('saves Panel TLS and Core JSON settings without mixing their drafts', async () => {
    const page = await browser.newPage({ viewport: desktop });
    const data = fixture();
    const preview = await mockPreview(page, data);
    const editCore = text => replaceText(page, 320, 177, text);
    const saveCore = () => click(page, 267, 389);
    try {
        await page.goto(url);
        await openView(page, 'Settings');
        await status(page, /Panel tab\. Administrator settings/);
        const tls = { ...data.settings, domain: 'updated.example.invalid' };
        await replaceText(page, 325, 138, tls.domain);
        await click(page, 281, 82);
        await status(page, /Core tab\. WaterWall core settings JSON/);

        await editCore('{"configs":[]}');
        await saveCore();
        await status(page, /Remove the configs key before saving/);
        assert.equal(preview.actions.length, 0);
        await click(page, 386, 389);
        await saveCore();
        assert.equal(preview.actions.length, 0, 'Revert clears the rejected draft.');

        const core = { misc: { workers: 4, 'ram-profile': 'client' } };
        await editCore(JSON.stringify(core, null, 2));
        await click(page, 223, 82);
        await status(page, /Panel tab\. Administrator settings/);
        await click(page, 245, 283);
        await confirmDialog(page, (x, y) => click(page, x, y), 370);
        const panelSave = await preview.action(0);
        assertRequest(panelSave.request, { action: 'apply_settings', settings: tls });
        data.settings = tls;
        panelSave.reply({ feedback: 'Applied sample TLS settings.' });
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        await click(page, 281, 82);
        await status(page, /Core tab/);
        await saveCore();
        const coreSave = await preview.action(1);
        assertRequest(coreSave.request, { action: 'apply_core_settings', settings: core });
        data.core_settings = core;
        coreSave.reply({ feedback: 'Saved core settings.' });
        await status(page, /Core tab.*Saved core settings/);
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
        await waitForUsers(page);
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
        await openView(page, 'Users');
        await page.locator('#wave-console').waitFor({ state: 'hidden', timeout });
        if (!closed) await once(messages, 'close', { signal: AbortSignal.timeout(timeout) });
        assert.equal(preview.actions.length, 0, 'Terminal input bypasses the preview socket.');
    } finally {
        await page.close();
    }
});

function waitForUsers(page) {
    return status(page, /Wave Users.*2 users/);
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
        await waitForUsers(page);
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

test('creates and enables a config from the top-bar menu on touch', async () => {
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
    try {
        await page.goto(url);
        await waitForUsers(page);
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
        created.reply();
        await page.waitForFunction(() => !document.getElementById('panel-controls').inert);

        await menuItem(191);
        await status(page, /Manage configs.*Lab disabled/);
        await tap(45, (await dialogFrame(page)).top + 158);
        const enabled = await preview.action(1);
        assertRequest(enabled.request, { action: 'set_core_config_enabled', id: 2, enabled: true });
        config.enabled = true;
        enabled.reply();
        await status(page, /Manage configs.*Lab enabled/);
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
        const x = 510;
        const buttons = await verticalRuns(page, x, 400, 820, [69, 64, 79], 20);
        assert.ok(buttons.length >= 2, 'Expected Backup and Restore controls.');
        const [downloadY, restoreY] = buttons.slice(-2).map(([top, bottom]) => Math.floor((top + bottom) / 2));
        const downloading = page.waitForEvent('download', { timeout });
        await click(page, x, downloadY);
        const download = await downloading;
        const buffer = await readFile(await download.path());
        const backup = JSON.parse(buffer);
        assert.deepEqual(backup.settings, data.settings);
        assert.deepEqual(backup.configs, [{ name: 'Standby', enabled: false }]);

        await click(page, x, restoreY);
        await status(page, /Restore backup/);
        const choosing = page.waitForEvent('filechooser', { timeout });
        await click(page, 560, (await dialogFrame(page)).top + 80);
        await (await choosing).setFiles({ name: download.suggestedFilename(), mimeType: 'application/json', buffer });
        await status(page, /Wave preview backup selected/);
        assert.equal(preview.actions.length, 0, 'Selecting a file waits for confirmation.');
        await confirmDialog(page, (x, y) => click(page, x, y));
        const restored = await preview.action(0);
        assertRequest(restored.request, { action: 'restore_preview_backup', backup });
        restored.reply({ feedback: 'Restored backup.' });
        await status(page, /Restored backup/);
    } finally {
        await page.close();
    }
});
