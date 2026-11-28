#pragma once

#include "config_nodes_view.h"
#include "config_users_view.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wave {

struct PanelControlsState;

struct ConfigWorkspaceTab {
    enum class Kind {
        nodes,
        users
    };

    std::string node;
    Kind kind = Kind::nodes;

    std::string label() const;
    bool operator==(const ConfigWorkspaceTab&) const = default;
};

struct ConfigWorkspaceState {
    ConfigNodesState nodes;
    std::map<std::string, ConfigUsersState, std::less<>> users;
    std::vector<ConfigWorkspaceTab> tabs;
    ConfigWorkspaceTab selected;
    std::optional<ConfigWorkspaceTab> requested;
};

void sync_config_workspace(ConfigWorkspaceState& state, PreviewSession& session,
                           const nlohmann::json& config, bool visible);
void render_config_workspace(ConfigWorkspaceState& state, PanelControlsState& controls,
                             PreviewSession& session, const nlohmann::json& config, int config_id,
                             bool narrow);
std::string describe_config_workspace(const ConfigWorkspaceState& state, const PreviewSession& session);
CanvasViewport config_workspace_scroll_bounds(const ConfigWorkspaceState& state);
void abandon_config_workspace(ConfigWorkspaceState& state, PreviewSession& session);

} // namespace wave
