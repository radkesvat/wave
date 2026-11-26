# Build and preview Wave

Wave builds a WebAssembly panel and a native server that supplies its preview
data. Use a browser with WebGL 2 enabled. The workspace starts on Users and
shows WaterWall as disconnected.

## Prerequisites

- CMake 3.24 or newer and Ninja on your `PATH`
- Git and Python 3 for the Emscripten SDK setup
- Emscripten SDK 7.0.1
- Network access on the first build to fetch Dear ImGui v1.93.0,
  nlohmann/json v3.12.0, Project Nayuki QR Code generator v1.8.0, and the SDK's
  SDL2 port. [QR release](https://github.com/nayuki/QR-Code-generator/releases/tag/v1.8.0)

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
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/web-debug/site
```

### Release

```sh
cmake --preset web-release
cmake --build --preset web-release
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/web-release/site
```

The release preset uses `-O3 -DNDEBUG` and enables link-time optimization
with `-flto` during compilation and linking. It generates a browser-only
JavaScript loader. [Emscripten optimization guide](https://emscripten.org/docs/optimizing/Optimizing-Code.html#lto)

The static preview shows the workspace and its unavailable-data state. Use
the native preview below for populated views and demo actions. [Static preview](http://127.0.0.1:8000/)

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

The backend needs CMake 3.24 or newer, Ninja, and a host C++20 compiler. The
first configure downloads cpp-httplib v0.67.0 and nlohmann/json v3.12.0 and
verifies their pinned archive checksums. [cpp-httplib release](https://github.com/yhirose/cpp-httplib/releases/tag/v0.67.0), [nlohmann/json release](https://github.com/nlohmann/json/releases/tag/v3.12.0)

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
[the local Wave preview](http://127.0.0.1:2030/).

```sh
./build/backend-release/wave-backend
```

Use the Users toolbar to search, sort, create, or pause sample records. Open
a row's Options for edits, notes, QR sharing, and simulated bot actions. The
Logs pane records demo changes and provides search, auto-scroll, copy, and
clear controls. Menu opens version information, demo sign-in, first-run setup,
and demo sign-out. Tools opens diagnostics and input helpers.

Preview dialogs share a square-cornered frame, a dark title strip, and a
right-aligned action row below the body. Workspace dialogs center in the
visible viewport, including when the page is scrolled. Entry dialogs center
beneath Wave's header when there is room. In very short viewports, the
dialog body and actions scroll together. At larger heights, the actions
stay visible while a long body scrolls. Action rows wrap on narrow screens.

### First-run setup

Wave opens Users on a normal page load. Choose **Menu → First-run setup**
to start with a fresh draft and see the Welcome strip beneath the header.
Continue from **Welcome to Wave** through **Preview notes**, then enter a
panel name, demo administrator name, and masked demo password in **Panel
details**. **Use example** fills the initial demo credentials, `demo` and
`wave-demo`. Select **Back** to review a step without losing the draft, or
**Back to workspace** to leave the walkthrough.

**Finish setup** saves the panel name and demo credentials for the current
preview process. The dialog shows the request status and keeps the draft
available after a rejection, timeout, or disconnect. When the server
confirms, select **Open workspace**. Use your saved credentials from
**Menu → Demo sign-in**. Set the domain and the paths to the certificate and
private key in **Panel Settings**.

### Preview controls and server options

Tab goes from **Menu** and **Tools** into the workspace. Press Enter on a
focused pane, then use Tab to move between its controls.

The Users table switches to a paginated layout when the work pane lacks room
for the desktop columns and names. Drag a desktop column divider to resize
it for the current session. The enabled switch follows the latest preview
state. Successful user changes appear in Logs while the Users table stays
in place. Swipe the panel to reach Logs. Swiping
along a pane's right edge scrolls its contents. Drag directly on a topology
map to pan it, or tap a node or link to inspect it.

Wave's scrollable panes use inset, rounded scrollbar thumbs and share the
same track, hover, and drag colors in each contrast mode. Classic page
scrollbars use those colors and rounded ends where browser styling is
supported. Overlay scrollbars use the browser's native appearance.

Server Status shows CPU and Memory above Server Information and Traffic
when the work pane can fit two columns. When space is tight, all four cards
stack in that order. On a phone, swipe through the cards to reach Logs.
At short desktop heights, scroll inside the work pane to see the rest of
the grid. Server Status reserves the browser scrollbar gutter on platforms
with a classic scrollbar, so the column count stays consistent as the page
starts or stops scrolling.

`--site-dir` selects a frontend build directory relative to the current
working directory or by absolute path. It defaults to `build/web-release/site`
and must contain `index.html`, `index.js`, and `index.wasm`. `--host` and
`--port` default to `127.0.0.1` and `2030`. Pass `--port 0` to let the operating
system choose a free port. The server prints its address and bound port at
startup. The `/ws` endpoint echoes text messages over the same port.

The workspace opens `/ws/preview` on the same host and port. A JSON frame with
`type` set to `snapshot` carries Users, Logs, server metrics, settings,
topology, bot status, and console data in `data`. Requests specify an `action`
and its fields. The server replies with a snapshot and action feedback.
Forms that wait for confirmation send a unique numeric `request_id`. The
server echoes it to the requesting browser, and the form completes only
from the reply carrying its ID. Browsers still apply valid snapshot data
when a reply arrives after the form has been closed or replaced. If the
socket closes between render frames, the form can still read a reply it
already received. A form still waiting for its reply reports the disconnect.

First-run setup sends `action: "setup"` with `request_id`, `name`, `username`,
and `password`. The server validates the name and both credentials before
updating the panel name and the in-memory demo account. Existing domain,
certificate, and private key settings are preserved. The preview snapshots
omit the password.

User edits send the user ID and changed fields. Connection and limit options
are updated individually so changes from other browsers to untouched fields
are preserved.

Both Telegram Bot forms use `apply_bot_settings`. Settings sends
`notifications`, `allow_status`, `allow_config`, `notification_rules`, and
`permissions`. Overrides sends `overrides` alone. The server merges the
submitted fields so saves from the two forms can arrive in either order.

Other connected browsers receive the shared updates. Overview and Server
Status use the same server metrics. Server Status plots CPU on a 0–100%
scale, memory usage against capacity in GiB, and upload and download
together in MiB/s. The charts keep up to 19 samples in time order. The
preview adds a sample about every five seconds and draws the latest one
at **Now**. Hover over a plot for its sample values. User changes that affect
traffic can update the latest rates between samples. Restarting the
process restores the sample data. The console, credentials, and bot actions
run as in-memory demonstrations.

The standalone `/ws/overview` endpoint sends one JSON text frame and closes
the connection. Its fixed fixture contains `cpu_percent`, `memory_used_gib`,
`memory_total_gib`, `uptime_minutes`, and `download_mib_per_second`. The last
field contains 13 samples taken five minutes apart, oldest first.

For a remote preview, bind to `0.0.0.0` and open `http://<server-ip>:2030/`
from the other machine.

```sh
./build/backend-release/wave-backend \
  --site-dir build/web-release/site --host 0.0.0.0 --port 2030
```

### Backend tests

The backend smoke tests use Node.js 22 or later, a temporary site fixture,
and an OS-assigned loopback port. They can run without an Emscripten build
or npm dependencies. From the Wave root, run the test preset that matches the
build.

```sh
ctest --preset backend-debug
ctest --preset backend-release
```
