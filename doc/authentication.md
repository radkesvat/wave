# Authentication and HTTPS

Wave requires an administrator session for the workspace over HTTP and HTTPS.
Server Status shows Linux host metrics. WaterWall tunnel users are managed
separately from Wave administrators in each config's AuthenticationServer tab.
Configs with a direct TcpListener to APIServer connection can also show live
account summaries while WaterWall is running. [Nodes and AuthenticationServer users](building.md#nodes-and-authenticationserver-users)

## Build

Native Linux backend builds enable `WAVE_ENABLE_AUTH` by default. This option
controls built-in HTTPS support. All backend builds use OpenSSL 3 Crypto for
session signing and random numbers and the Argon2 reference implementation
release `20190702` for password hashing. The same frontend build serves HTTP
and HTTPS. [Argon2 reference release](https://github.com/P-H-C/phc-winner-argon2/releases/tag/20190702)

```sh
cmake --preset backend-release
cmake --build --preset backend-release
```

For a previously configured build that has TLS off in its CMake cache, pass
`-DWAVE_ENABLE_AUTH=ON` when reconfiguring.

## Database

Wave stores panel settings, core settings, and credentials in `wave-db.json`
beside the executable. On
first startup it creates the file with `admin: []` and listener settings.
It fills missing listener fields with `0.0.0.0` and port `2045`, preserving
configured values. The service account needs write access to the directory
so Wave can replace the file atomically. Run one Wave process per database.
On POSIX hosts the database is owned by the service account with mode
`0600`. Keep the database outside web document roots and restrict access to
the service account and trusted operators.

The `admin` array holds one administrator after setup. Its object contains
the `username` and a salted Argon2id PHC string in `password`. Wave generates
hashes with Argon2 version 19, 64 MiB of memory, three passes, and four lanes.
The `auth` object holds the JWT signing key and active session records. If you
replace the administrator hash manually, stop Wave and remove `auth` before
restarting to revoke existing sessions. [Argon2 parameters](https://www.rfc-editor.org/rfc/rfc9106.html#section-4)

When upgrading, keep the listener and TLS settings in `wave-db.json` and
complete administrator setup again if `admin` is empty. Earlier versions
stored a scrypt hash in a separate `admin.json`. Remove `--auth-dir` from
the launch command.

## Configure TLS

Use a dedicated HTTPS hostname for remote administration. Wave terminates
TLS directly and requires TLS 1.2 or newer. A missing or false
`tls.enabled` starts the HTTP listener, where browsers send passwords and
session cookies over the network in cleartext.

To enable TLS, edit `wave-db.json` before starting Wave.

```json
{
  "admin": [],
  "listen": {
    "host": "127.0.0.1",
    "port": 8443
  },
  "tls": {
    "enabled": true,
    "domain": "panel.example.test",
    "certificate_chain": "/path/to/fullchain.pem",
    "private_key": "/path/to/privkey.pem"
  }
}
```

Keep an existing `admin` entry if the panel is already configured.

Use the exact lowercase DNS hostname the browser will use. The certificate’s
DNS Subject Alternative Name must cover it. A wildcard matches one label.
Use absolute PEM paths. Put the leaf certificate first, followed by the
intermediates in issuer order. Supply the matching unencrypted private key.

Wave reads the PEMs in place and checks certificate dates, hostname
coverage, the supplied chain, and the key before binding. Configuration errors
stop startup. Clients verify trust with their own configured roots, including
a private CA when applicable. For an existing installation, copy the previous
`tls.json` values into the `tls` object, using `domain` for the old `hostname`,
and set `enabled` to `true`.

Keep the PEM files and their backups outside the checkout and web document
roots. Give the service account read access to the PEM files and traversal
access to their parent directories. Restrict private key access to the
service account and trusted operators.

## Set up the administrator

Before starting with an empty `admin` array, bind to loopback or restrict
network access to the intended administrator. The first successful setup
request claims the account. Start Wave as the service account.

```sh
./build/backend-release/wave-backend --site-dir build/web-release/site
```

Open the listener URL. For HTTPS, use the configured `tls.domain` and the
listener port. The browser omits the port for 443. On **Set up administrator**,
enter a username of up to 64 ASCII letters, digits, periods, underscores,
or hyphens. Use a password of at least 15 characters and up to 256 UTF-8 bytes
without ASCII control characters. Select **Create administrator**, then sign
in with the new credentials. [Pre-DNS verification](#verify-https-before-dns)

To change the username or password, go to **Settings → Panel → Administrator**.
Enter the current password and leave **New password** blank to keep it.
**Save administrator** revokes all sessions and returns you to sign-in.

## Verify HTTPS before DNS

On an isolated test client, temporarily map the exact configured hostname to
the test listener’s address in the client’s hosts file. Open the HTTPS URL
with that hostname so the browser verifies the certificate name and trust
chain. With a private CA, trust the CA in the test browser or its test trust
store first. Remove the mapping when testing is complete.

For a single request from the server host to the example loopback listener,
use `curl --resolve`. Enter the hostname privately. The URL still supplies
the name for SNI and certificate verification. A successful unauthenticated
request to `/` returns `303`.

```sh
read -r -s -p 'Wave hostname ' wave_tls_hostname
printf '\n'
curl --noproxy '*' --resolve "${wave_tls_hostname}:8443:127.0.0.1" \
  --silent --fail --output /dev/null --write-out '%{http_code}\n' \
  "https://${wave_tls_hostname}:8443/"
unset wave_tls_hostname
```

Use the corresponding test IP and port for a remote test client. For a
private CA, supply its trusted CA PEM with `--cacert`, using a local path
entered privately. Keep hostname and trust checks enabled. After sign-in,
confirm the browser connects to `/ws/panel` over WSS and the workspace
shows the signed-in administrator.

## Renew TLS

Obtain the renewed certificate before expiry and prepare the complete chain
and matching key with the same service access. When the paths change, update
both paths in `wave-db.json`. When a renewal tool keeps the configured paths
stable, wait until it has updated both files or symlinks before checking the
pair.

Run this check as the service account before restarting Wave. It reads the
database and validates the TLS configuration without binding a port.

```sh
./build/backend-release/wave-backend \
  --site-dir build/web-release/site --check-tls
```

Restart Wave after the check succeeds. The running listener uses the TLS
context it loaded at startup. Repeat the verified client request and confirm
the expected expiry or SHA-256 fingerprint in the browser’s certificate
viewer. Renewal uses the existing administrator state and sessions.

## Sessions

Sign-in issues a 24-hour HS256 JWT in a host-only cookie with `HttpOnly`,
`SameSite=Strict`, and `Max-Age`. HTTPS uses `__Host-wave_session` with
`Secure`, and HTTP uses `wave_session`. Expiry is fixed at sign-in. Up to 16
sessions can be active, with later sign-ins replacing the oldest when the
limit is reached. **Menu → Sign out** revokes the current session and clears
its cookie. Revocation survives a backend restart. Treat restoration of
older database backups as credential recovery because unexpired sessions
from those backups may become valid again.

The entry routes and their shared WebAssembly assets load without a cookie.
The panel, `/api/session`, and WebSockets require a current session. An
unauthenticated browser goes to sign-in, or administrator setup if the
database has no account.

Authentication POST requests require a same-origin `Origin`, JSON, and
`X-Wave-CSRF: 1`. `POST /api/auth/admin` takes `username`,
`current_password`, and `password`. An empty new password keeps the
current one. The session monitor at `/ws/panel` and the workspace sockets
check origin and session before upgrading. Active connections close when
their session expires or is revoked.

The Console starts a host shell under the Wave backend's operating system
account after the administrator opens it. Sign-out and credential changes
revoke access to open terminals as well. [Console](building.md#console)

## Tests

The native suites use Node.js 22 or later and the OpenSSL command-line tool.
`backend.tls` checks startup validation, private file locations, and certificate
renewal without binding a listener. `backend.auth` checks HTTPS trust and
session handling over loopback. The HTTPS fixture generates a private root,
intermediate, and leaf certificate for a generic hostname and resolves it locally.

```sh
ctest --preset backend-debug
ctest --preset backend-release
```

Run the browser sign-in smoke test against the built frontend and backend.
It uses Chromium and a loopback HTTP listener. The native suites cover HTTPS
trust and secure session handling. [Frontend test prerequisites](building.md#frontend-tests)

```sh
WAVE_BACKEND="$PWD/build/backend-release/wave-backend" \
WAVE_SITE_DIR="$PWD/build/web-release/site" \
node --test tests/frontend/auth.test.cjs
```
