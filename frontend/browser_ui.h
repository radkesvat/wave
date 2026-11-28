#pragma once

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
void download_workspace_backup();
void choose_workspace_backup();
void apply_workspace_backup(const char* token);
bool backup_request_pending(const char* operation = nullptr);
std::optional<std::string> take_workspace_backup_reply();
void clear_workspace_backup_request();
void show_config_json_editor(const std::string& initial_text, bool read_only, CanvasViewport bounds,
                             CanvasViewport clip);
bool config_json_editor_ready();
std::optional<std::string> read_config_json_editor_text();
void close_config_json_editor();
void finish_config_json_editor_frame();

} // namespace wave
