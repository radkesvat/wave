#include "browser_ui.h"

#include <emscripten.h>

#include <cstdlib>

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

EM_JS(void, reveal_canvas_item_in_document, (float top, float bottom), {
    const canvas = Module['canvas'].getBoundingClientRect();
    const viewport = window.visualViewport;
    const viewportTop = viewport?.offsetTop ?? 0;
    const viewportHeight = viewport?.height ?? innerHeight;
    if (canvas.height <= viewportHeight) return;
    const itemTop = canvas.top + top - viewportTop;
    const itemBottom = canvas.top + bottom - viewportTop;
    if (itemTop < 0) window.scrollBy(0, itemTop - 8);
    else if (itemBottom > viewportHeight) window.scrollBy(0, itemBottom - viewportHeight + 8);
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

EM_JS(void, choose_json_file, (int maxBytes), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json,application/json';
    const selection = {};
    Module['previewJsonSelection'] = selection;
    input.addEventListener('change', async () => {
        const file = input.files[0];
        if (!file) return;
        if (file.size > maxBytes) {
            selection.text = "";
            return;
        }
        try {
            const content = new TextDecoder('utf-8', { fatal: true }).decode(await file.arrayBuffer());
            selection.text = content.includes(String.fromCharCode(0)) ? "" : content;
        } catch {
            selection.text = "";
        }
    }, { once: true });
    input.click();
});

EM_JS_DEPS(take_json_file, "$stringToNewUTF8");
EM_JS(char*, take_json_file, (), {
    const selection = Module['previewJsonSelection'];
    if (selection?.text === undefined) return 0;
    delete Module['previewJsonSelection'];
    return stringToNewUTF8(selection.text);
});

EM_JS(void, clear_json_file, (), { delete Module['previewJsonSelection']; });
// clang-format on

} // namespace

CanvasViewport visible_canvas()
{
    CanvasViewport viewport{};
    read_visible_canvas(&viewport.left, &viewport.top, &viewport.width, &viewport.height);
    return viewport;
}

void reveal_canvas_item(float top, float bottom)
{
    reveal_canvas_item_in_document(top, bottom);
}

void download_preview_file(const char* filename, std::string_view contents, const char* media_type)
{
    download_file(filename, contents.data(), static_cast<int>(contents.size()), media_type);
}

void choose_preview_json_file(std::size_t max_bytes)
{
    choose_json_file(static_cast<int>(max_bytes));
}

std::optional<std::string> take_preview_json_file()
{
    auto* text = take_json_file();
    if (!text) {
        return std::nullopt;
    }
    std::string result(text);
    std::free(text);
    return result;
}

void clear_preview_json_file()
{
    clear_json_file();
}

} // namespace wave
