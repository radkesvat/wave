# Install and manage a Wave release

Wave publishes one ZIP for Linux `x86_64` and one for Linux `aarch64`. Each
contains the native backend, the WebAssembly site, the instance manager, and
license notices. The release workflow builds and tests both packages on
Ubuntu 24.04. Use that runtime baseline or a compatible glibc-based system
with systemd, OpenSSL 3, and libstdc++. The installer uses Bash, `curl`,
Python 3.9 or later, and `flock`. Run it as root or as a regular user with
`sudo` access. Root installations require Wave v0.0.2-beta or later and use
`useradd`, `userdel`, `nologin`, and `setpriv`.
[Installation command](../README.md#install)

## Install an instance

1. Enter an exact tag, such as `v0.0.2-beta`, or press Enter for the latest
   stable package. If only a beta is available, the interactive installer
   offers it for confirmation.
2. Choose the instance directory. The default is the first available name
   among `/opt/wave`, `/opt/wave2`, `/opt/wave3`, and so on. A custom absolute
   path must have an existing parent on persistent storage outside other Wave
   installations. When run as a regular user, the installer uses `sudo` to
   register the service and to create the instance when the parent directory
   requires it. For root installations, use root-owned parent directories the
   service account can traverse, such as `/opt`.
3. Choose a port. The installer suggests the first available loopback port
   starting at `2045`. Each running instance needs its own port.
4. Start Wave when prompted, then open the printed setup URL. Wave listens on
   `127.0.0.1` for initial setup. On a remote server, run the printed SSH
   forwarding command from your computer. Choose the panel base path and create
   the administrator in your browser. [Administrator setup](authentication.md#set-up-the-administrator)

For direct remote access after setup, configure HTTPS and set `listen.host` to
the desired address in the instance's `wave-db.json` while Wave is stopped.
[HTTPS configuration](authentication.md#configure-tls)

Each instance owns its own `wave-db.json`, `configs/`, `waterwall_bin/`, and
WaterWall logs beside `wave-backend`. Panel logs go to the systemd journal.
Regular-user installations run Wave as that user and put the command link in
`~/.local/bin` by default. Root installations create a dedicated unprivileged
system account for each instance and place the backend and its writable files
in `.wave-data/` under the instance directory. The command link goes in
`/usr/local/bin` by default and is managed from the root account.

Set `XDG_BIN_HOME` to override the command directory. For root installations,
it must be in a root-owned directory tree. The installer prints the full command
path if that directory is outside your `PATH`. Systemd starts each enabled
instance after a reboot. `--no-start` defers the initial start.

The installer also accepts flags for automated or offline use. Download the
release's `install.sh`, `SHA256SUMS`, and the ZIP for your architecture to the
same directory. The installer verifies the ZIP before extracting it. Wave
uses `unzip` for automatic WaterWall downloads. You can also provision
WaterWall offline.
[WaterWall versions and offline provisioning](building.md#panel-controls-and-versioning)

```bash
bash install.sh --yes --version v0.0.2-beta --from-dir "$PWD" \
  --path /opt/wave-test --port 2045 --no-start
```

## Manage an instance

Run `wave` for an interactive menu, or give it a command. Use the name of the
instance directory for other installs, such as `wave2 status`.

| Command | What it does |
| --- | --- |
| `wave status` | Shows whether the instance is running, its active panel URL, and whether systemd starts it at boot. |
| `wave start` | Starts the systemd service and waits for the panel listener. |
| `wave stop` | Stops the instance until you start it again or reboot. Wave gives a running WaterWall process time to shut down and save before exiting. |
| `wave restart` | Stops the instance and starts it with its saved listener settings. Start WaterWall again from Panel Controls when needed. |
| `wave settings` | Shows saved settings including the listener address, port, TLS, base path, administrator username, and selected WaterWall release. |
| `wave logs panel` | Shows recent panel events and startup output from the systemd journal. Add `--follow` to watch the log. |
| `wave logs core` | Shows recent WaterWall entries from the files configured by the selected release and core settings. |
| `wave uninstall` | Offers time to back up the workspace, then stops and removes the confirmed instance, its systemd service, and its command link. |

When run as a regular user, the manager uses `sudo` for systemd lifecycle
actions and panel journal access. For a root installation, run the manager as
root. Uninstalling removes the instance's service account.

Panel Controls starts WaterWall under the Wave service account. If WaterWall
must run as root, manage it separately with a root-owned executable and
configuration.

Use the panel to edit the base path, TLS, and administrator. If saved listener
settings need a restart, compare `wave settings` with the active URL from
`wave status`. Before uninstalling, open **Panel Controls → Backup** and save
the workspace ZIP outside the instance directory. Copy any separately supplied
TLS certificates or config dependencies you want to keep.
[Workspace backup and restore](building.md#workspace-backup-and-restore)

## Package a release

Set the Wave version and channel in `CMakeLists.txt` and use the matching tag,
such as `v0.0.2-beta`. The **Wave release packages** GitHub Actions workflow
builds the web site once and uses native x86-64 and ARM64 runners for the two
backends. The smoke tests check the packaged musl notice, installation under
inherited directory permissions, boot enablement, the setup page, and the
served WebAssembly. They also check root and regular-user management and
verify that WaterWall can finish shutting down during a service stop or restart.

A manual workflow run produces a downloadable `release-bundle` artifact. A
version-tag push also stages a draft GitHub Release after both smoke tests
pass. Review the draft and publish it to make the ZIPs, `SHA256SUMS`, and
`install.sh` available. Beta tags are staged as prereleases.
