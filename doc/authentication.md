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

In **Settings → Panel → TLS**, turn on **Enable TLS**, enter the domain and
PEM paths, and select **Apply changes**. Wave validates the files before
saving. Restart the Wave backend to use the saved settings, then reconnect
over HTTPS with the configured hostname and listener port. Turn off
**Enable TLS** and apply to return to HTTP after a restart. The fields stay
locked while the switch is off, and Wave keeps the saved hostname and paths.

To configure TLS before first sign-in or recover an unavailable listener,
set the same values in `wave-db.json` while Wave is stopped.

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

Set a fixed HTTPS listener port. Use the exact lowercase DNS hostname the
browser will use. The certificate’s DNS Subject Alternative Name must cover
it. A wildcard matches one label.
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

## Set the panel base path

Before starting with an empty `admin` array, bind to loopback or restrict
network access to the intended administrator. Start Wave as the service account.

```sh
./build/backend-release/wave-backend --site-dir build/web-release/site
```

When `panel-base-path` is missing from `wave-db.json`, open the listener's root
URL. **Panel base path** starts with `/panel`. You can choose another path,
such as `/admin/wave`. Start with a slash and use letters, digits, hyphens, or
underscores in each segment. Leave out the trailing slash. Wave reserves
`/_wave-setup` for setup. Save the path to open the panel there immediately.
An existing installation that starts with a missing path asks you to sign in
as the administrator before choosing a replacement.

Use the saved path for subsequent visits. Pages, assets, API requests, and
WebSockets all use that prefix. For example, `/panel` serves the workspace at
`/panel/` and its data socket at `/panel/ws/workspace`. Requests outside the
selected prefix receive an nginx-style **404 Not Found** response from Wave
with `Server: nginx` and a `Date` header in GMT.

To move an existing panel, open **Settings → Panel → Panel base path** and
save the new path. Wave applies it immediately and opens the new URL with
your existing sign-in. Other connected panel tabs move to the new URL as
well. Save open edits before moving the panel. Update any bookmarks or proxy
routes that use the previous URL.

## Set up the administrator

For HTTPS, use the configured `tls.domain` and the listener port. The browser
omits the port for 443. Open the panel at its base path. On **Set up administrator**,
enter a username of up to 64 ASCII letters, digits, periods, underscores,
or hyphens. New passwords need at least 6 characters, including an English
letter (A–Z or a–z) and a digit (0–9). Symbols are optional. Wave accepts
up to 256 UTF-8 bytes and rejects ASCII control characters. It checks for
common passwords and predictable patterns using zxcvbn-c locally. Select
**Create administrator**, then sign in with the new credentials.
The setup and sign-in dialogs show a circular spinner in the lower-left
corner while Wave checks the request.
If sign-in fails, the message appears below the divider in the dialog footer.
The sign-in background stays still when the browser requests reduced motion.
[Pre-DNS verification](#verify-https-before-dns)

To change the username or password, go to **Settings → Panel → Administrator**.
Enter the current password and leave **New password** blank to keep it.
Passwords created under an earlier policy still work at sign-in.
**Save administrator** revokes all sessions and returns you to sign-in.

## Verify HTTPS before DNS

On an isolated test client, temporarily map the exact configured hostname to
the test listener’s address in the client’s hosts file. Open the HTTPS URL
with that hostname so the browser verifies the certificate name and trust
chain. With a private CA, trust the CA in the test browser or its test trust
store first. Remove the mapping when testing is complete.

For a single request from the server host to the example loopback listener,
use `curl --resolve`. The URL supplies the name for SNI and certificate
verification. An unauthenticated request to the saved base path returns `303`.
Use `/` while the panel is waiting for its base path.

```sh
read -r -s -p 'Wave hostname ' wave_tls_hostname
printf '\n'
read -r -p 'Wave entry path (for example /panel/) ' wave_tls_path
curl --noproxy '*' --resolve "${wave_tls_hostname}:8443:127.0.0.1" \
  --silent --fail --output /dev/null --write-out '%{http_code}\n' \
  "https://${wave_tls_hostname}:8443${wave_tls_path}"
unset wave_tls_hostname wave_tls_path
```

Use the corresponding test IP and port for a remote test client. For a
private CA, supply its trusted CA PEM with `--cacert`, using a local path
entered privately. Keep hostname and trust checks enabled. After sign-in,
confirm the browser connects to the session socket over WSS and the workspace
shows the signed-in administrator. The default session socket is
`/panel/ws/panel`.

## Renew TLS

Obtain the renewed certificate before expiry and prepare the complete chain
and matching key with the same service access. When the paths change, update
both paths in `wave-db.json`. When a renewal tool keeps the configured paths
stable, wait until it has updated both files or symlinks before checking the
pair.

Run this check as the service account before restarting Wave. It reads the
database and validates the TLS configuration without changing the file or
binding a port.

```sh
./build/backend-release/wave-backend \
  --site-dir build/web-release/site --check-tls
```

Restart Wave after the check succeeds. The running listener uses the TLS
context it loaded at startup. Repeat the verified client request and confirm
the expected expiry or SHA-256 fingerprint in the browser’s certificate
viewer. Renewal uses the existing administrator state and sessions.

## Sessions

Set **Session expiry (days)** under **Settings → Panel**. It defaults to
3 days and accepts whole numbers from 1 to 365. Wave saves the value as
`session_expiry_days` in `wave-db.json`. Changes take effect for new sign-ins
as soon as they are saved. Active sessions keep the expiry set at sign-in.

Sign-in issues an HS256 JWT in a host-only cookie with `HttpOnly`,
`SameSite=Strict`, and `Max-Age` set to the configured lifetime. HTTPS uses
`__Host-wave_session` with `Secure`, and HTTP uses `wave_session`. Up to 16
sessions can be active, with later sign-ins replacing the oldest when the
limit is reached. **Menu → Sign out** revokes the current session and clears
its cookie. Revocation survives a backend restart. Treat restoration of
older database backups as credential recovery because unexpired sessions
from those backups may become valid again.

The entry routes and their shared WebAssembly assets load without a cookie.
The panel, `/panel/api/session`, and WebSockets require a current session. An
unauthenticated browser goes to sign-in, or administrator setup if the
database has no account. The route examples below use `/panel` as the base path.

Authentication POST requests require a same-origin `Origin`, JSON, and
`X-Wave-CSRF: 1`. `POST /panel/api/auth/admin` takes `username`,
`current_password`, and `password`. An empty new password keeps the
current one. The session monitor at `/panel/ws/panel` and the workspace sockets
check origin and session before upgrading. The monitor's identity message
includes an opaque `session_id` for that sign-in. The open panel uses it to
recognize the same session after a connection drops. Authentication still
requires the signed session cookie. Active connections close when their
session expires or is revoked.

`POST /panel/api/auth/base-path` takes the new path as `panel-base-path` and
requires an active session. A successful change returns HTTP 201 with the
new panel URL in `Location`. The session monitor announces the move with a
`base_path` message containing `path`. Sockets using the previous prefix close.

The Console starts a host shell under the Wave backend's operating system
account after the administrator opens it. Sign-out and credential changes
revoke access to open terminals as well. [Console](building.md#console)

## Workspace roster stream

Workspace snapshots advertise roster support with a top-level
`users_roster: true`. A browser watching a Users tab sends
`users_roster_subscribe` with a fresh `subscription` string, `config_id`,
exact `node`, `source` (`live` or `offline`), `run_id`, the
`database_generation` when present in `users_access`, and the current
`schematic_revision`. Live subscriptions require the `list-users` grant.
Subscribers receive only that node's account summaries. Navigating away,
hiding the document, ending the sign-in or closing the connection releases
the subscription. `users_roster_unsubscribe` takes the subscription string.
`users_roster_refresh` takes the same string and requests a reread or retry.
Set `resync: true` to restart delivery from the latest cached baseline.
Before sending the first baseline to a saved subscription, Wave validates
the file in a read scheduled for that entry. Concurrent entries can share that
read. The gate also applies to resync. If Wave cannot read the saved config
or verify its node structure, it keeps the last verified tabs and generations
in `users_access` with a `binding_error` message. Saved actions and roster
delivery pause until the binding is verified.

The authenticated workspace handler sends these frames with
`type: "users_roster"`, the subscription string and an `event` field.
Ordinary workspace snapshots and action replies still use their existing
format and may arrive between roster frames.

| Event | Fields and behavior |
| --- | --- |
| `begin` | Identifies an `attempt`, expected final `total`, and Wave `sequence`. A delta also carries the required `base_sequence`. |
| `chunk` | Carries the `attempt`, zero-based `index`, complete changed `rows` and `removed` IDs. Each frame is at most 128 KiB. |
| `end` | Commits the `attempt` and its expected `chunks` count, including empty rosters. May carry `host_time` and `accounting_time` samples in milliseconds. If the source became stale during delivery, sets `state` to `stale` and carries its `message`. |
| `state` | Reports `loading`, `ready`, `stale`, `error` or `lost` and a display `message`. A `lost` frame also reports a `cause` of `permission` for live list denial, `config_unverified` for an unreadable saved config, or `identity` for other access or identity losses. |

Sequences, attempts, account IDs, counters and time fields use decimal
strings. A transfer commits only after all of its chunks arrive in order.
A delta applies to its matching complete baseline. If multiple edits publish
between deliveries, Wave compares the latest version with the version already
delivered to that socket. A delta can skip sequence numbers and can have zero
chunks when edits cancel out. A viewer can request a fresh baseline when it
needs to resync. On the first `lost` frame the browser unsubscribes that ID
and keeps the loss message on screen. This releases its share of the polling
work. Retry starts a fresh scoped subscription. Protocol callers that keep
their own subscriptions can explicitly refresh them.

The backend bounds the cache at
100,000 normalized rows and 32 MiB of serialized rows per roster. It keeps
account credentials, full edit details and write preconditions on their
separately scoped action paths.

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
