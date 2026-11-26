#include "console_view.h"

#include <imgui.h>

#include <algorithm>

namespace wave {

CanvasViewport render_console()
{
    const auto position = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(0.0f, available.x), std::max(0.0f, available.y)};
    ImGui::Dummy(size);
    return {position.x, position.y, size.x, size.y};
}

} // namespace wave
