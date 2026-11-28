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

void sync_node_tabs(ConfigWorkspaceState& state, PreviewSession& session, const view::Json& config)
{
    state.tabs.clear();
    state.tabs.emplace_back();
    if (!waterwall_version().empty()) {
        for (const auto& item : view::array_field(config, "auth_nodes")) {
            const auto node = view::string_value(item);
            if (!node.empty()) {
                state.tabs.push_back({std::string(node), TabKind::users});
            }
        }
    }
    const auto& core = view::field(view::field(session.data(), "server"), "core");
    const auto core_state = view::string_field(core, "state");
    const auto& access = view::field(config, "users_access");
    for (auto it = state.users.begin(); it != state.users.end();) {
        if (std::ranges::find(state.tabs, Tab{it->first, TabKind::users}) == state.tabs.end()) {
            abandon_config_users(it->second, session);
            it = state.users.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& tab : state.tabs) {
        if (tab.kind == TabKind::users) {
            sync_config_users(state.users[tab.node], session, view::field(access, tab.node), core_state,
                              view::bool_field(config, "active"), view::bool_field(config, "live_users"));
        }
    }
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
    std::string id = "###Users/";
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
    case Kind::users:
        return node;
    }
    return {};
}

void sync_config_workspace(ConfigWorkspaceState& state, PreviewSession& session, const view::Json& config,
                           bool visible)
{
    sync_node_tabs(state, session, config);
    for (auto& [node, users] : state.users) {
        if (!visible || state.selected.kind != TabKind::users || state.selected.node != node) {
            hide_config_users(users, session);
        }
    }
}

void render_config_workspace(ConfigWorkspaceState& state, PanelControlsState& controls,
                             PreviewSession& session, const nlohmann::json& config, int config_id,
                             bool narrow)
{
    sync_config_workspace(state, session, config, true);
    state.nodes.viewport = {};
    render_config_header(controls, session, config, config_id, state.nodes.dirty, narrow);
    ImGui::PushID(config_id);
    if (!ImGui::BeginTabBar("Config tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        ImGui::PopID();
        return;
    }

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
        case TabKind::users:
            render_config_users(state.users[tab.node], session, config_id, tab.node, narrow);
            break;
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::PopID();
    for (auto& [node, users] : state.users) {
        if (state.selected.kind != TabKind::users || state.selected.node != node) {
            hide_config_users(users, session);
        }
    }
}

std::string describe_config_workspace(const ConfigWorkspaceState& state, const PreviewSession& session)
{
    auto description = state.selected.label() + " tab. ";
    switch (state.selected.kind) {
    case TabKind::nodes:
        return description + describe_config_nodes(state.nodes);
    case TabKind::users: {
        const auto users = state.users.find(state.selected.node);
        return description + (users == state.users.end() ? "Users." : describe_config_users(users->second, session));
    }
    }
    return description;
}

CanvasViewport config_workspace_scroll_bounds(const ConfigWorkspaceState& state)
{
    if (state.selected.kind == TabKind::users) {
        const auto users = state.users.find(state.selected.node);
        return users != state.users.end() ? users->second.scroll_bounds : CanvasViewport{};
    }
    return {};
}

void abandon_config_workspace(ConfigWorkspaceState& state, PreviewSession& session)
{
    session.abandon(state.nodes.request);
    for (auto& [_, users] : state.users) {
        abandon_config_users(users, session);
    }
}

} // namespace wave
