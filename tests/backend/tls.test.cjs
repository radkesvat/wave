const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const { mkdir, mkdtemp, readFile, rename, rm, stat, symlink, writeFile } = require('node:fs/promises');
const { tmpdir } = require('node:os');
const { dirname, join, resolve } = require('node:path');
const { test } = require('node:test');
const { promisify } = require('node:util');
const { bindHost, copyBackend, hostname, openSSL, timeout, tlsFixture, writeTlsConfig } = require('./tls_fixture.cjs');

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

test('concurrent starts save listener defaults and TLS validates without binding', async t => {
    const { cwd, pair, config, execute, writeConfig } = await deployment(t);
    await Promise.all(Array.from({ length: 4 }, () =>
        rejectStartup(execute('--check-tls'), /TLS is disabled in wave-db.json|Another Wave process is using wave-db.json/)));
    assert.equal((await stat(config)).mode & 0o777, 0o600);
    const listen = { host: '0.0.0.0', port: 2045 };
    assert.deepEqual(JSON.parse(await readFile(config, 'utf8')), { listen, admin: [] });

    await writeFile(config, JSON.stringify({ tls: { enabled: false, domain: {}, private_key: 'unused' } }));
    await rejectStartup(execute('--check-tls'), /TLS is disabled in wave-db.json/);
    await writeConfig();
    const saved = JSON.parse(await readFile(config, 'utf8'));
    assert.deepEqual(saved.tls, {
        enabled: true, domain: hostname, certificate_chain: pair.chainPath, private_key: pair.keyPath,
    });
    const settings = JSON.stringify({ ...saved, listen, admin: [], panel_name: 'Fixture panel' }, null, 2);
    await writeFile(config, settings);
    assert.equal((await execute('--check-tls', '--site-dir', join(cwd, 'no-frontend'))).stdout,
        'TLS configuration is valid.\n');
    assert.equal(await readFile(config, 'utf8'), settings);
    await writeFile(config, JSON.stringify({ ...saved, listen: { ...listen, port: 0 } }));
    await rejectStartup(execute(), /Set a fixed listen\.port/);
    await rejectStartup(execute('--configure-tls'), /Unknown option: --configure-tls/);
});

test('authenticated startup rejects invalid hostnames, certificates, and keys before binding', async t => {
    const { cwd, pair, execute, writeConfig } = await deployment(t);
    const expired = await pair.issue('expired', { start: -2, end: -1 });
    const future = await pair.issue('future', { start: 1, end: 2 });
    const unreadableKey = join(cwd, 'unreadable key.pem');
    const unreadableChain = join(cwd, 'unreadable chain.pem');
    await writeFile(unreadableKey, await readFile(pair.keyPath), { mode: 0o000 });
    await writeFile(unreadableChain, await readFile(pair.chainPath), { mode: 0o000 });
    const invalid = join(cwd, 'invalid.pem');
    await writeFile(invalid, 'invalid fixture PEM\n');
    const malformedChain = join(cwd, 'malformed chain.pem');
    await writeFile(malformedChain, Buffer.concat([await readFile(pair.chainPath),
        Buffer.from('-----BEGIN CERTIFICATE-----\ninvalid\n-----END CERTIFICATE-----\n')]));
    const wrongChain = join(cwd, 'wrong chain.pem');
    await writeFile(wrongChain, Buffer.concat([await readFile(pair.certPath), pair.ca]));
    const missing = join(cwd, 'missing.pem');
    const encrypted = join(cwd, 'encrypted.pem');
    await openSSL(cwd, 'pkey', '-in', pair.keyPath, '-aes-256-cbc', '-passout', 'pass:fixture', '-out', encrypted);
    const { stdout: keyDer } = await run('openssl', ['ec', '-in', pair.keyPath, '-outform', 'DER'],
        { cwd, timeout, encoding: 'buffer' });
    // Clear the fixture's P-256 private scalar while retaining its public point.
    assert.deepEqual(keyDer.subarray(5, 7), Buffer.from([4, 32]));
    keyDer.fill(0, 7, 39);
    const damagedKey = join(cwd, 'damaged key.pem');
    const body = keyDer.toString('base64').match(/.{1,64}/g).join('\n');
    await writeFile(damagedKey, `-----BEGIN EC PRIVATE KEY-----\n${body}\n-----END EC PRIVATE KEY-----\n`, { mode: 0o600 });

    const failures = [
        ['expired leaf', expired, hostname, /certificate has expired/],
        ['future leaf', future, hostname, /certificate is not yet valid/],
        ['wrong host', pair, 'another.zone.test', /does not cover the configured hostname/],
        ['wildcard spans only one label', pair, 'nested.panel.example.test', /does not cover the configured hostname/],
        ['missing chain', { ...pair, chainPath: missing }, hostname, /Cannot read the TLS certificate chain/],
        ['missing key', { ...pair, keyPath: missing }, hostname, /Cannot read the TLS private key/],
        ['directory as chain', { ...pair, chainPath: cwd }, hostname, /Cannot read the TLS certificate chain/],
        ['unreadable chain', { ...pair, chainPath: unreadableChain }, hostname, /Cannot read the TLS certificate chain/],
        ['unreadable key', { ...pair, keyPath: unreadableKey }, hostname, /Cannot read the TLS private key/],
        ['malformed leaf', { ...pair, chainPath: invalid }, hostname, /Cannot load the TLS certificate chain/],
        ['malformed intermediate', { ...pair, chainPath: malformedChain }, hostname, /Cannot load the TLS certificate chain/],
        ['unrelated issuer', { ...pair, chainPath: wrongChain }, hostname, /certificate chain in issuer order/],
        ['malformed key', { ...pair, keyPath: invalid }, hostname, /Cannot load the TLS private key/],
        ['encrypted key', { ...pair, keyPath: encrypted }, hostname, /Supply an unencrypted PEM/],
        ['mismatched key', { ...pair, keyPath: expired.keyPath }, hostname, /private key does not match/],
        ['damaged private scalar', { ...pair, keyPath: damagedKey }, hostname, /Cannot use the supplied TLS private key/],
        ['relative PEM path', { ...pair, keyPath: 'server.pem' }, hostname, /Use absolute local paths/],
    ];
    for (const [label, files, name, message] of failures) {
        await t.test(label, async () => {
            await writeConfig(files, name);
            await rejectStartup(execute(), message, [name, files.chainPath, files.keyPath]);
        });
    }
    for (const name of ['https://panel.example.test', '*.example.test', 'Panel.example.test', 'panel..example.test',
        'panel.example.test.', 'panel.example.test:443', '-label.example.test', bindHost, `${'a'.repeat(64)}.test`]) {
        await writeConfig(pair, name);
        await rejectStartup(execute(), /Use a lowercase DNS hostname/, [name]);
    }
});

test('startup enforces private configuration permissions and locations', async t => {
    const { cwd, site, pair, config, execute, writeConfig } = await deployment(t);
    await writeConfig();

    const backup = join(cwd, 'backup.json');
    await rename(config, backup);
    await symlink(backup, config);
    await rejectStartup(execute(), /wave-db.json must be a regular file/);
    await rejectStartup(execute('--site-dir', cwd), /Keep wave-db.json outside the site directory/);
    await rm(config);
    await writeFile(config, '{not json}', { mode: 0o600 });
    await rejectStartup(execute(), /wave-db.json must contain a JSON object/);

    const checkout = join(cwd, 'checkout');
    await mkdir(checkout, { mode: 0o700 });
    await writeFile(join(checkout, '.git'), 'gitdir: fixture\n');
    await writeConfig();
    await rejectStartup(execute('--auth-dir', checkout), /Unknown option: --auth-dir/);
    const keyInSite = join(site, 'fixture key.pem');
    await symlink(pair.keyPath, keyInSite);
    await writeConfig({ ...pair, keyPath: keyInSite });
    await rejectStartup(execute(), /outside the Git checkout and the site directory/);
    const chainInCheckout = join(checkout, 'fixture chain.pem');
    await symlink(pair.chainPath, chainInCheckout);
    await writeConfig({ ...pair, chainPath: chainInCheckout });
    await rejectStartup(execute(), /outside the Git checkout/);
});

test('TLS can be renewed through local symlinks and is revalidated on startup', async t => {
    const { cwd, pair, config, execute, writeConfig } = await deployment(t);
    const renewed = await pair.issue('renewed');
    const chainPath = join(cwd, 'live chain.pem');
    const keyPath = join(cwd, 'live key.pem');
    await symlink(pair.chainPath, chainPath);
    await symlink(pair.keyPath, keyPath);
    await writeConfig({ chainPath, keyPath });

    async function repoint(path, target) {
        await symlink(target, `${path}.next`);
        await rename(`${path}.next`, path);
    }
    await repoint(chainPath, renewed.chainPath);
    await rejectStartup(execute(), /private key does not match/);
    await repoint(keyPath, renewed.keyPath);
    assert.equal((await execute('--check-tls')).stdout, 'TLS configuration is valid.\n');
    await writeConfig(renewed);
    assert.equal((await execute('--check-tls')).stdout, 'TLS configuration is valid.\n');
    assert.deepEqual(JSON.parse(await readFile(config, 'utf8')).tls, {
        enabled: true, domain: hostname, certificate_chain: renewed.chainPath, private_key: renewed.keyPath,
    });
    assert.equal((await stat(config)).mode & 0o777, 0o600);
});

test('enabled TLS requires a domain and both PEM paths in the database', async t => {
    const { pair, config, execute } = await deployment(t);
    const valid = { enabled: true, domain: hostname, certificate_chain: pair.chainPath, private_key: pair.keyPath };
    await writeFile(config, JSON.stringify({ tls: { enabled: 'true' } }));
    await rejectStartup(execute(), /tls.enabled in wave-db.json must be true or false/);
    for (const key of ['domain', 'certificate_chain', 'private_key']) {
        const tls = { ...valid };
        delete tls[key];
        await writeFile(config, JSON.stringify({ tls }));
        await rejectStartup(execute(), new RegExp(`Enabled TLS requires a nonempty tls\\.${key}`));
    }
});

test('a client verifies the generated chain and DNS SAN using its private CA', async t => {
    const { cwd, pair } = await deployment(t);
    const verify = (...options) => openSSL(cwd, 'verify', '-no-CAfile', '-no-CApath', '-no-CAstore',
        '-purpose', 'sslserver', ...options, pair.certPath);
    const trusted = ['-CAfile', pair.rootPath, '-untrusted', pair.issuerPath];
    assert.match((await verify(...trusted, '-verify_hostname', hostname)).stdout, /: OK\n$/);
    await assert.rejects(verify(...trusted, '-verify_hostname', 'nested.panel.example.test'),
        { stderr: /hostname mismatch/ });
    await assert.rejects(verify('-untrusted', pair.issuerPath, '-verify_hostname', hostname),
        { stderr: /unable to get local issuer certificate/ });
    await assert.rejects(verify('-CAfile', pair.rootPath, '-verify_hostname', hostname),
        { stderr: /unable to get local issuer certificate/ });
});
