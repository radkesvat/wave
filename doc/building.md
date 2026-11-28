# Build and preview Wave

Wave builds a WebAssembly panel and a Linux server that manages the WaterWall
executable and supplies host metrics and preview workspace data. Use a browser
with WebGL 2 enabled. After sign-in, the workspace starts on **Server Status**.
See **Panel Controls** there for the WaterWall process state.

## Prerequisites

- CMake 3.24 or newer and Ninja on your `PATH`
- Git and Python 3 for the Emscripten SDK setup
- Emscripten SDK 7.0.1
- Network access on the first build to fetch Dear ImGui v1.93.0, ImPlot v1.1,
  nlohmann/json v3.12.0, utf8proc v2.12.0,
  xterm.js 6.0.0, its Fit Addon 0.11.0, vanilla-jsoneditor 3.13.1,
  lossless-json 4.3.4, and the SDK's SDL2 port.
  [ImPlot release](https://github.com/epezent/implot/releases/tag/v1.1) [xterm.js](https://www.npmjs.com/package/@xterm/xterm/v/6.0.0) [Fit Addon](https://www.npmjs.com/package/@xterm/addon-fit/v/0.11.0)

The SDK supplies the Clang-based `emcc` and `em++` drivers and the Emscripten CMake toolchain.
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

`WAVE_ENABLE_UI_DEMOS` enables the Dear ImGui and ImPlot example windows in
**Tools**. It defaults to `OFF`, including in the web presets used by CI.
For development, set it to `ON` in a local `CMakeUserPresets.json` preset
or configure with `cmake --preset web-debug -DWAVE_ENABLE_UI_DEMOS=ON`.

Run the native backend to serve either frontend build. Pass
`--site-dir build/web-debug/site` to use the debug build. [Native backend](#native-backend)

Each preset writes `index.html`, `index.js`, `index.wasm`, the console and
JSON editor assets, and third-party license notices to its own `site/`
directory. Add `--verbose` to a build command
to show the compile and link commands. When switching SDK versions, add
`--fresh` to the configure command to reset CMake's cached compiler and toolchain.

## Workspace theme

Wave starts with a near-black canvas, charcoal panes, and lavender text.
The active navigation row has a near-black fill and an orange right edge.
Orange also marks keyboard focus and highlighted values. Green indicates
success or an enabled state. Blue indicates links and information, amber
indicates warnings, and coral indicates errors. Panes and controls use
rounded corners. Tab corners have a radius of 2 logical pixels, matching
the old panel's Default theme. Table rows, plot grids, and progress fills
keep straight edges. Borders use logical pixels so they scale with the canvas.

Workspace buttons use a lighter fill, a subtle shadow, and distinct colors
for hover and press. The header uses flat Menu and Tools controls.

Wave embeds Poppins Medium 4.004. Workspace text uses 23 logical pixels from
1600 CSS pixels and 18 at narrower widths. Secondary text and headings use
shared sizes. The font notice is available in About and under `site/licenses/`.
The tall, scrollable phone workspace keeps its layout and touch coordinates
when Wave reduces the canvas pixel density to fit WebGL framebuffer limits.

The frontend theme defines the shared colors and geometry for ImGui widgets
and custom drawing. `WAVE_STARTUP_CANVAS` and `WAVE_STARTUP_TEXT` in the
frontend CMake file also generate the browser shell and a C++ swatch header.
Rebuilding after a swatch change updates both the loading screen and the
canvas with no additional asset requests.

Choose **Menu → Theme** to switch between **Default**, **Light**, **Classic**,
**Dark**, and **Darker**. The choice applies to the workspace, charts, node
graph, scrollbars, and terminal. Wave saves the preset automatically as
`theme` in `wave-db.json` and restores it when a browser opens the authenticated
workspace. Open workspaces receive the saved preset automatically.
**High contrast** strengthens boundaries, secondary text, and keyboard focus
for the current page with any theme.

On phones, Wave opens its on-screen keyboard at the bottom of the browser
when you focus a panel text field. It uses the browser's user-agent information
to identify phones and keeps the active field above the keyboard. Use **123**
for numbers and symbols and the down arrow to dismiss the keyboard. On desktop,
**Tools → Mini keyboard** opens a scratch field with the same keys and a
**Copy** button.

## Frontend tests

The browser smoke tests live in `tests/frontend/`. Use Node.js 22 or later
and npm to install the pinned test dependencies and headless Chromium.

```sh
npm ci --prefix tests/frontend
npm run --prefix tests/frontend install-browser
```

After building a preset, use the matching CTest command. The panel and
updates, config and graph editing, and user management suites each serve
the built site on a separate local port and supply a WebSocket fixture for
the workspace in Chromium.

```sh
ctest --preset web-debug
ctest --preset web-release
```

To run the five-theme canvas and browser check on its own after a release
build, use the same site and UI demo setting as the preset.

```sh
WAVE_SITE_DIR="$PWD/build/web-release/site" WAVE_EXPECT_UI_DEMOS=0 \
  node --test --test-name-pattern='among all five themes' tests/frontend/workspace-panel.test.cjs
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
v0.67.0, nlohmann/json v3.12.0, utf8proc v2.12.0, the Argon2 reference implementation release
`20190702`, zxcvbn-c v2.7, and Miniz 3.1.2, then verifies their pinned archive checksums.
[cpp-httplib release](https://github.com/yhirose/cpp-httplib/releases/tag/v0.67.0),
[nlohmann/json release](https://github.com/nlohmann/json/releases/tag/v3.12.0),
[utf8proc release](https://github.com/JuliaStrings/utf8proc/releases/tag/v2.12.0),
[zxcvbn-c release](https://github.com/tsyrogit/zxcvbn-c/releases/tag/v2.7),
[Miniz release](https://github.com/richgel999/miniz/releases/tag/3.1.2),
[zxcvbn-c license](licenses/zxcvbn-c.txt)

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
`build/backend-debug/wave-backend`. With Ninja Multi-Config, each configuration
builds the executable in its own directory, such as `Debug/` or `Release/`.
Keep the build's `licenses/utf8proc.txt` and `licenses/miniz.txt` with the backend when distributing it.
The executable contains the default WaterWall 1.6.0 schematic. On first
startup, Wave creates its `waterwall_bin/` cache and seeds the default
release from that embedded copy. Rebuilding after a schematic edit updates
the copy in the executable. Existing cached schematics take priority.
After building the frontend with the `web-release` preset, start the backend
from the Wave root and open
[the local setup URL](http://127.0.0.1:2045/) for the first visit. Use the
saved base path for later visits. [Panel base path](authentication.md#set-the-panel-base-path)

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

Workspace operation results appear in **Logs**.

Menu opens version information and signs you out. Tools opens diagnostics
and input helpers. When built with `WAVE_ENABLE_UI_DEMOS`, Tools also
offers **ImGui Demo** and **ImPlot Demo**.

Preview dialogs share a square-cornered frame, a dark title strip, and a
right-aligned action row below the body. Workspace dialogs center in the
visible viewport, including when the page is scrolled. When space allows,
entry dialogs center beneath the **Welcome** bar on sign-in and the **Setup**
bar during administrator setup. In very short viewports, the dialog body
and actions scroll together. At larger heights, the actions
stay visible while a long body scrolls. Action rows wrap on narrow screens.
During a submitted action, a circular spinner appears at the lower left of
its dialog or beside its controls. Content areas waiting for initial data
stay empty until that data arrives. Workspace operation results are recorded
in **Logs**. Forms show validation where you enter the values.

Hover over or focus a **(?)** beside a field to read its help. On a touch
screen, tap the marker and tap outside the help to dismiss it.

### First-run setup

When the database lacks `panel-base-path`, Wave asks you to choose a base path,
starting with `/panel`. An empty `admin` array then opens **Set up administrator**.
Enter the username and password, then select **Create administrator**. Sign in
with those credentials to open the workspace. A browser with a valid session
opens the workspace directly.

Use **Settings → Panel → Panel base path** to change the URL of a running
panel. The new path takes effect when you save it.
[Panel base path](authentication.md#set-the-panel-base-path)

Use **Settings → Panel → Administrator** to change the username or password.
Saving administrator credentials signs out every active session. In the
**Panel** tab, **Session expiry (days)** controls how long new sign-ins last
and defaults to 3 days. The **TLS** box enables or disables HTTPS and saves the
domain, certificate chain path, and private key path. The fields are dimmed
and locked when **Enable TLS** is off. Enabling TLS on the HTTP panel restarts
the listener automatically. Certificate changes on an HTTPS panel and
disabling TLS take effect after a manual restart. [TLS configuration](authentication.md#configure-tls)

### Preview controls and server options

The sidebar starts with **Server Status**, followed by the WaterWall configs.
**Console**, **Settings**, and **About** are pinned to the bottom of the desktop
sidebar and share the last row on narrow screens. The dashed donation box sits
below the sidebar and matches its width and corner rounding. Select the box to
open **About**. The box shows a cat for **no donation** and for donations of
**1 USD**, **3 USD**, **5 USD**, **10 USD**, **15 USD**, and **30 USD**. The
**50+ USD** choice uses the Gigachad sticker. Before the first answer, the box
shows **no donation**. When a reminder is due, it shows **donation?** with the
first cat.
Tab goes from **Menu** and **Tools** into the workspace. Press Enter on a
focused pane, then use Tab to move between its controls.

The **Donations** section in **About** records your answer and the time you
answered. Select **Donate** in this section or in the **No donation** response
to open the project's support page. Each donation answer schedules a reminder
for 30 days after you save it. The saved choice shows the next reminder date in
`YY/MM/DD` (UTC). **No donation** clears the reminder. **Change answer** opens
the choices in a dialog. When a reminder is due, the choices return to the page.

Wave stores the answer and timestamp as plain JSON in `wave-db.json` under `donation`.
Set `answer` to `no`, `1`, `3`, `5`, `10`, `15`, `30`, or `50_plus` as a string.
Set `answered_at` to a UTC timestamp such as `2026-11-27T14:15:00Z`. Wave keeps
the original ranges for older answers saved as `below_5` or `more_than_5`. To
edit the file by hand, stop Wave, update the object, and restart Wave to load it.

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
On desktop, scroll inside the work pane when the cards stack or the window
is short. Logs stays in place, and the header uses the full canvas width.
The column count follows the browser viewport width.

`--site-dir` selects a frontend build directory relative to the current
working directory or by absolute path. It defaults to `build/web-release/site`
and must contain `index.html`, `index.js`, and `index.wasm`. Set `listen.host`
and `listen.port` in `wave-db.json` to choose the bind address and port. Use
`127.0.0.1` for local-only access. For an HTTP listener, port `0` lets the
operating system choose a free port. The backend prints its address and
bound port at startup. The `/panel/ws` endpoint echoes text messages over the
same port after authentication. Route examples use the default `/panel` base
path. Panel assets are served with `Cache-Control: no-store`.

The signed-in workspace opens `/panel/ws/workspace` on the same host and port. A JSON
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

When the workspace WebSocket is unavailable, Wave exchanges snapshots and
actions over authenticated HTTP at `/panel/api/workspace`. It polls while the
tab is visible and submits actions one at a time, without replaying actions
whose outcome is unknown. Each request identifies the current sign-in, and
actions also identify the workspace instance. The streaming account list and
host terminal require their WebSocket connections. Live exact-ID account
lookup remains available with the corresponding WaterWall grant.

The snapshot's `settings.tls` object reports the saved `enabled`, `domain`,
`certificate_chain`, and `private_key` values, along with `supported` and
`restart_required`. Send `apply_tls_settings` with a `settings` object
containing `enabled`. When enabling, include `domain`, `certificate_chain`,
and `private_key` so the backend can validate them before saving. On an HTTP
listener, `activate: true` in the action restarts the listener with HTTPS
enabled. The reply includes the new address in `result.https_url`. When
disabling, the backend retains the saved paths. The legacy `apply_settings`
action still updates the sample values used by preview backups.

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

The authenticated `/panel/ws/overview` endpoint sends one JSON text frame and closes
the connection. It reports the same current `cpu_percent`, `memory_used_gib`,
`memory_total_gib`, and `uptime_minutes` as the workspace. Its
`download_mib_per_second` array contains up to 13 recent download samples,
oldest first.

With the default HTTP listener and base path, open
`http://<server-ip>:2045/panel/`. [HTTPS configuration](authentication.md#configure-tls)

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

The terminal uses the authenticated, same-origin `/panel/ws/console` WebSocket.
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

Wave and WaterWall have independent versions. **Server Information** shows
the running Wave version, the selected WaterWall version, and the latest known
version of each. A warning-colored dot marks a newer version. The selected
WaterWall version also appears in **Panel Controls**, **Settings → Core**, and
**Version information**. The backend uses one active WaterWall schematic.
When `wave-db.json` lacks `waterwall_version`, startup selects 1.6.0 and
saves it after validating its schematic. Wave reuses the saved selection on
later starts.

Wave's panel version comes from `project(Wave VERSION ...)` in `CMakeLists.txt`.
Set `WAVE_VERSION_CHANNEL` to `beta` for a beta build or leave it empty for a
stable build.

In **Panel Controls**, **Check for updates** checks both projects. Wave searches
all pages of GitHub tags for the highest numeric version and refreshes the
WaterWall schematic catalog. Wave accepts `MAJOR.MINOR.PATCH` tags with an
optional `v` prefix and optional `-beta` suffix. It skips unrelated tags and
prefers stable over beta at the same version. A newer Wave version opens a
dialog with an **Open build guide** button that links to the
[build and deployment guide](#native-backend). Check results and errors appear
in **Logs**. If WaterWall is installing, catalog discovery waits for the
installation to finish while the Wave tag lookup runs.

Wave checks for compatible schematics on startup, every 24 hours, and after
installing a release. The backend lists `backend/schematics/` on the Wave GitHub
repository's `main` branch and fetches new or revised files by Git blob ID. It checks
the bytes and validates the schematic before atomically replacing a cached
file. Core update checks use `curl` on `PATH` and download schematic files only.
If GitHub is unreachable or a file fails validation, Wave keeps its valid
cached releases and writes the error to **Logs**.
See the [schematic authoring guide](waterwall-schematics.md).

Save open edits and stop WaterWall before switching. In **Panel Controls**,
choose a **Target version**, click **Change core version**, and confirm. Wave reuses
an installed binary or downloads the matching WaterWall archive and verifies
its SHA-256 before selecting the release. If installation fails, Wave keeps
the previous selection and writes the error to **Logs**. A successful switch
saves `waterwall_version` in `wave-db.json`, keeps the previous executable
available, and reloads open forms from saved settings and configs. The Wave
backend continues running, and the newly selected WaterWall stays stopped
until you start it.

Wave uses the following directories beside `wave-backend`.

- `waterwall_bin/<version>/` holds `Waterwall` and `waterwall-<version>.json`
  for that release. A catalog entry can have a schematic before its binary
  is installed. The installer uses the archive name and SHA-256 from the
  schematic for the current Linux architecture. Automatic installation
  requires `curl` and `unzip` on `PATH` and access to GitHub.
- `configs/` starts empty. The **Create config** menu item creates
  `configs/<name>/config.json` with a WaterWall configuration skeleton.
  Select its name in the sidebar to add nodes. Wave stores config names
  and enabled flags in `wave-db.json` under `core_configs`.

On startup, Wave creates the runtime release directories as needed. Existing
version-folder schematics take priority. For missing schematics, Wave tries
`waterwall_bin/schematics/`, then `schematics/` beside `wave-backend`.
For 1.6.0, Wave also has the embedded schematic as a fallback.
Wave validates each candidate before publishing it
atomically. It also imports the legacy `waterwall_bin/Waterwall` executable
into `waterwall_bin/1.6.0/` and preserves the legacy originals. If the legacy
executable cannot be migrated, Wave reports a startup warning and continues
with the selected release.

On first startup, Wave installs the selected release in the background if
its binary is missing. Wave records that attempt in `wave-db.json`. After
an installation failure or removal of the currently selected executable, use
**Install core**. The panel remains available during installation and reports
any error. To provision WaterWall 1.6.0 offline, place its executable with
execute permission in `waterwall_bin/1.6.0/`. For another release, place
both its executable and its schematic in `waterwall_bin/<version>/`.
A published correction with unchanged
archive metadata becomes active on the next backend startup or after
switching away from and back to that version. If the published archive
metadata changes for a selected or installed release, Wave keeps its
cached schematic and reports the conflict.

In Panel Controls, **Start core** shares a row with **Stop core**, and **Backup**
shares a row with **Restore**. The left button in each pair gets twice the width.
The desktop grid uses shorter rows, and the stacked layout gives touch controls
more height. **Start core** is available after installation while WaterWall is
stopped. **Stop core** and **Restart core** ask for confirmation while WaterWall
is running. Restart waits for the old process to exit, then uses the saved
settings and enabled configs. If the executable is removed while WaterWall is
running, stop it to reinstall. Wave starts WaterWall only when requested.

Use **Settings → Core** to configure the selected WaterWall release. In the
bundled schematic, **Runtime** covers workers, memory profiles, MTU, TCP
tuning, library paths, and advanced userspace stack limits. **Logging** covers the log
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

Select **Add API and authentication database nodes** in **Create config** to
start with an **AuthenticationClient → AuthenticationServer** chain and a
**TcpListener → UsersApi** chain. The generated API client can run
`status`, `list-users`, `get-user`, `create-user`, `update-user`, and
`change-user-password` for this config's AuthenticationServer.
The single checkbox enables live account edits,
including the Enable switch and traffic quotas. Deletion is available after
stopping WaterWall. To enable live edits on a previously created
monitoring-only config, add `get-user` and `update-user` to its existing
`list-users` grant, save the config, and restart WaterWall.

Wave creates a private, empty user database, a random API token and a separate
shared secret for the authentication chain. By default, it sets the new
**AuthenticationClient → Usage report interval (ms)** to **500**. Edit that
field to change how often the client reports accumulated traffic to WaterWall.
The TCP listener uses `127.0.0.1` and a random port from 49152 to 65535.
Wave checks that the port can be bound
during creation and skips ports already assigned to conflicting listeners in
saved configs.

#### Nodes and AuthenticationServer users

Select a config in the sidebar and open the **Nodes** tab. Use the graph's
**Nodes** menu to add, edit, or delete nodes from the selected schematic.
The graph fills
the workspace below the toolbar and follows its rounded bottom corners.
New nodes appear at the center of the visible graph at the current pan and
zoom. Right-click the graph for **Add node**, **Save**, and **Fit graph**. Right-click a node
for its **Edit node** and **Delete node** actions. Drag nodes to move them and
use the connection ports or the next node selector to link them. Double-click
a node card to open its editor. The graph labels links
defined in WaterWall settings, including split paths, pairs, fallbacks,
authentication dependencies, and route targets.
The node editor shows controls and value checks for the selected WaterWall
node type. The bundled 1.6.0 schematic includes named targets, Router rules,
and SniffRouter routes. Changing a control updates **Settings JSON** and preserves other
settings in the object. Typing in **Settings JSON** selects **Custom JSON**
and disables the settings controls, including named-target selectors. Clear
**Custom JSON** to resume editing the parsed object with those controls.
Wave checks JSON structure in both modes and checks graph references on save.
Renaming a node updates references in the config.
TunDevice and RawSocket show the connected port when placed at either end of
a chain. Unconnected adapters show both ports so you can attach either side.
Roll the mouse wheel up to zoom in around the pointer and down to zoom out.
Use **View** to zoom or fit the graph. Select **Save** at the start of the
menu bar to write `config.json`. Choose **Config → View saved JSON** to see
the saved configuration in read-only **Text** mode. Select formatted JSON
directly, or use **Copy JSON** to copy the full saved configuration.
Wave writes coordinates and each node's custom JSON preference into its
`panel-settings` object. WaterWall ignores that object.

Use **Config → Edit JSON** to edit the current draft in **Text**, which also
accepts a pasted `config.json` or JSON array of nodes. Both dialogs offer
**Level 2**, **Level 3**, and **Level 4** in **Text** and **Tree** to fold
containers at the chosen depth, counting the root as level 1. Press
**Ctrl+Shift+Space** to move keyboard focus between the JSON editor and the
dialog controls. **Apply nodes**
replaces the draft's nodes and keeps the destination config's top-level
settings. Review the graph, then select **Save**. For imported AuthenticationServer
nodes, Wave keeps any existing local user database and assigns a local database
path to each new node.

Pasted configs with WaterWall variables, `//` comments, or top-level `commits`
metadata require confirmation. **Resolve and save** replaces unquoted `$name$`
references with their JSON values, discards the variable definitions, comments,
and commits metadata, and saves the imported nodes as plain JSON. The graph
updates after the save succeeds. Paste and expansion are limited to 1 MiB each.

Wave edits JSON config files up to 256 KiB. If another browser or a local
editor changes the file first, reload it before saving again. When you confirm
a reload with unsaved changes, the dialog stays open with a spinner until
Wave receives the saved config or the request fails. The editor
rejects duplicate JSON object keys and NUL characters because WaterWall
rejects them. Add and configure the nodes needed for each chain before enabling
the config. WaterWall checks each node's settings and chain compatibility when
it starts.

Saving an **AuthenticationServer** adds one users tab with the node's name.
While WaterWall is stopped, Wave uses the private JSON file under
`configs/<name>/` and validates the current file each time an operator opens the
roster, including when another operator already has it open. **Refresh from
disk** picks up external edits while the tab is open. Saved rosters have no timed
reads. Committed Wave edits also update active viewers of that file. Wave sets the node's
`db-path` and keeps the same file when the node is renamed. When WaterWall
runs the selected config, the same tab uses the local API. While at least one
operator views a live roster, Wave shares one revision check for the node
every 500 ms. Browsers receive changed summaries and removed IDs after their
initial load. Starting and stopping pause account access. If the selected config is
inactive or the API is unavailable, the tab shows the reason and disk access
resumes after WaterWall stops.

During a refresh or account request, the spinner appears at the right end of
the summary above the Users table. The rows stay in place. Successful account
changes appear in **Logs**.

**Search** finds an exact decimal ID or a case-insensitive substring of the
displayed name. Surrounding whitespace is ignored, and spaces inside names
are significant. Exact-ID lookup also ignores surrounding whitespace and
accepts leading zeroes. Both sources use Unicode case folding and preserve
accent distinctions. Search, column sorting and scrolling work over the full roster in
the browser. WaterWall 1.6.0 list summaries show at most 256 UTF-8 bytes of a
name. Search uses that displayed preview. **Options** can open the full name
with the `get-user` grant. When only exact-ID lookup is available or the
live list is unavailable, **Find account** opens that account's details
directly. A `get-user` grant is enough to view it. Save or cancel pending
account edits before using **Change password** when that action is granted.
Add `list-users` to enable browsing and name search.

**Traffic usage** shows total charged upload and download combined in decimal
units, where 1 MB is 1,000,000 bytes and 1 GB is 1,000 MB. It can be sorted
locally, including for large imported accounts. A missing counter makes the
total unavailable. To clear one user's usage, stop WaterWall, open
**Options → Reset traffic** on that user's row, and confirm. Wave sets both
charged counters to zero. The account keeps its configured quota and expiry.
WaterWall 1.6.0 has no live API operation for this reset.

**Avg upload speed** and **Avg download speed** show estimated charged bytes
per second. Each live tab times two consecutive changes to an account's
counters, then divides the bytes added in each direction by that interval.
Estimates can vary slightly across viewers. For example, 6 MB of new upload
over 60 seconds gives 100.0 kB/s. WaterWall batches usage and can apply quota
multipliers.
Accounts with longer reporting intervals can take several minutes to show
their first estimate. Hover over a speed to see its interval and the age of
the last update. Both columns
sort locally. The last estimate stays visible between reports and clears if
the roster goes stale or the counters reset. Speeds are unavailable offline.

**Days Left** shows the earlier active
absolute or first-use deadline, with separate displays for expiry, unlimited
duration, first-use timers and unavailable values. Saved first-use durations
that have started show as unavailable while their accounting clock is offline.
Enable the optional **Group** column to see assigned groups. Unassigned rows
show **Default**. Measured network speeds require additional WaterWall
telemetry. [Per-user rate proposal](waterwall-user-rates.md)

Leave **User ID** blank to start at `1` in an empty database and then use the
highest existing ID plus one. The form shows the next ID when it has a current
account list. Search and sort have no effect on this suggestion. In offline
mode Wave assigns the ID from the saved file when it writes the account.
Custom schematic ID ranges limit the sequence. If the highest ID has reached
the upper limit, enter an unused ID within the range.

A live panel with create permission alone picks a random ID within the allowed
range. If a permitted list is still loading or stale, wait for it to refresh
or enter an ID yourself. If another admin takes the suggested ID first, Wave
refreshes the list. Review the new suggestion, re-enter the password, and
submit again. A manually entered ID stays in the form until you change it.

For a live create, the browser knows the exact ID it sent.
An open browser tab retains the ID of an unconfirmed
create when the connection drops. Reconnecting to the same sign-in restores
the ID. The tab's local **Logs** also record the account ID, config and node for
confirmed or unconfirmed creates. If Wave loses the database identity during
a create, it closes the draft. Check the account ID in the original database
using the config and node recorded in Logs. A later acknowledgement can still
confirm the original write. Reloading the page or
starting a new sign-in clears these browser-local receipts.

Use **Options** to fetch the account and open its access, expiry, activity and limit settings.
Existing IDs, creation and first-use times, and recorded statistics are shown for
reference. The inline switch fills the fixed-width **Enable** column when the
schematic defines the control and the source permits editing.

The editor shows traffic quotas in decimal GB and uses the roster's decimal
units for recorded traffic and estimated speeds.
Enter up to three decimal places for quotas. `10.531` GB is 10 GB and 531 MB.
WaterWall stores the charged byte count. Saved values with finer precision
retain their exact byte count until the field is changed. Bandwidth limits
use bytes per second. Zero or blank limits mean unlimited. Absolute expiry
uses Unix epoch milliseconds, and first-use expiry uses a duration in
milliseconds. Clearing a numeric override restores WaterWall's default,
including the 120,000 ms statistics record interval. A WireGuard account can
have one IPv4 or IPv6 CIDR.

| Action | WaterWall stopped | WaterWall running the selected config |
| --- | --- | --- |
| Browse, search, sort and monitor users | Saved roster and charged totals | `list-users` gives the roster and current charged totals. `status`, when granted, can accelerate revision checks. |
| Database status | — | `status` gives save, checkpoint, user count, and audit health |
| Account details and exact-ID lookup | Saved account data | `get-user` gives redacted account data |
| Create an account | Available | `create-user` |
| Edit account fields or toggle Enable | Available | `get-user` and `update-user` |
| Change password | Available in the account editor | Separate **Change password** action with `get-user` and `change-user-password` |
| Show saved password | Individual password read | — |
| Reset traffic usage | Available with confirmation | — |
| Delete an account | Available with confirmation | — |

A successful live write updates the running core. WaterWall saves changes to
disk on its normal save cycle and can keep recovery backups in the same folder.
If a newer account edit conflicts with a draft, review the latest values
before trying again. If a live write loses its acknowledgement, use the
original account ID to inspect the result before taking further action.
If a live read fails temporarily, Wave marks the displayed rows stale. When
access is lost, Wave clears the list. Use **Retry** once access returns. An
open editor for the same verified account can review fresh details and
resume with independently granted edit or password access. When there are multiple
eligible local listeners, Wave can try another after a transport failure. A
write can move to another listener only when its complete request could not
have reached WaterWall. Once Wave sends the complete write, it waits for its
acknowledgement for the rest of the two-second request deadline.

Wave reads private databases up to 16 MiB. Deleting a node or removing a
config preserves these database files. To manage an existing database, put it
in the config folder and set the node's `db-path` to that file. Each server
needs `auth-clients` settings to run. [WaterWall 1.6.0 AuthenticationServer](https://github.com/radkesvat/WaterWall/blob/v1.6.0/tunnels/AuthenticationServer/description.md)
If a saved node is reassigned to a different database, reopen the account in
its tab before editing or showing its password.
If Wave temporarily cannot read a saved config or its `nodes` field is missing
or is the wrong type, it keeps the Users tabs and nonsecret drafts. Account
actions pause until the config can be verified. A valid empty `nodes` array
removes the tabs.
Restore the config and select **Retry** to validate the binding and file again.

Use a dedicated API port on `127.0.0.1` or `::1`. For a listener with multiple
explicit ports or a socket-backed range, Wave checks which ports are dedicated
to the API. Wave also reaches wildcard listeners through loopback. In the APIServer's
`settings.clients`, set a client name, a 64-character hexadecimal token, and
an `allow` entry with the config's top-level `name`, the exact
AuthenticationServer node name, and the operations from the table above.
For example, `"operations": ["list-users", "get-user", "update-user"]` lets
the client monitor and edit accounts. Add each operation explicitly to give
the client additional access. Save the config and restart WaterWall to apply
the new grants. Wave verifies the local TCP peer belongs to its WaterWall
process before sending credentials. [WaterWall 1.6.0 APIServer](https://github.com/radkesvat/WaterWall/blob/v1.6.0/tunnels/APIServer/description.md)

### Workspace backup and restore

Stop WaterWall before using **Backup** or **Restore** in Panel Controls.
**Backup** downloads `wave-backup.zip`. Its `manifest.json` records the backup
format version, the source `wave_version`, the selected `waterwall_version`,
saved core settings, and the registered config IDs, names, and enabled flags.
Version 2 also records `panel_settings` with the base path, listener address and
port, TLS settings and PEM paths, session expiry, theme, donation answer and UTC
timestamp, and the administrator username and Argon2id password hash. The ZIP
includes each config's
`config.json`, referenced AuthenticationServer databases, and their `.backup`
recovery files when present. Wave preserves the saved bytes of a primary and
recovery file when WaterWall can load the primary or recover from the backup.
The export captures the last state saved to disk and excludes Wave session
tokens, session records, the JWT signing key, and TLS certificate and key files.

**Restore** validates the ZIP and shows the WaterWall version and the configs
that will be added, replaced, or removed from the workspace. The destination
must have a valid schematic for the recorded release. Wave reports a missing
executable or known external file dependencies, including databases required
by Router rules, before confirmation. It preserves disabled drafts and warns
when the selected release lacks one of their node types. Supply host-specific
files separately. Restore preview shows the restored administrator and base
path, and reports TLS problems to resolve before restarting. ZIPs contain the
Wave administrator's password hash and may contain WaterWall credentials, so
store them privately and use HTTPS for remote access.
[Authentication and HTTPS](authentication.md)

Confirm to apply the restored configuration files, core settings, and selected
WaterWall release together with any included panel settings. A version 2 restore
replaces the administrator credentials, signs out existing sessions, generates
a fresh JWT signing key, and switches to the restored base path. Sign in there
with the administrator credentials from the backup. The listener address, port,
and TLS changes take effect after Wave restarts. A version 1 backup restores the
WaterWall workspace and keeps the destination panel settings and sessions.
Wave keeps unrelated files in the config folders, including
the files of configs removed from the workspace registry. Wave recovers an
interrupted restore at startup before opening the workspace. Keep the config
filesystems mounted until recovery finishes. Start WaterWall from Panel
Controls when the restored workspace is ready.

You can cancel while uploading a ZIP. If a release check delays Apply, retry
after it finishes. If the Apply response times out, check Server Status and
Logs for the outcome before attempting another restore.

The backend accepts ZIPs up to 128 MiB compressed and 128 MiB expanded, with
at most 4096 files. Config files are limited to 256 KiB and user databases
to 16 MiB each. The authenticated HTTP routes relative to the panel base path
are `GET /api/backup`, `POST /api/backup/prepare` with a ZIP body, and
`POST /api/backup/apply` with the proposal token as JSON. POST requests use
the panel's origin and CSRF checks. A proposal expires after five minutes.
Apply returns HTTP 409 with `retryable: true` while a valid proposal is waiting
for a core operation to finish.

The workspace snapshot reports the Wave build version in `server.version`.
`server.checking_updates` stays true until both parts of a combined check finish.
The `server.panel` object reports `checking_updates` and an
`update_check_generation` that advances after each completed manual lookup.
It reports the display version of the latest known Wave tag in `latest_version`
and whether that tag is newer than the running version in `update_available`.
A failed check adds `update_check_error` and keeps the last known version. A
successful lookup with no tags removes `latest_version`.
The `server.core` object reports the selected WaterWall `version`, `state`,
`available_versions` from the local cache, its highest `latest_version`, and
whether it is newer than the selected version in `update_available`.
It also reports `checking_updates` and any `update_check_error`. While Wave
installs another version, `update_target` identifies it. A failed update reports
`update_error`. The object also reports
any installer or process `error`, `configs`, and the `lifecycle` and `configs`
capability flags.

Workspace snapshots include a top-level `schematic_revision`. The first
snapshot on a connection and the first snapshot after a switch also carry
the decoded `schematic`. Requests that depend on the active WaterWall schematic
must include its current `schematic_revision`, even if the connection has
already received the schematic. Older clients can omit the revision while
Wave is on its initial bundled 1.6.0 selection before the first switch.
Selecting another release at startup also requires the revision. Requests
to enable, disable, or delete configs also require the current revision after
a workspace restore, which can reuse config IDs. Workspace actions use
the existing `request_id` and snapshot reply.
The opt-in Users roster stream uses separate messages. [Workspace roster protocol](authentication.md#workspace-roster-stream)
Checks, installs, and other operations that continue in the background
acknowledge receipt and report completion in later snapshots. Stop and restart
dialogs keep the spinner visible while WaterWall is changing state.
Installation and process outcomes appear in **Logs**.

When a reply includes `feedback_in_logs_only: true`, its message has been written
to Logs. The reply still carries `feedback` and any `result` data. Workspace
changes that time out or lose their connection add a browser-local log entry.
While a reply is pending, the controls block duplicate submissions. If the
connection closes, Server Status shows the last snapshot and offers to reconnect.

| Action | Additional request fields |
| --- | --- |
| `check_updates` | None. Starts or joins checks for both Wave and WaterWall. |
| `install_core`, `start_core`, `stop_core`, `restart_core` | None |
| `check_panel_updates`, `check_core_updates` | None. Checks one project. |
| `update_core` | Cached WaterWall `version` to select |
| `apply_core_settings` | Overrides supported by the selected schematic as `settings`, up to 16 KiB |
| `apply_theme` | `theme` set to `default`, `light`, `classic`, `dark`, or `darker` |
| `apply_donation` | `answer` set to the string `no`, `1`, `3`, `5`, `10`, `15`, `30`, or `50_plus`. Older clients can still send `below_5` or `more_than_5`. The snapshot reports `answer`, `answered_at` (UTC), and `reminder_due` in `data.settings.donation`. `next_reminder_at` (UTC) is present when a donation reminder is scheduled and still upcoming. |
| `create_core_config` | `name` and optional boolean `add_auth_nodes`. Setting it to `true` provisions the starter chains with full scoped user management. Older clients can explicitly send `allow_live_user_management: false` to retain the template's original grants. |
| `get_core_config` | Config `id`. The result has `config`, `revision`, and `editable`. |
| `save_core_config` | Config `id`, edited `config` object, and last loaded `revision` |
| `set_core_config_enabled` | Config `id` and boolean `enabled` |
| `remove_core_config` | Config `id` |
| `get_config_users` | Offline scope, optional zero-based `page`, and optional `search` string |
| `get_config_user` | Offline scope and decimal string `user_id`. Returns the redacted editable account. |
| `get_config_user_password` | Offline scope and decimal string `user_id`. The offline edit read returns `result.password` to the requesting administrator. |
| `get_live_config_users` | `config_id`, AuthServer `node` name, and optional opaque `cursor` from the previous live page |
| `get_live_config_user_status` | Live scope. Returns the public database status. |
| `get_live_config_user` | Live scope and decimal string `user_id`. Returns a redacted `user`, `if_match` edit token, `accounting_time_ms` and `host_time_ms` samples. |
| `create_live_config_user` | Live scope, `user` object with an initial `password`, and a required canonical nonzero unsigned 64-bit decimal string in `user.id` or `user_id`. Both values must agree when both are supplied. |
| `edit_live_config_user` | Live scope, `user_id`, original `if_match`, and changed fields in `user` |
| `change_live_config_user_password` | Live scope, `user_id`, original `if_match`, and the new `password` |
| `create_config_user` | Offline scope, `user` object with `password` and optional decimal string `id`, and optional `page` or `roster: true` |
| `edit_config_user` | Offline scope, string `user_id`, changed fields in `user`, and optional `page` or `roster: true`. Set a numeric override to `null` to restore its default. |
| `reset_config_user_traffic` | Current offline scope, decimal string `user_id`, and optional `page` or `roster: true`. Sets the saved charged upload and download counters to zero. |
| `delete_config_user` | Offline scope, string `user_id`, and optional `page` or `roster: true` |
| `restore_preview_backup` | Legacy preview metadata JSON object as `backup` |

Live scope consists of `config_id`, `node`, `source` set to `live`, and the
node's `run_id` from `users_access` on the config snapshot. The legacy paged
action remains available to existing clients.

Offline scope consists of `config_id`, `node`, `source` set to `offline`, and the
node's `run_id` from the current offline snapshot. The opaque scope includes the
generation of the node's assigned database. The browser tracks the opaque
`database_generation` across starts and stops and asks the operator to reopen
the account when the assignment changes. The reply to a stale request carries
the refreshed snapshot. Legacy paged account actions also accept just
`config_id` and `node` and use the current database mapping at dispatch.

Saved mutation replies with `roster: true` return the committed `user_id`
in place of a page. The roster stream delivers the resulting row change
independently of the action acknowledgement.

### Backend tests

The backend smoke tests use Node.js 22 or later, a temporary site fixture,
and an OS-assigned loopback port. They can run without an Emscripten build
or npm dependencies. From the Wave root, run the test preset that matches the
build.

```sh
ctest --preset backend-debug
ctest --preset backend-release
```

To exercise all six live user operations against the actual core, supply a
WaterWall 1.6.0 executable. The test creates private config and database
files in a temporary deployment, uses loopback for the API and usage producer,
and removes the deployment when it finishes.

```sh
WAVE_BACKEND="$PWD/build/backend-release/wave-backend" \
WAVE_WATERWALL_V160="$PWD/build/backend-release/waterwall_bin/1.6.0/Waterwall" \
node --test tests/backend/users-real-core.test.cjs
```

[Users roster validation and measurements](users-roster-validation.md)
