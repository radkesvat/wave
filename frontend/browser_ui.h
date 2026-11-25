#pragma once

#include <string_view>

namespace wave {

void fit_preview_dialog(float width, float max_height = 620.0f);
void download_preview_file(const char* filename, std::string_view contents, const char* media_type);

} // namespace wave
