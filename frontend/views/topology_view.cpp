#include "topology_view.h"

#include "view_helpers.h"

#include <cfloat>
#include <format>
#include <vector>

namespace wave {

namespace {

using namespace view;

using Selection = TopologyViewState::Map::Selection;
using Tab = TopologyViewState::Tab;

bool in_map(const Json& item, std::string_view map)
{
    const auto membership = string_field(item, "map", "both");
    return membership == "both" || membership == map;
}

std::string_view node_label(const Json& nodes, std::string_view id)
{
    for (const auto& node : nodes) {
        if (string_field(node, "id") == id) {
            return string_field(node, "label", id);
        }
    }
    return id;
}

void inspect(TopologyViewState::Map& state, Selection selection, std::string_view id)
{
    state.selection = selection;
    state.selected = id;
}

float distance_to_link(ImVec2 mouse, ImVec2 start, ImVec2 end)
{
    const float x = end.x - start.x;
    const float y = end.y - start.y;
    const float length_squared = x * x + y * y;
    const float fraction =
        length_squared > 0
            ? std::clamp(((mouse.x - start.x) * x + (mouse.y - start.y) * y) / length_squared, 0.0f, 1.0f)
            : 0.0f;
    return std::hypot(mouse.x - start.x - x * fraction, mouse.y - start.y - y * fraction);
}

struct GraphNode {
    const Json& data;
    std::string_view id;
    ImVec2 position;
};

TopologyViewState::Bounds draw_topology(TopologyViewState::Map& state, const Json& nodes, const Json& links,
                                        std::string_view map, bool narrow)
{
    ImGui::BeginDisabled(state.zoom <= 0.7f);
    if (ImGui::Button("-##zoom")) {
        state.zoom = std::max(0.7f, state.zoom / 1.25f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.zoom >= 2.5f);
    if (ImGui::Button("+##zoom")) {
        state.zoom = std::min(2.5f, state.zoom * 1.25f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Fit map")) {
        state.zoom = 1;
        state.offset_x = state.offset_y = 0;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", state.zoom * 100);
    ImGui::TextWrapped("Drag to pan. Select a node or connection to inspect it.");

    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = narrow ? 230.0f : 290.0f;
    state.width = width;
    state.height = height;
    const auto corner = ImGui::GetCursorScreenPos();
    const ImVec2 bottom{corner.x + width, corner.y + height};
    const bool selected = ImGui::InvisibleButton("##network-topology", {width, height});
    if (ImGui::IsItemActivated()) {
        state.graph_dragged = false;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 7)) {
        const auto delta = ImGui::GetIO().MouseDelta;
        state.pan(delta.x, delta.y);
        state.graph_dragged = true;
    }
    const bool inspect_graph = selected && !state.graph_dragged;
    const auto mouse = ImGui::GetIO().MousePos;
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(corner, bottom, plot_surface, 6);
    draw.PushClipRect(corner, bottom, true);
    const ImVec2 visible_min = draw.GetClipRectMin();
    const ImVec2 visible_max = draw.GetClipRectMax();
    constexpr float grid = 28;
    for (float x = corner.x + std::fmod(state.offset_x, grid); x < bottom.x; x += grid) {
        draw.AddLineV(x, corner.y, bottom.y, plot_grid);
    }
    for (float y = corner.y + std::fmod(state.offset_y, grid); y < bottom.y; y += grid) {
        draw.AddLineH(corner.x, bottom.x, y, plot_grid);
    }

    const float node_width = narrow ? 71.0f : 103.0f;
    constexpr float node_height = 34.0f;
    const ImVec2 center{corner.x + width / 2 + state.offset_x, corner.y + height / 2 + state.offset_y};
    const float span_x = std::max(1.0f, width - node_width - 22) * state.zoom;
    const float span_y = (height - node_height - 22) * state.zoom;
    std::vector<GraphNode> graph_nodes;
    graph_nodes.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (!in_map(node, map)) {
            continue;
        }
        const float x = static_cast<float>(std::clamp(numeric_field(node, "x", 0.5), 0.0, 1.0));
        const float y = static_cast<float>(std::clamp(numeric_field(node, "y", 0.5), 0.0, 1.0));
        graph_nodes.push_back({node,
                               string_field(node, "id"),
                               {center.x + (x - 0.5f) * span_x, center.y + (y - 0.5f) * span_y}});
    }
    const auto lookup_node = [&](std::string_view id) {
        return std::ranges::find(graph_nodes, id, &GraphNode::id);
    };
    float nearest_distance = 8.0f;
    std::string_view nearest_link;
    for (const auto& link : links) {
        if (!in_map(link, map)) {
            continue;
        }
        const auto from = lookup_node(string_field(link, "from"));
        const auto to = lookup_node(string_field(link, "to"));
        if (from == graph_nodes.end() || to == graph_nodes.end()) {
            continue;
        }
        const auto id = string_field(link, "id");
        const bool active = state.selection == Selection::link && state.selected == id;
        draw.AddLine(from->position, to->position, active ? plot_mint : plot_blue, active ? 3.0f : 1.5f);
        if (inspect_graph) {
            const float distance = distance_to_link(mouse, from->position, to->position);
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_link = id;
            }
        }
    }
    if (!nearest_link.empty()) {
        inspect(state, Selection::link, nearest_link);
    }
    for (const auto& node : graph_nodes) {
        const ImVec2 start{node.position.x - node_width / 2, node.position.y - node_height / 2};
        const ImVec2 end{start.x + node_width, start.y + node_height};
        if (inspect_graph && mouse.x >= start.x && mouse.x <= end.x && mouse.y >= start.y &&
            mouse.y <= end.y) {
            inspect(state, Selection::node, node.id);
        }
        const bool active = state.selection == Selection::node && state.selected == node.id;
        draw.AddRectFilled(start, end, IM_COL32(25, 50, 66, 255), 6);
        draw.AddRect(start, end, active ? plot_mint : plot_blue, 6, active ? 2.5f : 1.0f);
        const auto label = string_field(node.data, "label", node.id);
        auto* font = ImGui::GetFont();
        const float font_size = narrow ? 13.0f : 15.0f;
        const float wrap_width = node_width - 12;
        const auto* label_end = label.data() + label.size();
        const auto extent = font->CalcTextSizeA(font_size, FLT_MAX, wrap_width, label.data(), label_end);
        draw.AddText(font, font_size, {node.position.x - extent.x / 2, node.position.y - extent.y / 2},
                     IM_COL32(234, 243, 247, 255), label.data(), label_end, wrap_width);
    }
    draw.PopClipRect();
    return {visible_min.x, visible_min.y, visible_max.x, visible_max.y};
}

const Json* selected_item(const Json& items, std::string_view id)
{
    const auto item =
        std::ranges::find_if(items, [&](const Json& value) { return string_field(value, "id") == id; });
    return item == items.end() ? nullptr : &*item;
}

void topology_details(const TopologyViewState::Map& state, const Json& nodes, const Json& links,
                      const Json& routes)
{
    if (state.selected.empty()) {
        return;
    }
    ImGui::SeparatorText("Inspection");
    const Json* selected = nullptr;
    switch (state.selection) {
    case Selection::node:
        selected = selected_item(nodes, state.selected);
        if (selected) {
            wrapped(
                std::format("{} ({})", string_field(*selected, "label"), string_field(*selected, "type")));
            wrapped(string_field(*selected, "address"));
        }
        break;
    case Selection::link:
        selected = selected_item(links, state.selected);
        if (selected) {
            wrapped(
                std::format("{} ({})", string_field(*selected, "label"), string_field(*selected, "type")));
            wrapped(std::format("{} to {}", node_label(nodes, string_field(*selected, "from")),
                                node_label(nodes, string_field(*selected, "to"))));
        }
        break;
    case Selection::route:
        selected = selected_item(routes, state.selected);
        if (selected) {
            wrapped(string_field(*selected, "name"));
            std::string path;
            for (const auto& node : array_field(*selected, "path")) {
                if (!path.empty()) {
                    path += " > ";
                }
                path += node_label(nodes, string_value(node));
            }
            wrapped(path);
        }
        break;
    }
    if (selected) {
        wrapped(string_field(*selected, "description"));
    }
}

void render_map(TopologyViewState& view, Tab tab, const Json& topology, bool narrow)
{
    auto& state = tab == Tab::inside ? view.inside_map : view.outside_map;
    const std::string_view map = tab == Tab::inside ? "inside" : "outside";
    const auto& nodes = array_field(topology, "nodes");
    const auto& links = array_field(topology, "links");
    const auto& routes = array_field(topology, "routes");
    view.visible_map = tab;
    view.visible_bounds = draw_topology(state, nodes, links, map, narrow);
    topology_details(state, nodes, links, routes);
    if (ImGui::CollapsingHeader("Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& node : nodes) {
            if (in_map(node, map)) {
                const auto id = string_field(node, "id");
                const auto label = std::format("{}##{}", string_field(node, "label"), id);
                if (ImGui::Selectable(label.c_str(),
                                      state.selection == Selection::node && state.selected == id)) {
                    inspect(state, Selection::node, id);
                }
            }
        }
    }
    if (ImGui::CollapsingHeader("Connections")) {
        for (const auto& link : links) {
            if (in_map(link, map)) {
                const auto id = string_field(link, "id");
                const auto label = std::format("{}##{}", string_field(link, "label"), id);
                if (ImGui::Selectable(label.c_str(),
                                      state.selection == Selection::link && state.selected == id)) {
                    inspect(state, Selection::link, id);
                }
            }
        }
    }
    if (ImGui::CollapsingHeader("Routes")) {
        for (const auto& route : routes) {
            if (in_map(route, map)) {
                const auto id = string_field(route, "id");
                const auto label = std::format("{}##{}", string_field(route, "name"), id);
                if (ImGui::Selectable(label.c_str(),
                                      state.selection == Selection::route && state.selected == id)) {
                    inspect(state, Selection::route, id);
                }
            }
        }
    }
}

} // namespace

void TopologyViewState::Map::pan(float dx, float dy)
{
    offset_x = std::clamp(offset_x + dx, -width, width);
    offset_y = std::clamp(offset_y + dy, -height, height);
}

TopologyViewState::Tab TopologyViewState::map_at(float x, float y) const
{
    const auto& bounds = visible_bounds;
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom ? visible_map
                                                                                        : Tab::none;
}

void TopologyViewState::pan(Tab map, float dx, float dy)
{
    switch (map) {
    case Tab::inside:
        inside_map.pan(dx, dy);
        break;
    case Tab::outside:
        outside_map.pan(dx, dy);
        break;
    case Tab::none:
        break;
    }
}

void render_topology(TopologyViewState& state, const nlohmann::json& topology, bool narrow)
{
    if (ImGui::BeginTabBar("topology-views")) {
        if (ImGui::BeginTabItem("Inside Map")) {
            ImGui::PushID("inside-map");
            render_map(state, Tab::inside, topology, narrow);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Outside Map")) {
            ImGui::PushID("outside-map");
            render_map(state, Tab::outside, topology, narrow);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace wave
