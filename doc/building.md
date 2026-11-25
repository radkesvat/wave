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

From the Wave root in that shell, run the following commands.

```sh
cmake --preset web-debug
cmake --build --preset web-debug
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/web-debug/site
```

When switching SDK versions, use `cmake --fresh --preset web-debug` for the
configure step to reset CMake's cached compiler and toolchain.

Open [the local Wave preview](http://127.0.0.1:8000/) and check that Overview
opens with the disconnected status, sample metrics, and download chart.

The preset writes `index.html`, `index.js`, `index.wasm`, and third-party license
notices under `build/web-debug/site/`. Serve that directory over HTTP. To see
the compile and link commands, run `cmake --build --preset web-debug --verbose`.

## Browser smoke test

With the preview server running, use Node.js 22 or later and npm to run the
optional browser smoke test. Playwright installs its headless Chromium and
keeps the local test tooling under `build/`.

```sh
npm install --prefix build/browser-tests playwright-core@1.65.1
PLAYWRIGHT_BROWSERS_PATH="$PWD/build/browser-tests/browsers" \
  build/browser-tests/node_modules/.bin/playwright-core install --only-shell chromium
PLAYWRIGHT_BROWSERS_PATH="$PWD/build/browser-tests/browsers" \
  NODE_PATH="$PWD/build/browser-tests/node_modules" node --test tests/overview.test.cjs
```
