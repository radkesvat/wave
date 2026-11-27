#pragma once

#include <cstddef>
#include <optional>
#include <string>
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
void choose_preview_json_file(std::size_t max_bytes);
std::optional<std::string> take_preview_json_file();
void clear_preview_json_file();
void show_config_json_editor(const std::string& initial_text, bool read_only, CanvasViewport bounds,
                             CanvasViewport clip);
bool config_json_editor_ready();
std::optional<std::string> read_config_json_editor_text();
void close_config_json_editor();
void finish_config_json_editor_frame();

} // namespace wave
