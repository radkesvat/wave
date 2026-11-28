#pragma once

#include "core_config_name.h"
#include "preview_session.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

struct PanelUpdateState {
    std::optional<std::uint64_t> request_id;
    std::optional<std::uint64_t> check_generation;
    std::string available_version;
    bool open_dialog = false;
};

struct PanelControlsState {
    enum class Dialog {
        none,
        stop,
        restart,
        update,
        create,
        remove,
        restore
    };

    struct RestoreProposal {
        std::string token;
        std::string summary;
    };

    Dialog dialog = Dialog::none;
    bool open_dialog = false;
    PanelUpdateState updates;
    std::string target_version;
    // Leave room for one extra UTF-8 character so validation can detect an overlong name.
    std::array<char, max_core_config_name_bytes + 5> config_name{};
    bool add_auth_nodes = false;
    int remove_id = 0;
    int action_config_id = 0;
    unsigned keyboard_focus = 0;
    std::string remove_name;
    bool remove_has_unsaved_changes = false;
    std::optional<std::uint64_t> request_id;
    std::optional<bool> request_accepted;
    std::optional<RestoreProposal> restore_proposal;
    std::string restore_error;
    bool restore_applied = false;
};

void update_panel_controls(PanelControlsState& state, PreviewSession& session);
void render_create_config_menu(PanelControlsState& state, const PreviewSession& session);
void render_config_header(PanelControlsState& state, PreviewSession& session, const nlohmann::json& config,
                          int config_id, bool dirty, bool narrow);
bool config_action_pending(const PanelControlsState& state, int config_id);
float panel_controls_height(const PreviewSession& session, float width, bool compact);
void render_panel_controls(PanelControlsState& state, PreviewSession& session, float width, bool compact);
void render_panel_control_dialog(PanelControlsState& state, PreviewSession& session);
void render_panel_update_dialog(PanelUpdateState& state, PreviewSession& session);
std::string describe_panel_update_dialog(const PanelUpdateState& state);
std::string describe_panel_controls(const PanelControlsState& state, const PreviewSession& session,
                                    bool include_status);

} // namespace wave
