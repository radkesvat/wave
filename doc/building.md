# Build and preview Wave

Wave builds a WebAssembly panel and a Linux server that manages the WaterWall
executable and supplies host metrics and preview workspace data. Use a browser
with WebGL 2 enabled. After sign-in, the workspace starts on Users with sample
records. See **Panel Controls** on Server Status for the WaterWall process state.

## Prerequisites

- CMake 3.24 or newer and Ninja on your `PATH`
- Git and Python 3 for the Emscripten SDK setup
- Emscripten SDK 7.0.1
- Network access on the first build to fetch Dear ImGui v1.93.0, ImPlot v1.1,
  nlohmann/json v3.12.0,
  xterm.js 6.0.0, its Fit Addon 0.11.0, and the SDK's SDL2 port.
  [ImPlot release](https://github.com/epezent/implot/releases/tag/v1.1) [xterm.js](https://www.npmjs.com/package/@xterm/xterm/v/6.0.0) [Fit Addon](https://www.npmjs.com/package/@xterm/addon-fit/v/0.11.0)

The SDK supplies the Clang-based `em++` driver and the Emscripten CMake toolchain.
Install the SDK outside the Wave checkout and activate it in the shell you will
use to build. [Emscripten SDK setup](https://emscripten.org/docs/getting_started/downloads.html)

```sh
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 7.0.1
./emsdk activate 7.0.1
source ./emsdk_env.sh
```

## Build

Run the build commands from the Wave root after activating the SDK.

### Debug

```sh
cmake --preset web-debug
cmake --build --preset web-debug
```

### Release

```sh
cmake --preset web-release
cmake --build --preset web-release
```

The release preset uses `-O3 -DNDEBUG` and enables link-time optimization
with `-flto` during compilation and linking. It generates a browser-only
JavaScript loader. [Emscripten optimization guide](https://emscripten.org/docs/optimizing/Optimizing-Code.html#lto)

Run the native backend to serve either frontend build. Pass
`--site-dir build/web-debug/site` to use the debug build. [Native backend](#native-backend)

Each preset writes `index.html`, `index.js`, `index.wasm`, the console's
JavaScript and stylesheet, and third-party license notices to its own
`site/` directory. Add `--verbose` to a build command
to show the compile and link commands. When switching SDK versions, add
`--fresh` to the configure command to reset CMake's cached compiler and toolchain.

## Workspace theme

Wave starts with a near-black canvas, charcoal panes, and lavender text.
Orange marks the right edge of the active navigation row, keyboard focus,
and highlighted values. Green indicates success or an enabled state, blue
indicates links and information, amber indicates warnings, and coral indicates
errors. Panes, tabs, and controls use rounded corners. Table rows, plot grids,
and progress fills keep straight edges. Borders use logical pixels so they
scale with the canvas.

Workspace buttons use a lighter fill, a subtle shadow, and distinct colors
for hover and press. The header uses flat Menu and Tools controls.

Wave embeds Poppins Medium 4.004. Workspace text uses 23 logical pixels from
1600 CSS pixels and 18 at narrower widths. Secondary text and headings use
shared sizes. The font notice is available in About and under `site/licenses/`.

The frontend theme defines the shared colors and geometry for ImGui widgets
and custom drawing. `WAVE_STARTUP_CANVAS` and `WAVE_STARTUP_TEXT` in the
frontend CMake file also generate the browser shell and a C++ swatch header.
Rebuilding after a swatch change updates both the loading screen and the
canvas with no additional asset requests.

Choose **High contrast** in **Menu** to strengthen boundaries, secondary text,
and keyboard focus. The workspace and its widgets apply the variant together
on the next frame. Toggle it again to restore the default. Reloading starts
with the default theme.

## Frontend tests

The browser smoke tests live in `tests/frontend/`. Use Node.js 22 or later
and npm to install the pinned test dependencies and headless Chromium.

```sh
npm ci --prefix tests/frontend
npm run --prefix tests/frontend install-browser
```

After building a preset, use the matching CTest command. Each run serves its
built site on a separate local port and supplies a WebSocket fixture for the
workspace in Chromium. The smoke suite covers browser startup and the main
workspace workflows.

```sh
ctest --preset web-debug
ctest --preset web-release
```

The **Frontend** GitHub Actions workflow builds and tests both presets on
pushes to `main` and on pull requests. Each successful job uploads its
generated site as a separate artifact.

## Frontend C++ formatting

Use clang-format 23.1.6 with `clang-format` on your `PATH`. The
`frontend/.clang-format` configuration applies to the frontend sources
and headers. Run the check from the Wave root. [LLVM 23.1.6 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.6)

```sh
rg --files -0 frontend -g '*.cpp' -g '*.h' | xargs -0 clang-format --dry-run --Werror
```

Replace `--dry-run --Werror` with `-i` to apply the format.

## Native backend

The backend needs CMake 3.24 or newer, Ninja, host C and C++20 compilers, and
the OpenSSL 3 development package. The first configure downloads cpp-httplib
v0.67.0, nlohmann/json v3.12.0, and the Argon2 reference implementation release
`20190702` and verifies their pinned archive checksums. [cpp-httplib release](https://github.com/yhirose/cpp-httplib/releases/tag/v0.67.0), [nlohmann/json release](https://github.com/nlohmann/json/releases/tag/v3.12.0)

On Linux, the build patches cpp-httplib so closing a WebSocket wakes a thread
that is still reading terminal input. Linux builds include HTTPS support by
default. Use `-DWAVE_ENABLE_AUTH=OFF` to build without HTTPS support.
HTTP still requires administrator sign-in.

Run these commands from the Wave root with the native compiler on `PATH`.
Use `backend-debug` in both commands for a debug build.

```sh
cmake --preset backend-release
cmake --build --preset backend-release
```

If an existing backend build directory was configured from `backend/`, add
`--fresh` to the first configure command to reset its CMake cache.

The executable is `build/backend-release/wave-backend` or
`build/backend-debug/wave-backend`. After building the frontend with the
`web-release` preset, start the backend from the Wave root and open
[the local Wave preview](http://127.0.0.1:2045/).

Wave creates `wave-db.json` beside its executable on first launch and fills
in any missing listener settings. The default is `0.0.0.0:2045`, which binds
to all IPv4 interfaces. Before starting with an empty administrator list,
restrict network access to the intended administrator or set `listen.host`
to `127.0.0.1`. Wave uses HTTP when TLS is disabled in the database.
[Authentication and HTTPS configuration](authentication.md)

```sh
./build/backend-release/wave-backend
```

The Logs pane shows panel activity alongside WaterWall's Core, DNS, Network,
and Internal messages. The five source checkboxes control what appears in the
list and what **Copy** includes. The pane also provides search and auto-scroll.
**Clear** clears the panel's view.

Completed changes such as saving settings or enabling a config appear in
**Logs**. Errors and warnings stay visible where the action was taken.

Menu opens version information and signs you out. Tools opens diagnostics
and input helpers. **ImGui Demo** and **ImPlot Demo** toggle the libraries'
interactive example windows.

Preview dialogs share a square-cornered frame, a dark title strip, and a
right-aligned action row below the body. Workspace dialogs center in the
visible viewport, including when the page is scrolled. Entry dialogs center
beneath Wave's header when there is room. In very short viewports, the
dialog body and actions scroll together. At larger heights, the actions
stay visible while a long body scrolls. Action rows wrap on narrow screens.

### First-run setup

An empty `admin` array in the database opens **Set up administrator**. Enter
the username and password, then select **Create administrator**. Sign in
with those credentials to open the workspace. A browser with a valid
session opens the workspace directly.

Use **Settings → Panel → Administrator** to change the username or password.
Saving changes signs out every active session. The **Panel** tab also shows
the sample domain, certificate chain path, and private key path.
**Apply changes** keeps those sample values for the current preview process.
Configure deployment TLS in `wave-db.json`. [Administrator setup](authentication.md#set-up-the-administrator)

### Preview controls and server options

The sidebar starts with **Server Status**, followed by the WaterWall configs.
**Console**, **Settings**, and **About** are pinned to the bottom of the desktop
sidebar and share the last row on narrow screens.
Tab goes from **Menu** and **Tools** into the workspace. Press Enter on a
focused pane, then use Tab to move between its controls.

Wave's scrollable panes use inset, rounded scrollbar thumbs and share the
same track, hover, and drag colors in each contrast mode. Classic page
scrollbars use those colors and rounded ends where browser styling is
supported. Overlay scrollbars use the browser's native appearance.

Server Status shows CPU and Memory in the top row when the work pane is
wide enough. Server Information and Panel Controls each have their own card
under CPU, beside Host Network. When space is tight, the five cards stack in order
from CPU and Memory through Server Information, Panel Controls, and Host Network.
Each plot shows current readings in a legend inside its upper-left corner.
On a phone, swipe through the cards to reach Logs.
At short desktop heights, scroll inside the work pane to see the rest of
the grid. Server Status reserves the browser scrollbar gutter on platforms
with a classic scrollbar, so the column count stays consistent as the page
starts or stops scrolling.

`--site-dir` selects a frontend build directory relative to the current
working directory or by absolute path. It defaults to `build/web-release/site`
and must contain `index.html`, `index.js`, and `index.wasm`. Set `listen.host`
and `listen.port` in `wave-db.json` to choose the bind address and port. Use
`127.0.0.1` for local-only access. For an HTTP listener, port `0` lets the
operating system choose a free port. The backend prints its address and
bound port at startup. The `/ws` endpoint echoes text messages over the same
port after authentication. Panel assets are served with `Cache-Control: no-store`.

The signed-in workspace opens `/ws/preview` on the same host and port. A JSON
frame with `type` set to `snapshot` carries logs, server metrics, config names,
and settings in `data`. Requests specify an `action` and its fields. The
server replies with a snapshot and action feedback. Config contents and
AuthenticationServer user lists are sent in the `result` of the requesting
browser's reply.
Forms that wait for confirmation send a unique numeric `request_id`. The
server echoes it to the requesting browser, and the form completes only
from the reply carrying its ID. Browsers still apply valid snapshot data
when a reply arrives after the form has been closed or replaced. If the
socket closes between render frames, the form can still read a reply it
already received. A form still waiting for its reply reports the disconnect.

Send one or more of `domain`, `certificate_path`, and `private_key_path`
in the `settings` object of an `apply_settings` request.

Wave samples Linux CPU, RAM, and network counters twice per second,
including when browsers are disconnected. CPU shows aggregate busy time
since the previous reading on a 0–100% scale. Memory shows
`MemTotal - MemAvailable` against total usable RAM in GiB. Host Network uses
receive and transmit byte counters from non-loopback interfaces visible to
Wave. It calculates rates from counter changes on matching interfaces over
the measured interval and uses KiB/s for small rates. Its sent and received
totals reflect the current interface counters. Stacked virtual interfaces can
count traffic on more than one link. [Linux procfs](https://docs.kernel.org/filesystems/proc.html), [Linux interface statistics](https://docs.kernel.org/networking/statistics.html)

CPU and network rates become available after the first sampling interval.
Unavailable readings appear as gaps after a chart has data. The charts keep
up to 19 samples in memory and draw the latest one at **Now**. Hover over
a plot for its sample values. History starts fresh when Wave restarts.

The authenticated `/ws/overview` endpoint sends one JSON text frame and closes
the connection. It reports the same current `cpu_percent`, `memory_used_gib`,
`memory_total_gib`, and `uptime_minutes` as the workspace. Its
`download_mib_per_second` array contains up to 13 recent download samples,
oldest first.

With the default HTTP listener, open `http://<server-ip>:2045/` from another
machine to reach the sign-in page. [HTTPS configuration](authentication.md#configure-tls)

### Console

Sign in and select **Console → Open terminal** to start a Linux shell. The
shell starts beside `wave-backend`, the same working directory Wave uses for
WaterWall. Wave uses Bash when installed and falls back to `/bin/sh`.
The shell has the permissions of the operating system account running Wave.
If Wave runs as root, the console has root access.
Use HTTPS when accessing the console remotely.
[HTTPS configuration](authentication.md#configure-tls)

Run programs installed on the server, including `nano`, `btop`, and `htop`.
The browser sends keystrokes and paste to a Linux pseudoterminal with
`TERM=xterm-256color` and updates its dimensions as the pane resizes.
Full-screen programs use the host's `xterm-256color` terminfo entry.
Use **Ctrl+Shift+V** or the browser's Paste command to paste on Linux.
Select **Ctrl**, then press a letter to send its control character.
Select **Esc** to send Escape.
Press **Ctrl+Shift+Space** to move focus from the terminal to **Close terminal**
while the shell or a full-screen program keeps running. Enable **Screen reader
output** in the toolbar to read terminal rows and hear new output.
Use `exit` or **Close terminal** to end the session. Leaving Console,
disconnecting, and session revocation also close its terminal. A Wave backend
can host up to four console sessions at a time.
On Linux 5.1 and later, Wave can forcibly stop foreground pipeline stages
that ignore a terminal hangup. On older kernels, a foreground job that
ignores both interrupts and hangups can continue after disconnecting.

The terminal uses the authenticated, same-origin `/ws/console` WebSocket.
Binary frames carry terminal bytes in both directions. A text frame such
as `{"type":"resize","cols":100,"rows":30}` starts the shell on first receipt
and resizes it thereafter. The server reports terminal status in text
frames with `type` set to `ready`, `exit`, or `error`.

Each binary input message can carry up to 64 KiB. The server allows up to
1 MiB of pending input per terminal. The browser requests input acknowledgements
with `"input_ack":true` in the first resize frame. The server adds
`"input_window":1048576` to its `ready`
frame and sends `{"type":"input_ack","bytes":N}` as it writes each group of
`N` bytes to the pseudoterminal. The browser sends a whole paste only when it
fits in the remaining window. If the queue is full, wait and retry the paste.
Split pastes larger than the window into smaller pieces. With an older backend
that omits `input_window`, the browser limits the WebSocket's outgoing buffer.

### Panel Controls and versioning

Each Wave panel release uses the same version number as the WaterWall core
release it supports. Wave 1.6.0 installs WaterWall from the
[v1.6.0 release](https://github.com/radkesvat/WaterWall/releases/tag/v1.6.0).
The current build offers only version 1.6.0 in the panel version picker.

Wave creates two directories beside `wave-backend`:

- `waterwall_bin/` holds the `Waterwall` executable. On first startup, Wave
  downloads and extracts the matching release in the background if the binary
  is missing. Automatic installation requires `curl` and `unzip` on `PATH`
  and access to GitHub. Wave selects the Linux x86-64 or ARM64 archive built for
  older CPUs and checks its pinned SHA-256 before extracting the executable.
- `configs/` starts empty. The **Create config** menu item creates
  `configs/<name>/config.json` with a WaterWall configuration skeleton.
  Select its name in the sidebar to add nodes. Wave stores config names
  and enabled flags in `wave-db.json` under `core_configs`.

After a failed install or removal of the executable, **Install core** replaces
the lifecycle buttons and can retry the download. Wave records the first
install attempt in `wave-db.json` so later restarts leave retries to the
administrator. The panel remains available during installation and reports
any error. Precreate `waterwall_bin/` and place the matching executable there
with execute permission to provision Wave offline.

In Panel Controls, **Start core** shares a row with **Stop core**, and **Backup**
shares a row with **Restore**. The left button in each pair gets twice the width.
The desktop grid uses shorter rows, and the stacked layout gives touch controls
more height. **Start core** is available after installation while WaterWall is
stopped. **Stop core** and **Restart core** ask for confirmation while WaterWall
is running. Restart waits for the old process to exit, then uses the saved
settings and enabled configs. If the executable is removed while WaterWall is
running, stop it to reinstall. Wave starts WaterWall only when requested.

Use **Settings → Core** to configure the WaterWall release that matches this
version of Wave. **Runtime** covers workers, memory profiles, MTU, TCP tuning,
library paths, and advanced userspace stack limits. **Logging** covers the log
directory and each of the four loggers. **DNS** covers address preference,
servers, search domains, lookup order, and advanced resolver and failover
options. Settings left at the core default are omitted so WaterWall can use
its built-in or system defaults. [WaterWall 1.6.0 core settings](https://github.com/radkesvat/WaterWall/blob/v1.6.0/core/readme.md)

Wave reads new lines from WaterWall's dated log files during each run. The
four logger file destinations must be enabled and distinct for Wave to identify
their sources. **Clear** leaves WaterWall's files intact. Wave retains up to 100
entries per source in memory.

Wave validates the form and saves the overrides in `wave-db.json` under
`core_settings`, grouped as `misc`, `log`, and `dns`. On **Start core**, Wave
builds `core.json` from those groups and adds the paths of enabled configs.
The generated file is passed through an anonymous in-memory
file. Relative paths resolve from the directory containing `wave-backend`.
Enable at least one config before starting. Saved settings and enabled flags
take effect on the next start.

Wave loads supported values saved by the previous JSON editor and converts
legacy aliases to the corresponding form options. If the saved settings include
unsupported fields or invalid values, review the notice in **Settings → Core**,
then save the form before starting WaterWall. Saving replaces the previous
`core_settings` with the supported values shown in the form.

For deployments using `waterwall_config/` from an earlier build, enter the
core settings in the form, create an entry for each config, and copy each node
JSON into its new `configs/<name>/config.json`. Wave leaves the old directory
in place.

The top-bar **Menu** provides **Create config** on every workspace page.
New configs start disabled. Names are case-sensitive,
can contain up to 64 UTF-8 bytes, and must be unique after trimming surrounding
spaces. Blank names, `.` and `..`, path separators, and ASCII control
characters are invalid. Open a config in the sidebar and use the switch in its
header to enable or disable it. The header's **Delete** button asks for
confirmation before removing its `config.json` and any empty config folder.
Configs in use by the running core are protected.

#### Nodes and AuthenticationServer users

Select a config in the sidebar and open the **Nodes** tab. Use the graph's
**Nodes** menu to add, edit, or delete WaterWall 1.6.0 nodes. Drag nodes to
move them and use the connection ports or the next node selector to link
them. Double-click a node card to open its editor. The graph labels links
defined in WaterWall settings, including split paths, pairs, fallbacks,
authentication dependencies, and route targets.
Set named targets in the node editor. Edit Router rules and SniffRouter routes
in **Settings JSON**. Renaming a node updates references in the config.
TunDevice and RawSocket show the connected port when placed at either end of
a chain. Unconnected adapters show both ports so you can attach either side.
Roll the mouse wheel up to zoom in around the pointer and down to zoom out.
Use **View** to zoom or fit the graph. In **Config**, choose **Save changes**
to write `config.json` or **View saved JSON** to see the saved file. Wave writes
coordinates into each node's `panel-settings` object. WaterWall ignores that
object.

Use **Config → Edit JSON** to edit the current draft or paste an existing `config.json`
or a JSON array of nodes. **Apply nodes** replaces the draft's nodes and keeps
the destination config's top-level settings. Review the graph, then use **Save
changes**. For imported AuthenticationServer nodes, Wave keeps any existing
local user database and assigns a local database path to each new node.

Pasted configs with WaterWall variables, `//` comments, or top-level `commits`
metadata require confirmation. **Resolve and save** replaces unquoted `$name$`
references with their JSON values, discards the variable definitions, comments,
and commits metadata, and saves the imported nodes as plain JSON. The graph
updates after the save succeeds. Paste and expansion are limited to 1 MiB each.

Wave edits JSON config files up to 256 KiB. If another browser or a local
editor changes the file first, reload it before saving again. Add and configure
the nodes needed for each chain before enabling the config. WaterWall checks
each node's settings and chain compatibility when it starts.

Saving an **AuthenticationServer** adds a tab with the node's name. Its user
database is a separate JSON file under `configs/<name>/`. Wave sets the node's
`db-path` and keeps the same file when the node is renamed. The offline editor
can read and change the database while WaterWall is stopped. WaterWall writes
its in-memory users back to the database and can keep recovery backups in the
same folder.

Use **Add user** or **Edit** to change the account, access, expiry, and limit
settings. You can enter a unique nonzero user ID when adding an account or
leave it blank to generate one. Existing IDs, creation and first-use times,
and recorded statistics are shown for reference.

Traffic quotas use charged bytes. Bandwidth limits use bytes per second. Zero
or blank limits mean unlimited. Absolute expiry uses Unix epoch milliseconds,
and first-use expiry uses a duration in milliseconds. Clearing a numeric
override restores WaterWall's default, including the 120,000 ms statistics
record interval. A WireGuard account can have one IPv4 or IPv6 CIDR.

End-user passwords are stored in WaterWall's required format in the private
node database. The panel accepts new passwords and never returns existing
passwords in user lists. Wave reads databases up to 16 MiB and loads 50 users
per page. Deleting a node or removing a config preserves user
database files. To manage a pre-existing AuthenticationServer in Wave, place
its database in the config folder and set its `db-path` to that file before
editing the node. Each server also needs its own `auth-clients` settings before
it can run. [WaterWall 1.6.0 AuthenticationServer](https://github.com/radkesvat/WaterWall/blob/v1.6.0/tunnels/AuthenticationServer/description.md)

A saved config with a direct **TcpListener → APIServer** connection also has a
**Live** tab for each AuthenticationServer. The tab is empty until WaterWall
runs that config and answers a `list-users` request. The live list refreshes
while open and shows account state and accepted upload and download totals.

Use a dedicated API listener on `127.0.0.1` or `::1` with an explicit port. Wave
also reaches wildcard listeners through loopback. In the APIServer's
`settings.clients`, set a client name, a 64-character hexadecimal token, and
an `allow` entry with the config's top-level `name`, the exact
AuthenticationServer node name, and `"operations": ["list-users"]`. Wave
verifies the local TCP peer belongs to its WaterWall process before sending
the request. [WaterWall 1.6.0 APIServer](https://github.com/radkesvat/WaterWall/blob/v1.6.0/tunnels/APIServer/description.md)

**Backup** in Panel Controls downloads `wave-preview-backup.json` with the
sample TLS domain, PEM paths, and the inactive config list. The backup contains
config names and enabled flags. **Restore** replaces the inactive config list
after confirmation, retains existing `config.json` files, and creates a
skeleton for a restored name that lacks a folder. For deployment settings,
see [Authentication and HTTPS](authentication.md).

The workspace snapshot reports the build version in `server.version` and
`server.available_panel_versions`. The `server.core` object reports `state`,
any `error` from the installer or process, `configs`, and the `lifecycle`
and `configs` capability flags. Requests use the existing `request_id` and
snapshot reply. The install, start, stop, and restart replies confirm receipt and
later snapshots report completion.

When a reply includes `feedback_in_logs_only: true`, Wave shows its success
message in Logs. The reply still carries `feedback` and any `result` data.
While a reply is pending, the controls block duplicate submissions. If the
connection closes, Server Status shows the last snapshot and offers to reconnect.

| Action | Additional request fields |
| --- | --- |
| `install_core`, `start_core`, `stop_core`, `restart_core` | None |
| `apply_core_settings` | WaterWall 1.6.0 `misc`, `log`, and `dns` overrides as `settings`, up to 16 KiB |
| `create_core_config` | `name` |
| `get_core_config` | Config `id`. The result has `config`, `revision`, and `editable`. |
| `save_core_config` | Config `id`, edited `config` object, and last loaded `revision` |
| `set_core_config_enabled` | Config `id` and boolean `enabled` |
| `remove_core_config` | Config `id` |
| `get_config_users` | `config_id`, AuthServer `node` name, and optional zero-based `page` |
| `get_live_config_users` | `config_id`, AuthServer `node` name, and optional opaque `cursor` from the previous live page |
| `create_config_user` | `config_id`, `node`, `user` object with `password` and optional decimal string `id`, and optional `page` |
| `edit_config_user` | `config_id`, `node`, string `user_id`, changed fields in `user`, and optional `page`. Set a numeric override to `null` to restore its default. |
| `delete_config_user` | `config_id`, `node`, string `user_id`, and optional `page` |
| `restore_preview_backup` | Downloaded JSON object as `backup` |

### Backend tests

The backend smoke tests use Node.js 22 or later, a temporary site fixture,
and an OS-assigned loopback port. They can run without an Emscripten build
or npm dependencies. From the Wave root, run the test preset that matches the
build.

```sh
ctest --preset backend-debug
ctest --preset backend-release
```
