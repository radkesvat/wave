#pragma once

#include "core_config_name.h"
#include "preview_session.h"

#include <array>

namespace wave {

struct PanelControlsState {
    enum class Dialog {
        none,
        stop,
        update,
        create,
        manage,
        remove
    };

    Dialog dialog = Dialog::none;
    bool open_dialog = false;
    std::string target_version;
    // Leave room for one extra UTF-8 character so validation can detect an overlong name.
    std::array<char, max_core_config_name_bytes + 5> config_name{};
    int remove_id = 0;
    unsigned keyboard_focus = 0;
    std::string remove_name;
    std::optional<std::uint64_t> request_id;
    std::optional<PreviewReply> reply;
};

void update_panel_controls(PanelControlsState& state, PreviewSession& session);
float panel_controls_height(const PanelControlsState& state, const PreviewSession& session, float width,
                            bool compact);
void render_panel_controls(PanelControlsState& state, PreviewSession& session, float width, bool compact);
void render_panel_control_dialog(PanelControlsState& state, PreviewSession& session);
std::string describe_panel_controls(const PanelControlsState& state, const PreviewSession& session);

} // namespace wave
