# Build and preview Wave

Wave builds a static WebAssembly Overview page. Use a browser with WebGL 2
enabled to preview it.

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
generated site as a separate artifact. `tests/backend/` is reserved for
backend tests.
