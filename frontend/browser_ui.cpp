#include "browser_ui.h"

#include <emscripten.h>

namespace wave {
namespace {

// clang-format off
EM_JS(void, read_visible_canvas, (float* left, float* top, float* width, float* height), {
    const canvas = Module['canvas'].getBoundingClientRect();
    const viewport = window.visualViewport;
    const x = viewport?.offsetLeft ?? 0;
    const y = viewport?.offsetTop ?? 0;
    HEAPF32[left >> 2] = Math.max(0, x - canvas.left);
    HEAPF32[top >> 2] = Math.max(0, y - canvas.top);
    HEAPF32[width >> 2] = Math.max(0, Math.min(x + (viewport?.width ?? innerWidth), canvas.right) -
        Math.max(x, canvas.left));
    HEAPF32[height >> 2] = Math.max(0, Math.min(y + (viewport?.height ?? innerHeight), canvas.bottom) -
        Math.max(y, canvas.top));
});

EM_JS(void, download_file, (const char* filename, const char* contents, int length, const char* media_type), {
    const file = new Blob([UTF8ToString(contents, length)], { type: UTF8ToString(media_type) });
    const url = URL.createObjectURL(file);
    const link = document.createElement('a');
    link.href = url;
    link.download = UTF8ToString(filename);
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
});
// clang-format on

} // namespace

CanvasViewport visible_canvas()
{
    CanvasViewport viewport{};
    read_visible_canvas(&viewport.left, &viewport.top, &viewport.width, &viewport.height);
    return viewport;
}

void download_preview_file(const char* filename, std::string_view contents, const char* media_type)
{
    download_file(filename, contents.data(), static_cast<int>(contents.size()), media_type);
}

} // namespace wave
