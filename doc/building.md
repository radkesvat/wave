# Build and preview Wave

Wave builds a WebAssembly panel and a native server that supplies its preview
data. Use a browser with WebGL 2 enabled. After sign-in, the workspace starts
on Users and shows WaterWall as disconnected.

## Prerequisites

- CMake 3.24 or newer and Ninja on your `PATH`
- Git and Python 3 for the Emscripten SDK setup
- Emscripten SDK 7.0.1
- Network access on the first build to fetch Dear ImGui v1.93.0, ImPlot v1.1,
  nlohmann/json v3.12.0, Project Nayuki QR Code generator v1.8.0, and the SDK's
  SDL2 port. [ImPlot release](https://github.com/epezent/implot/releases/tag/v1.1) [QR release](https://github.com/nayuki/QR-Code-generator/releases/tag/v1.8.0)

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

Each preset writes `index.html`, `index.js`, `index.wasm`, and third-party
license notices to its own `site/` directory. Add `--verbose` to a build command
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
workspace in Chromium.

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

Linux builds include HTTPS support by default. Use `-DWAVE_ENABLE_AUTH=OFF`
to build without HTTPS support. HTTP still requires administrator sign-in.

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

Use Users to search, sort, and create sample records or pause live data.
Open a row's **Options** to edit the user, read notes, share a configuration
by QR code, reset traffic totals, or delete the user. The Logs pane records
demo changes and provides search, auto-scroll, copy, and clear controls.
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

Use **Panel Settings → Administrator** to change the username or password.
Saving changes signs out every active session. **Panel Settings** also shows
the sample domain, certificate chain path, and private key path.
**Apply changes** keeps those sample values for the current preview process.
Configure deployment TLS in `wave-db.json`. [Administrator setup](authentication.md#set-up-the-administrator)

### Preview controls and server options

The sidebar contains **Server Status**, **Users**, **Panel Settings**,
**Console**, **Nodes & Routers**, and **About**.
Tab goes from **Menu** and **Tools** into the workspace. Press Enter on a
focused pane, then use Tab to move between its controls.

The Users table switches to a paginated layout when the work pane lacks room
for the desktop columns and names. Drag a desktop column divider to resize
it for the current session. The enabled switch follows the latest preview
state. Successful user changes appear in Logs while the Users table stays
in place. Swipe the panel to reach Logs. Swiping along a pane's right edge
scrolls its contents.

**Nodes & Routers** shows the sample topology on one canvas. Drag to pan,
use the zoom buttons and **Fit map** to adjust the view, and select a node or
connection to see its details. **Inspect items** also lists the routes. Use
**Add node** to place a named sample node. Select a node and use **Delete node**
to confirm removal of the node and its connections and routes. On narrow
screens, swipe outside the canvas to reach Logs. Changes are shared in the
preview and reset when the preview process restarts.

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
frame with `type` set to `snapshot` carries Users, Logs, server metrics, settings,
topology, bot status, and console data in `data`. Requests specify an `action`
and its fields. The server replies with a snapshot and action feedback.
Forms that wait for confirmation send a unique numeric `request_id`. The
server echoes it to the requesting browser, and the form completes only
from the reply carrying its ID. Browsers still apply valid snapshot data
when a reply arrives after the form has been closed or replaced. If the
socket closes between render frames, the form can still read a reply it
already received. A form still waiting for its reply reports the disconnect.

Send one or more of `domain`, `certificate_path`, and `private_key_path`
in the `settings` object of an `apply_settings` request.

User edits send the user ID and changed fields. Connection and limit options
are updated individually so changes from other browsers to untouched fields
are preserved.

Topology edits use `add_node` with a `name` or `delete_node` with a node `id`.
The preview accepts up to 64 nodes. Names are limited to 64 UTF-8 bytes and
must be nonblank, unique after trimming surrounding spaces, and free of
ASCII control characters. The server assigns the new node's ID and position.
Deleting a node removes links with that endpoint and routes containing it.

The backend preview API merges the fields submitted with `apply_bot_settings`
into `bot`. Requests can update `notifications`, `allow_status`,
`allow_config`, `notification_rules`, and `permissions` independently of
`overrides`.

Other connected browsers receive the shared preview updates. Simulated user
traffic updates about every five seconds. Bot actions run in memory.

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

### Panel Controls and versioning

Each Wave panel release uses the same version number as the WaterWall core
release it supports. For example, Wave 1.6.0 would support WaterWall 1.6.0.

Each button and the target panel version picker fill their own row in Panel
Controls. The desktop grid uses shorter rows, and the stacked layout gives
touch controls more height. **Start core** is available while the preview
core is stopped. **Stop core** is available while it is running and requires
confirmation.
These actions change the simulated lifecycle state in memory. Live WaterWall
starts its executable with a core JSON input and loads the listed node configs
during startup. Live start and stop are unimplemented in Wave.

Pick a target Wave panel version from the preview's sample catalog. The
choice stays in the browser until you confirm **Update panel**. The preview
stores the reported panel version in memory and displays it in Server
Information, About, and the Wave version dialog.

The top-bar **Menu** provides **Create config** and **Manage configs** on
every preview workspace page. **Create config** saves an in-memory entry by
name with its enabled checkbox clear. Names are case-sensitive, can contain up
to 64 UTF-8 bytes, and must be unique after trimming surrounding spaces.
Blank names and ASCII control characters are invalid. **Manage configs** can
enable or disable an entry, or remove it after confirmation. The active entry is protected against both
changes, including while the preview core is stopped. Enabling an entry only
changes its preview flag. Live config-file writes, activation, and reload are
unimplemented. Config changes are shared by connected browsers and reset with
the preview backend.

**Backup** in Panel Controls downloads `wave-preview-backup.json` with the
preview TLS domain, PEM paths, and inactive config entries. **Restore** imports
that file and asks for confirmation before replacing those settings and entries
for all connected preview browsers. The active config stays protected. The
file contains config names and enabled flags, and restoration affects only
in-memory preview data. For deployment settings, see
[Authentication and HTTPS](authentication.md).

The preview snapshot reports the current panel version in `server.version`
and the panel catalog in `server.available_panel_versions`. The `server.core`
object reports `state`, `configs`, and the `lifecycle` and `configs` capability
flags. Requests use the existing `request_id` and snapshot reply.
While a reply is pending, the controls block duplicate submissions. If the
connection closes, Server Status shows the last snapshot and offers to reconnect.

| Action | Additional request fields |
| --- | --- |
| `start_core`, `stop_core` | None |
| `update_panel` | `version` from `server.available_panel_versions` |
| `create_core_config` | `name` |
| `set_core_config_enabled` | Config `id` and boolean `enabled` |
| `remove_core_config` | Config `id` |
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
