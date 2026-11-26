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
void download_preview_file(const char* filename, std::string_view contents, const char* media_type);

} // namespace wave
