#include "browser_ui.h"

#include <emscripten.h>
#include <imgui.h>

#include <algorithm>

namespace wave {
namespace {

// clang-format off
EM_JS(float, visible_top, (), {
    return (window.visualViewport?.offsetTop ?? 0) - Module['canvas'].getBoundingClientRect().top;
});

EM_JS(float, visible_height, (), { return window.visualViewport?.height ?? window.innerHeight; });

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

void fit_preview_dialog(float width, float max_height)
{
    const float available_width = std::max(1.0f, ImGui::GetIO().DisplaySize.x - 24.0f);
    const float height = visible_height();
    const float available_height = std::max(1.0f, height - 24.0f);
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x * 0.5f, visible_top() + height * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(width, available_width), 0.0f});
    ImGui::SetNextWindowSizeConstraints({1.0f, 1.0f},
                                        {available_width, std::min(max_height, available_height)});
}

void download_preview_file(const char* filename, std::string_view contents, const char* media_type)
{
    download_file(filename, contents.data(), static_cast<int>(contents.size()), media_type);
}

} // namespace wave
