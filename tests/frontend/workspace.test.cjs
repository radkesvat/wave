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

async function canvasClick(page, x, y) {
    await page.mouse.click(x, y, { delay: 40 });
    await settleCanvas(page);
}

async function replaceText(page, x, y, value) {
    await canvasClick(page, x, y);
    await page.locator('#wave-text-input').waitFor({ state: 'visible' });
    await page.keyboard.press('Control+a');
    await settleCanvas(page);
    await page.keyboard.insertText(value);
    await settleCanvas(page);
}

function assertRequest(request, expected) {
    const { request_id: id, ...fields } = request;
    assert.ok(Number.isSafeInteger(id) && id > 0, `Expected a positive request ID, got ${id}`);
    assert.deepEqual(fields, expected);
    return id;
}

async function openSetup(page) {
    await canvasClick(page, 132, 23);
    await page.waitForFunction(() => document.getElementById('menu').getAttribute('aria-expanded') === 'true');
    await canvasClick(page, 170, 160);
    await page.getByRole('img', { name: /Wave first-run setup. Step 1 of 3/ }).waitFor();
    await canvasClick(page, 449, 244);
    await page.getByRole('img', { name: /Step 2 of 3/ }).waitFor();
    await canvasClick(page, 495, 294);
    await page.getByRole('img', { name: /Step 3 of 3/ }).waitFor();
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

        const menu = page.getByRole('button', { name: 'Menu' });
        const tools = page.getByRole('button', { name: 'Tools' });
        for (const [button, other] of [[menu, tools], [tools, menu]]) {
            await button.focus();
            await page.keyboard.press('Enter');
            await page.locator(`#${await button.getAttribute('id')}[aria-expanded="true"]`).waitFor();
            assert.equal(await other.getAttribute('aria-expanded'), 'false');
            await canvasClick(page, 400, 30);
            assert.equal(await button.getAttribute('aria-expanded'), 'false');
        }

        const firstNavRow = { x: 11, y: 61, width: 162, height: 34 };
        await tools.focus();
        await settleCanvas(page);
        const unfocused = await page.screenshot({ clip: firstNavRow });
        for (const [index, name] of pages.entries()) {
            await page.keyboard.press('Tab');
            assert.equal(await page.evaluate(() => document.activeElement.textContent), name);
            if (index === 0) {
                await settleCanvas(page);
                assert.notDeepEqual(await page.screenshot({ clip: firstNavRow }), unfocused);
            }
            await page.keyboard.press('Enter');
            await assertPage(name);
        }
        await page.getByRole('img', { name: /CPU utilization 63 percent.*Memory 4\.5 of 16\.0 GiB.*Latest 9\.6 MiB/ }).waitFor();

        for (const [index, name] of pages.entries()) {
            await canvasClick(page, 65, 76 + 34 * index);
            await assertPage(name);
        }

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
    await mockPreview(page, { handle(data, action, reply) {
        actions.push(action);
        const changed = data.users.find(row => row.id === action.id);
        assert.ok(changed && action.action === 'set_user_enabled');
        changed.enabled = action.enabled;
        const message = `Disabled ${changed.name} in the preview.`;
        data.logs.push({ ts: '09:41:13', level: 'success', message });
        reply({ feedback: message });
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
        await replaceText(page, 540, 104, 'Al');
        await canvasClick(page, 747, 167);
        await canvasClick(page, 767, 184);
        await replaceText(page, 495, 212, 'Albright local');

        data.users = [
            { ...user, id: 5, name: 'Alpine new' },
            { ...user, id: 2, name: 'Albright remote', notes: 'Other notes', remaining_minutes: 120, configuration },
        ];
        await preview.broadcast();
        await description(/Wave Users.*2 users/).waitFor();
        await canvasClick(page, 481, 654);
        await description(/Applied the Users draft/).waitFor();
        assertRequest((await preview.action(0)).request, {
            action: 'edit_user', id: 2, name: 'Albright local',
        });

        await canvasClick(page, 747, 190);
        await canvasClick(page, 800, 253);
        await canvasClick(page, 490, 554);
        await page.waitForFunction(async text => (await navigator.clipboard.readText()) === text, configuration);
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
        await canvasClick(page, 119, 421);
        await canvasClick(page, 224, 485);
        assert.deepEqual((await preview.action(1)).request, { action: 'set_user_enabled', id: 8, enabled: true });
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
    const preview = await mockPreview(page, { handle(_data, _action, reply) {
        reply({ feedback: 'Applied demo setup.' });
    } });

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openSetup(page);
        await canvasClick(page, 511, 327);
        await page.getByRole('img', { name: /Enter a demo username and password/ }).waitFor();
        assert.equal(preview.actions.length, 0);
        await canvasClick(page, 458, 294);
        await canvasClick(page, 511, 327);
        await page.getByRole('img', { name: /Applied demo setup/ }).waitFor();
        assert.equal(preview.actions.length, 1);
        assertRequest(preview.actions[0].request, {
            action: 'setup', name: 'Wave Preview', domain: 'wave.example', username: 'demo', password: 'wave-demo',
        });
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
            await openSetup(page);
            await canvasClick(page, 458, 294);
            await canvasClick(page, 511, 327);
            const { reply } = await preview.action(0);

            // Hold rendering so both socket events can precede the form's next frame.
            await page.clock.pauseAt(60_000);
            if (acknowledged) reply({ feedback: 'Applied demo setup.' });
            await preview.close();
            await page.clock.resume();
            await settleCanvas(page);

            const expected = acknowledged ? 'Applied demo setup.'
                : 'The preview connection closed. Try again after reconnecting.';
            const description = await page.getByRole('img').getAttribute('aria-label');
            assert.equal(description, `Wave first-run setup. Step 3 of 3. ${expected}`);
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
    const pending = /Wave first-run setup\. Step 3 of 3\. Applying demo setup/;
    const description = name => page.getByRole('img', { name });
    async function submit(index) {
        await canvasClick(page, 511, 327);
        await description(pending).waitFor();
        return preview.action(index);
    }

    try {
        await page.goto(url);
        await description(/Wave Users.*2 users/).waitFor();
        await openSetup(page);
        await canvasClick(page, 458, 294);
        const first = await submit(0);

        await openSetup(page);
        await canvasClick(page, 458, 294);
        const second = await submit(1);
        first.reply({ ok: false, feedback: 'The first setup was rejected.' });
        await page.waitForFunction(id => window.lastPreviewRequestId === id, first.request.request_id);
        await settleCanvas(page);
        assert.equal(await description(pending).count(), 1);
        second.reply({ feedback: 'The reopened setup completed.' });
        await description(/The reopened setup completed/).waitFor();

        await openSetup(page);
        await canvasClick(page, 458, 294);
        const third = await submit(2);
        third.reply({ ok: false, feedback: 'The active setup was rejected.' });
        await description(/The active setup was rejected/).waitFor();
        const retry = await submit(3);
        retry.reply({ feedback: 'The setup retry completed.' });
        await description(/The setup retry completed/).waitFor();

        const ids = [first, second, third, retry].map(({ request }) => assertRequest(request, {
            action: 'setup', name: 'Wave Preview', domain: 'wave.example', username: 'demo', password: 'wave-demo',
        }));
        assert.equal(new Set(ids).size, ids.length);
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
        await canvasClick(page, 480, 424);
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
        await canvasClick(page, 480, 424);
    };

    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Panel Settings');
        await replaceText(page, 325, 141, 'Harbor QA');
        await apply();
        const first = await preview.action(0);
        assertRequest(first.request, { action: 'apply_settings', settings: { ...original, name: 'Harbor QA' } });

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
    try {
        await page.goto(url);
        await page.getByRole('img', { name: /Wave Users.*2 users/ }).waitFor();
        await openView(page, 'Nodes & Routers');
        await waitForCanvas(page, portrait, true);
        await settleCanvas(page);
        const clip = { x: 40, y: 375, width: 310, height: 180 };
        const before = await page.screenshot({ clip });
        await swipe([{ x: 150, y: 520 }, { x: 180, y: 502 }, { x: 221, y: 479 }]);
        assert.ok(!(await page.screenshot({ clip })).equals(before), 'The touch drag pans the map.');
        assert.equal(await page.evaluate(() => scrollY), 0);
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
    } finally {
        gate.resolve();
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
