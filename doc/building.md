# Build and preview Wave

Wave builds a static WebAssembly Overview page and a native preview server.
Use a browser with WebGL 2 enabled to preview the Overview.

## Prerequisites

- CMake 3.24 or newer and Ninja on your `PATH`
- Git and Python 3 for the Emscripten SDK setup
- Emscripten SDK 7.0.1
- Network access on the first build to fetch Dear ImGui v1.92.5 and the SDK's
  SDL2 port

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

Open [the local Wave preview](http://127.0.0.1:8000/) and check that Overview
opens with the disconnected status, sample metrics, and download chart.

Each preset writes `index.html`, `index.js`, `index.wasm`, and third-party
license notices to its own `site/` directory. Add `--verbose` to a build command
to show the compile and link commands. When switching SDK versions, add
`--fresh` to the configure command to reset CMake's cached compiler and toolchain.

## Frontend tests

The browser smoke tests live in `tests/frontend/`. Use Node.js 22 or later
and npm to install the pinned test dependencies and headless Chromium.

```sh
npm ci --prefix tests/frontend
npm run --prefix tests/frontend install-browser
```

After building a preset, use the matching CTest command. Each run serves its
built site on a separate local port and tests the Overview in Chromium.

```sh
ctest --preset web-debug
ctest --preset web-release
```

The **Frontend** GitHub Actions workflow builds and tests both presets on
pushes to `main` and on pull requests. Each successful job uploads its
generated site as a separate artifact.

## Native backend

The backend needs CMake 3.24 or newer, Ninja, and a host C++20 compiler. The
first configure downloads cpp-httplib v0.67.0 and verifies the archive against
the checksum pinned in the native CMake project. [cpp-httplib release](https://github.com/yhirose/cpp-httplib/releases/tag/v0.67.0)

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

`--site-dir` selects a frontend build directory relative to the current
working directory or by absolute path. It defaults to `build/web-release/site`
and must contain `index.html`, `index.js`, and `index.wasm`. `--host` and
`--port` default to `127.0.0.1` and `2030`. Pass `--port 0` to let the operating
system choose a free port. The server prints its address and bound port at
startup. The `/ws` endpoint echoes text messages over the same port.

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
