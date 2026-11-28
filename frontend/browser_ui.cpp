#include "browser_ui.h"

#include <emscripten.h>

#include <cstdlib>

namespace wave {
namespace {

bool config_editor_open = false;
bool config_editor_drawn = false;
bool config_editor_ready = false;

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

EM_JS(void, start_backup_download, (), { Module['startBackupRequest']('download'); });

EM_JS(void, choose_backup_file, (), { Module['chooseWorkspaceBackup'](128 * 1024 * 1024); });

EM_JS(void, start_backup_apply, (const char* token), {
    Module['startBackupRequest']('apply', UTF8ToString(token));
});

EM_JS(bool, pending_backup_request, (const char* operation), {
    return Module['backupRequestPending'](operation ? UTF8ToString(operation) : undefined);
});

EM_JS_DEPS(take_backup_reply, "$stringToNewUTF8");
EM_JS(char*, take_backup_reply, (), {
    const reply = Module['takeWorkspaceBackupReply']();
    return reply === null ? 0 : stringToNewUTF8(reply);
});

EM_JS(void, clear_backup_request, (), { Module['clearWorkspaceBackupRequest'](); });

EM_JS(void, open_config_editor, (const char* text, bool read_only), {
    Module['openConfigJsonEditor'](UTF8ToString(text), !!read_only);
});

EM_JS(bool, position_config_editor,
      (float left, float top, float width, float height,
       float clip_left, float clip_top, float clip_right, float clip_bottom, bool read_only), {
    return Module['positionConfigJsonEditor'](left, top, width, height,
        clip_left, clip_top, clip_right, clip_bottom, !!read_only);
});

EM_JS_DEPS(read_config_editor, "$lengthBytesUTF8,$stringToNewUTF8");
EM_JS(char*, read_config_editor, (int* length), {
    const text = Module['configJsonEditorText']();
    if (text === null) return 0;
    HEAP32[length >> 2] = lengthBytesUTF8(text);
    return stringToNewUTF8(text);
});

EM_JS(void, close_config_editor, (), { Module['closeConfigJsonEditor'](); });
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

void download_workspace_backup()
{
    start_backup_download();
}

void choose_workspace_backup()
{
    choose_backup_file();
}

void apply_workspace_backup(const char* token)
{
    start_backup_apply(token);
}

bool backup_request_pending(const char* operation)
{
    return pending_backup_request(operation);
}

std::optional<std::string> take_workspace_backup_reply()
{
    auto* text = take_backup_reply();
    if (!text) {
        return std::nullopt;
    }
    std::string result(text);
    std::free(text);
    return result;
}

void clear_workspace_backup_request()
{
    clear_backup_request();
}

void show_config_json_editor(const std::string& initial_text, bool read_only, CanvasViewport bounds,
                             CanvasViewport clip)
{
    if (!config_editor_open) {
        open_config_editor(initial_text.c_str(), read_only);
        config_editor_open = true;
    }
    config_editor_drawn = true;
    config_editor_ready =
        position_config_editor(bounds.left, bounds.top, bounds.width, bounds.height, clip.left, clip.top,
                               clip.left + clip.width, clip.top + clip.height, read_only);
}

bool config_json_editor_ready()
{
    return config_editor_ready;
}

std::optional<std::string> read_config_json_editor_text()
{
    int length = 0;
    auto* text = read_config_editor(&length);
    if (!text) {
        return std::nullopt;
    }
    std::string result(text, length);
    std::free(text);
    return result;
}

void close_config_json_editor()
{
    if (config_editor_open) {
        close_config_editor();
        config_editor_open = false;
        config_editor_ready = false;
    }
}

void finish_config_json_editor_frame()
{
    if (!config_editor_drawn) {
        close_config_json_editor();
    }
    config_editor_drawn = false;
}

} // namespace wave
