const { execFile } = require('node:child_process');
const { copyFile, mkdir, readFile, writeFile } = require('node:fs/promises');
const { basename, dirname, join } = require('node:path');
const { promisify } = require('node:util');

const run = promisify(execFile);
const timeout = 10000;
const hostname = 'panel.example.test';
const bindHost = '127.0.0.1';
const bundledCoreVersion = '1.6.0';
const bundledSchematicFile = join(__dirname, '../../backend/schematics', `waterwall-${bundledCoreVersion}.json`);

function openSSL(cwd, ...args) {
    return run('openssl', args, { cwd, timeout });
}

function date(days) {
    return new Date(Date.now() + days * 86400000).toISOString().slice(0, 19).replace(/[-T:]/g, '') + 'Z';
}

async function tlsFixture(cwd) {
    const ecKey = ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-noenc'];
    await openSSL(cwd, 'req', '-x509', ...ecKey, '-days', '3', '-subj', '/CN=Wave Fixture Root',
        '-addext', 'basicConstraints=critical,CA:TRUE,pathlen:1',
        '-addext', 'keyUsage=critical,keyCertSign,cRLSign', '-keyout', 'root.key', '-out', 'root.pem');
    await openSSL(cwd, 'req', '-new', ...ecKey, '-subj', '/CN=Wave Fixture Intermediate',
        '-keyout', 'issuer.key', '-out', 'issuer.csr');
    await writeFile(join(cwd, 'issuer.ext'), [
        'basicConstraints=critical,CA:TRUE,pathlen:0', 'keyUsage=critical,keyCertSign,cRLSign',
        'subjectKeyIdentifier=hash', 'authorityKeyIdentifier=keyid', '',
    ].join('\n'));
    await openSSL(cwd, 'x509', '-req', '-in', 'issuer.csr', '-CA', 'root.pem', '-CAkey', 'root.key',
        '-set_serial', '1', '-extfile', 'issuer.ext', '-days', '2', '-out', 'issuer.pem');

    await writeFile(join(cwd, 'issued'), '');
    await writeFile(join(cwd, 'serial'), '10\n');
    await writeFile(join(cwd, 'issuer.cnf'), `
[ca]
default_ca = issuer
[issuer]
database = issued
new_certs_dir = .
serial = serial
certificate = issuer.pem
private_key = issuer.key
default_md = sha256
policy = policy
x509_extensions = server
copy_extensions = copy
[policy]
commonName = supplied
[server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
`);

    const rootPath = join(cwd, 'root.pem');
    const issuerPath = join(cwd, 'issuer.pem');
    const issuer = await readFile(issuerPath);

    async function issue(name, { san = 'DNS:*.example.test', start = -1, end = 1 } = {}) {
        const certPath = join(cwd, `${name}.pem`);
        const keyPath = join(cwd, `${name} key.pem`);
        const csr = `${name}.csr`;
        await openSSL(cwd, 'req', '-new', ...ecKey, '-subj', `/CN=${name}`, '-addext', `subjectAltName=${san}`,
            '-keyout', keyPath, '-out', csr);
        await openSSL(cwd, 'ca', '-batch', '-config', 'issuer.cnf', '-notext', '-in', csr, '-out', certPath,
            '-startdate', date(start), '-enddate', date(end));
        const chainPath = join(cwd, `${name} full chain.pem`);
        await writeFile(chainPath, Buffer.concat([await readFile(certPath), issuer]));
        return { certPath, chainPath, keyPath };
    }

    return { ...await issue('valid'), rootPath, issuerPath, ca: await readFile(rootPath), issue };
}

function coreReleaseDir(backend, version = bundledCoreVersion) {
    return join(dirname(backend), 'waterwall_bin', version);
}

function schematicFile(backend, version = bundledCoreVersion) {
    return join(coreReleaseDir(backend, version), `waterwall-${version}.json`);
}

async function copyBackend(source, cwd, directory = 'bin') {
    const backend = join(cwd, directory, basename(source));
    await mkdir(dirname(backend), { recursive: true, mode: 0o755 });
    await copyFile(source, backend);
    return backend;
}

function writeTlsConfig(config, { chainPath, keyPath }, { domain = hostname, listen, installAttempted, basePath } = {}) {
    return writeFile(config, JSON.stringify({
        listen,
        'panel-base-path': basePath ?? undefined,
        tls: { enabled: true, domain, certificate_chain: chainPath, private_key: keyPath },
        waterwall_install_attempted: installAttempted,
    }), { mode: 0o600 });
}

module.exports = { bindHost, bundledSchematicFile, copyBackend, coreReleaseDir, hostname, openSSL, schematicFile, timeout,
    tlsFixture, writeTlsConfig };
