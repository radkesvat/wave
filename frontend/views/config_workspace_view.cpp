#include "config_workspace_view.h"

#include "panel_controls_view.h"
#include "view_helpers.h"
#include "waterwall_schematic.h"

#include <imgui_internal.h>

#include <format>

namespace wave {
namespace {

using Tab = ConfigWorkspaceTab;
using TabKind = Tab::Kind;

template <typename Users>
void prune_users(Users& users, const std::vector<Tab>& tabs, TabKind kind, PreviewSession& session)
{
    for (auto it = users.begin(); it != users.end();) {
        const bool exists = std::ranges::any_of(
            tabs, [&](const Tab& tab) { return tab.kind == kind && tab.node == it->first; });
        if (exists) {
            ++it;
        } else {
            session.abandon(it->second.request);
            it = users.erase(it);
        }
    }
}

void sync_node_tabs(ConfigWorkspaceState& state, PreviewSession& session, const view::Json& config)
{
    state.tabs.clear();
    state.tabs.emplace_back();
    const bool has_live_users = view::bool_field(config, "live_users");
    if (!waterwall_version().empty()) {
        for (const auto& item : view::array_field(config, "auth_nodes")) {
            const auto node = view::string_value(item);
            if (!node.empty()) {
                state.tabs.push_back({std::string(node), TabKind::offline_users});
                if (has_live_users) {
                    state.tabs.push_back({std::string(node), TabKind::live_users});
                }
            }
        }
    }
    prune_users(state.users, state.tabs, TabKind::offline_users, session);
    prune_users(state.live_users, state.tabs, TabKind::live_users, session);
    if (std::ranges::find(state.tabs, state.selected) == state.tabs.end()) {
        state.selected = {};
        state.requested = state.selected;
    }
    if (state.requested && std::ranges::find(state.tabs, *state.requested) == state.tabs.end()) {
        state.requested.reset();
    }
}

bool begin_tab(const ConfigWorkspaceState& state, const Tab& tab)
{
    const auto flags = state.requested == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    if (tab.kind == TabKind::nodes) {
        return ImGui::BeginTabItem("Nodes", nullptr, flags);
    }

    // Keep the tab ID independent from a name that may contain ImGui's ## delimiters.
    std::string id = tab.kind == TabKind::live_users ? "###Live users/" : "###Offline users/";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : tab.node) {
        id += hex[byte >> 4];
        id += hex[byte & 0x0f];
    }
    const auto label = tab.label();
    const auto* end = label.data() + label.size();
    const auto* tab_bar = ImGui::GetCurrentContext()->CurrentTabBar;
    const auto padding = tab_bar->FramePadding;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(label.data(), end).x + 2 * padding.x);
    const bool selected = ImGui::BeginTabItem(id.c_str(), nullptr, flags);
    if (ImGui::IsItemVisible()) {
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        auto* draw = ImGui::GetWindowDrawList();
        const float left = std::max(min.x, tab_bar->ScrollingRectMinX);
        const float right = std::min(max.x, tab_bar->ScrollingRectMaxX);
        draw->PushClipRect({left, min.y}, {right, max.y}, true);
        ImGui::RenderTextEllipsis(draw, {min.x + padding.x, min.y + padding.y}, {max.x - padding.x, max.y},
                                  max.x - padding.x, label.data(), end, nullptr);
        draw->PopClipRect();
        ImGui::SetItemTooltip("%s", label.c_str());
    }
    return selected;
}

} // namespace

std::string ConfigWorkspaceTab::label() const
{
    switch (kind) {
    case Kind::nodes:
        return "Nodes";
    case Kind::offline_users:
        return node;
    case Kind::live_users:
        return node + " Live";
    }
    return {};
}

void render_config_workspace(ConfigWorkspaceState& state, PanelControlsState& controls,
                             PreviewSession& session, const nlohmann::json& config, int config_id,
                             bool narrow)
{
    sync_node_tabs(state, session, config);
    state.nodes.viewport = {};
    render_config_header(controls, session, config, config_id, state.nodes.dirty, narrow);
    if (!ImGui::BeginTabBar("Config tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        return;
    }

    const auto& core = view::field(view::field(session.data(), "server"), "core");
    const auto core_state = view::string_field(core, "state");
    const bool active = view::bool_field(config, "active");
    const bool offline_available =
        !active && core_state != "starting" && core_state != "running" && core_state != "stopping";
    const bool live_available = active && core_state == "running";
    for (const auto& tab : state.tabs) {
        if (!begin_tab(state, tab)) {
            continue;
        }
        state.selected = tab;
        if (state.requested == tab) {
            state.requested.reset();
        }
        switch (tab.kind) {
        case TabKind::nodes:
            render_config_nodes(state.nodes, session, config_id, narrow);
            break;
        case TabKind::offline_users:
            render_config_users(state.users[tab.node], session, config_id, tab.node, offline_available,
                                narrow);
            break;
        case TabKind::live_users:
            render_config_live_users(state.live_users[tab.node], session, config_id, tab.node, live_available,
                                     narrow);
            break;
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

std::string describe_config_workspace(const ConfigWorkspaceState& state)
{
    auto description = state.selected.label() + " tab. ";
    switch (state.selected.kind) {
    case TabKind::nodes:
        return description + describe_config_nodes(state.nodes);
    case TabKind::offline_users: {
        const auto users = state.users.find(state.selected.node);
        return description +
               (users == state.users.end() ? "Offline users." : describe_config_users(users->second));
    }
    case TabKind::live_users: {
        const auto users = state.live_users.find(state.selected.node);
        return description +
               (users == state.live_users.end() ? "Live users." : describe_config_live_users(users->second));
    }
    }
    return description;
}

CanvasViewport config_workspace_scroll_bounds(const ConfigWorkspaceState& state)
{
    if (state.selected.kind == TabKind::offline_users) {
        const auto users = state.users.find(state.selected.node);
        return users != state.users.end() ? users->second.scroll_bounds : CanvasViewport{};
    }
    if (state.selected.kind == TabKind::live_users) {
        const auto users = state.live_users.find(state.selected.node);
        return users != state.live_users.end() ? users->second.scroll_bounds : CanvasViewport{};
    }
    return {};
}

void abandon_config_workspace(ConfigWorkspaceState& state, PreviewSession& session)
{
    session.abandon(state.nodes.request);
    for (auto& [_, users] : state.users) {
        session.abandon(users.request);
    }
    for (auto& [_, users] : state.live_users) {
        session.abandon(users.request);
    }
}

} // namespace wave
