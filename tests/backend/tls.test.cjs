const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const { chmod, mkdir, mkdtemp, readFile, rename, rm, stat, symlink, writeFile } = require('node:fs/promises');
const { tmpdir } = require('node:os');
const { dirname, join, resolve } = require('node:path');
const { test } = require('node:test');
const { promisify } = require('node:util');
const { copyBackend, hostname, timeout, tlsFixture, writeTlsConfig } = require('./tls_fixture.cjs');

const run = promisify(execFile);

async function deployment(t) {
    assert.ok(process.env.WAVE_BACKEND, 'Set WAVE_BACKEND to a Wave build with WAVE_ENABLE_AUTH=ON.');
    const cwd = await mkdtemp(join(tmpdir(), 'wave-tls-test-'));
    t.after(() => rm(cwd, { recursive: true, force: true }));
    const backend = await copyBackend(resolve(process.env.WAVE_BACKEND), cwd);
    const site = join(cwd, 'site');
    await mkdir(site);
    for (const name of ['index.html', 'index.js', 'index.wasm']) await writeFile(join(site, name), '');
    const pair = await tlsFixture(cwd);
    const config = join(dirname(backend), 'wave-db.json');
    const args = ['--site-dir', site];
    const execute = (...extra) => run(backend, [...args, ...extra], { cwd, timeout });
    const writeConfig = (files = pair, domain = hostname) => writeTlsConfig(config, files, { domain });
    return { cwd, site, pair, config, execute, writeConfig };
}

async function rejectStartup(command, message, privateValues = []) {
    await assert.rejects(command, error => {
        assert.equal(error.code, 1);
        assert.equal(error.stdout, '');
        assert.match(error.stderr, message);
        for (const value of privateValues) assert.ok(!error.stderr.includes(value));
        return true;
    });
}

test('checks TLS without a site and follows renewed PEM symlinks', async t => {
    const { cwd, pair, config, execute } = await deployment(t);
    const chainPath = join(cwd, 'live chain.pem');
    const keyPath = join(cwd, 'live key.pem');
    await symlink(pair.chainPath, chainPath);
    await symlink(pair.keyPath, keyPath);
    const contents = JSON.stringify({
        admin: [],
        listen: { host: '127.0.0.1', port: 8443 },
        tls: { enabled: true, domain: hostname, certificate_chain: chainPath, private_key: keyPath },
    });
    await writeFile(config, contents, { mode: 0o600 });

    const check = () => execute('--check-tls', '--site-dir', join(cwd, 'no-site'));
    assert.match((await check()).stdout, /TLS configuration is valid/);
    const renewed = await pair.issue('renewed');
    for (const [link, target] of [[chainPath, renewed.chainPath], [keyPath, renewed.keyPath]]) {
        await symlink(target, `${link}.next`);
        await rename(`${link}.next`, link);
    }
    assert.match((await check()).stdout, /TLS configuration is valid/);
    assert.equal(await readFile(config, 'utf8'), contents);
});

test('rejects incomplete TLS settings and unsafe certificates before binding', async t => {
    const { cwd, pair, config, execute, writeConfig } = await deployment(t);
    const expired = await pair.issue('expired', { start: -2, end: -1 });
    const wrongChain = join(cwd, 'unrelated chain.pem');
    await writeFile(wrongChain, Buffer.concat([await readFile(pair.certPath), pair.ca]));

    for (const [files, domain, message] of [
        [expired, hostname, /certificate has expired/],
        [pair, 'nested.panel.example.test', /does not cover the configured hostname/],
        [{ ...pair, chainPath: wrongChain }, hostname, /certificate chain in issuer order/],
        [{ ...pair, keyPath: expired.keyPath }, hostname, /private key does not match/],
    ]) {
        await writeConfig(files, domain);
        await rejectStartup(execute(), message, [domain, files.chainPath, files.keyPath]);
    }
    await writeConfig();
    const settings = JSON.parse(await readFile(config, 'utf8'));
    delete settings.tls.private_key;
    await writeFile(config, JSON.stringify(settings));
    await rejectStartup(execute(), /Enabled TLS requires a nonempty tls\.private_key/);
});

test('requires a fixed HTTPS port in startup and TLS preflight', async t => {
    const { pair, config, execute } = await deployment(t);
    await writeTlsConfig(config, pair, { listen: { host: '127.0.0.1', port: 0 } });
    const message = /Set a fixed listen\.port in wave-db\.json for HTTPS/;
    await rejectStartup(execute('--check-tls'), message);
    await rejectStartup(execute(), message);
});

test('TLS preflight leaves database defaults and permissions unchanged', async t => {
    const { pair, config, execute } = await deployment(t);
    await writeTlsConfig(config, pair, { listen: { port: 8443 } });
    await chmod(config, 0o640);
    const original = await readFile(config, 'utf8');
    assert.match((await execute('--check-tls')).stdout, /TLS configuration is valid/);
    assert.equal(await readFile(config, 'utf8'), original);
    assert.equal((await stat(config)).mode & 0o777, 0o640);
});

test('keeps the database and TLS files out of served or checked-in directories', async t => {
    const { cwd, site, pair, config, execute, writeConfig } = await deployment(t);
    await writeConfig();

    const backup = join(cwd, 'backup.json');
    await rename(config, backup);
    await symlink(backup, config);
    await rejectStartup(execute(), /wave-db.json must be a regular file/);
    await rejectStartup(execute('--site-dir', cwd), /Keep wave-db.json outside the site directory/);
    await rename(backup, config);

    const keyInSite = join(site, 'fixture key.pem');
    await symlink(pair.keyPath, keyInSite);
    await writeConfig({ ...pair, keyPath: keyInSite });
    await rejectStartup(execute(), /outside the Git checkout and the site directory/);

    const checkout = join(cwd, 'checkout');
    await mkdir(checkout, { mode: 0o700 });
    await writeFile(join(checkout, '.git'), 'gitdir: fixture\n');
    const chainInCheckout = join(checkout, 'fixture chain.pem');
    await symlink(pair.chainPath, chainInCheckout);
    await writeConfig({ ...pair, chainPath: chainInCheckout });
    await rejectStartup(execute(), /outside the Git checkout/);
});
