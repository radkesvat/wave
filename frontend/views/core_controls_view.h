#pragma once

#include "core_config_name.h"
#include "preview_session.h"

#include <array>

namespace wave {

struct CoreControlsState {
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

void update_core_controls(CoreControlsState& state, PreviewSession& session);
float core_controls_height(const CoreControlsState& state, const PreviewSession& session, float width);
void render_core_controls(CoreControlsState& state, PreviewSession& session, float width);
void render_core_control_dialog(CoreControlsState& state, PreviewSession& session);
std::string describe_core_controls(const CoreControlsState& state, const PreviewSession& session);

} // namespace wave
