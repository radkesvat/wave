#pragma once

#include <string_view>

namespace wave {

struct CanvasViewport {
    float left;
    float top;
    float width;
    float height;
};

CanvasViewport visible_canvas();
void reveal_canvas_item(float top, float bottom);
void download_preview_file(const char* filename, std::string_view contents, const char* media_type);

} // namespace wave
