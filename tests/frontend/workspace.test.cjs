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

        const menu = page.getByRole('button', { name: 'Menu' });
        const tools = page.getByRole('button', { name: 'Tools' });
        for (const [button, other] of [[menu, tools], [tools, menu]]) {
            await button.focus();
            await page.keyboard.press('Enter');
            await page.locator(`#${await button.getAttribute('id')}[aria-expanded="true"]`).waitFor();
            assert.equal(await other.getAttribute('aria-expanded'), 'false');
            await button.focus();
            await page.keyboard.press('Enter');
            await page.locator(`#${await button.getAttribute('id')}[aria-expanded="false"]`).waitFor();
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
        await page.keyboard.press('Tab');
        assert.equal(await page.evaluate(() => document.activeElement.id), 'canvas');
        await settleCanvas(page);
        assert.ok((await page.screenshot({ clip: headerEdge })).equals(unfocusedEdge),
            'Keyboard focus leaves the canvas edge unchanged.');
        assert.ok(!(await page.screenshot({ clip: brandTile })).equals(unfocusedBrand),
            'Keyboard focus is visible on the brand tile.');

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

        await canvasClick(page, 590, 171);
        await canvasClick(page, 625, 235);
        await canvasClick(page, 490, 554);
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
        await replaceText(editor, 495, 212, 'Avery Local');
        await replaceText(editor, 457, 367, '25');

        await canvasClick(other, 514, 171);
        assertRequest((await secondPreview.action(0)).request,
            { action: 'set_user_enabled', id: 1, enabled: false });
        await firstPreview.broadcast();
        await editor.getByRole('img', { name: /Avery Brooks, disabled, 3\/5/ }).waitFor();

        await openFirstEdit(other);
        await replaceText(other, 457, 399, '18');
        await canvasClick(other, 481, 654);
        assertRequest((await secondPreview.action(1)).request, {
            action: 'edit_user', id: 1, options: { download_limit: 18 },
        });
        await firstPreview.broadcast();
        await editor.getByRole('img', { name: /Avery Brooks, disabled, 3\/18/ }).waitFor();

        await canvasClick(editor, 481, 654);
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
