# Authenticated HTTPS mode

Wave’s authenticated mode provides first-run setup and access to the panel
shell. The HTTP preview runs with its own in-memory demo data and demo
credentials.

## Build

Native Linux backend builds enable `WAVE_ENABLE_AUTH` by default. They use the
system OpenSSL 3 library for TLS, scrypt, HMAC, and random numbers. The build was
validated with the HLS1 OpenSSL package `3.0.2-0ubuntu1.30`. Keep the system
package on the distribution’s supported security updates. The same frontend
build serves either mode.

```sh
cmake --preset backend-release
cmake --build --preset backend-release
```

For a previously configured build that has TLS off in its CMake cache, pass
`-DWAVE_ENABLE_AUTH=ON` when reconfiguring.

## Install on the host

Use a dedicated HTTPS hostname for the authenticated listener. Wave
terminates TLS directly and requires TLS 1.2 or newer. Restrict network
access to the intended administrators.

Wave creates `wave-db.json` beside the executable on first startup. It fills
missing listener fields with `0.0.0.0` and port `2045`, preserving configured
values. The service account needs write access to that directory when Wave
creates or updates the file. Operators can also provision the settings file
in advance. Keep the database outside web document roots and restrict access
to trusted operators and the service account. A missing or false
`tls.enabled` starts the HTTP demo.

To enable TLS, edit `wave-db.json` before starting Wave.

```json
{
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

Provision a dedicated service account and a local authentication state
directory owned by that account with mode `0700`. The default is
`/var/lib/wave-auth`. Use `--auth-dir` with an absolute path to override it.
Keep this directory, the PEM files, and their backups outside the checkout
and every web document root, including the demo site. Give the service
account read access to the PEM files and traversal access to their parent
directories. Restrict private key and state access to the service account
and trusted operators.

## Bootstrap the administrator

Before the first launch, generate a one-time token on the host as the service
account. The state directory must already exist. Run one Wave process per
state directory.

```sh
(umask 077; set -C; openssl rand -hex 32 > /var/lib/wave-auth/bootstrap-token)
```

Launch Wave as the service account. The example database binds to loopback for
local verification. Set `listen.host` to the intended service bind address
and `listen.port` to a fixed port. Move any previous `--host` and `--port`
arguments into these settings. The browser origin uses `tls.domain` and the
listener port, omitting the port for 443.

```sh
./build/backend-release/wave-backend --site-dir build/web-release/site
```

Verify the HTTPS connection before opening the configured URL. Review and
acknowledge the projects, then enter the administrator username, password,
and bootstrap token. Read the token through your trusted local administration
channel. Enter real administrator credentials and the token only in the
authenticated HTTPS forms. After Wave creates the account, sign in with the
new credentials. [Pre-DNS verification](#verify-https-before-dns)

Wave stores the username, versioned acknowledgment, and a salted scrypt hash
in `admin.json`. It commits the file atomically before retiring the bootstrap
token. A completed installation goes directly to sign-in on a fresh browser.
Missing bootstrap credentials on first run, invalid state permissions, and TLS
configuration errors stop authenticated startup.

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

Sign-in issues a 24-hour HS256 JWT in a host-only `__Host-wave_session` cookie
with `HttpOnly`, `Secure`, `SameSite=Strict`, and `Max-Age`. Both the signing
key and active session records are stored in private state. Expiry is fixed
at sign-in. Up to 16 sessions can be active, with later sign-ins replacing the
oldest when the limit is reached. **Menu → Sign out** revokes the current
session and clears its cookie. Revocation survives a backend restart. Treat
restoration of older state backups as credential recovery because older
unexpired sessions may be present.

The entry routes and their shared WebAssembly assets load without a cookie.
The panel, `/api/session`, and the WebSocket require a current session.
HTTPS writes require the configured `Origin`, JSON,
and `X-Wave-CSRF: 1`. `/ws/panel` requires that same origin and a current
session before upgrading, then closes idle browser connections when their
session expires or is revoked. The demo action sockets run only in demo mode.

## Tests

The native suites use Node.js 22 or later and the OpenSSL command-line tool.
`backend.tls` exercises database initialization, startup rejection, and client
trust checks without a listener. `backend.auth` and `backend.smoke` require
loopback sockets. The HTTPS fixture generates a private root, intermediate,
and leaf certificate for a generic hostname and resolves it locally.

```sh
ctest --preset backend-debug
ctest --preset backend-release
```

Run the browser session test against a built frontend with the existing
Chromium test dependencies. On Linux the test uses NSS `certutil` to trust
the generated root in a temporary browser trust store. [Frontend test prerequisites](building.md#frontend-tests)

```sh
WAVE_BACKEND="$PWD/build/backend-release/wave-backend" \
WAVE_SITE_DIR="$PWD/build/web-release/site" \
node --test tests/frontend/auth.test.cjs
```
