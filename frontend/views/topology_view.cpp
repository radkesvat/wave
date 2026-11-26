#include "topology_view.h"

#include "dialog.h"
#include "view_helpers.h"

#include <cfloat>
#include <format>
#include <utility>
#include <vector>

namespace wave {

namespace {

using namespace view;
using theme::Color;
using theme::packed;

using Selection = TopologyViewState::Selection;
constexpr char add_dialog[] = "Add sample node";
constexpr char delete_dialog[] = "Delete sample node";
constexpr char inspect_dialog[] = "Inspect topology";
constexpr float min_zoom = 0.7f;
constexpr float max_zoom = 2.5f;

std::string_view node_label(const Json& nodes, std::string_view id)
{
    for (const auto& node : nodes) {
        if (string_field(node, "id") == id) {
            return string_field(node, "label", id);
        }
    }
    return id;
}

void inspect(TopologyViewState& state, Selection selection, std::string_view id)
{
    state.selection = selection;
    state.selected = id;
    state.reply.reset();
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

void draw_zoom(TopologyViewState& state, bool empty)
{
    ImGui::BeginDisabled(empty || state.zoom <= min_zoom);
    if (theme::button("-##zoom")) {
        state.zoom = std::max(min_zoom, state.zoom / 1.25f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(empty || state.zoom >= max_zoom);
    if (theme::button("+##zoom")) {
        state.zoom = std::min(max_zoom, state.zoom * 1.25f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(empty);
    if (theme::button("Fit map")) {
        state.zoom = 1;
        state.offset_x = state.offset_y = 0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::rgba(Color::secondary_text), "%.0f%%", state.zoom * 100);
}

void draw_topology(TopologyViewState& state, const Json& nodes, const Json& links)
{
    const auto available = ImGui::GetContentRegionAvail();
    const float width = std::max(1.0f, available.x);
    const float height = std::max(1.0f, available.y);
    const auto pos = ImGui::GetCursorScreenPos();
    const ImVec2 corner{std::round(pos.x), std::round(pos.y)};
    const ImVec2 bottom{std::round(pos.x + width), std::round(pos.y + height)};

    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(corner, bottom, packed(Color::canvas));
    draw.PushClipRect(corner, bottom, true);
    const ImVec2 visible_min = draw.GetClipRectMin();
    const ImVec2 visible_max = draw.GetClipRectMax();
    if (!nodes.empty()) {
        state.visible_bounds = {visible_min.x, visible_min.y, visible_max.x, visible_max.y};
    }
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
    const ImU32 grid_color = packed(Color::plot_grid);
    const ImU32 border = packed(Color::control_border);
    const ImU32 accent = packed(Color::accent);
    const ImU32 foreground = packed(Color::primary_text);
    constexpr float grid = 28;
    for (float x = corner.x + std::round(std::fmod(state.offset_x, grid)); x < bottom.x; x += grid) {
        draw.AddLineV(x, corner.y, bottom.y, grid_color, theme::border_width);
    }
    for (float y = corner.y + std::round(std::fmod(state.offset_y, grid)); y < bottom.y; y += grid) {
        draw.AddLineH(corner.x, bottom.x, y, grid_color, theme::border_width);
    }

    // Smaller cards leave room between nearby nodes when the pane is short.
    const float preferred_width = width < 700 ? 74.0f : 103.0f;
    const float node_width = height < 400 ? std::clamp(width / 11, 42.0f, preferred_width) : preferred_width;
    constexpr float node_height = 34.0f;
    const ImVec2 center{corner.x + width / 2 + state.offset_x, corner.y + height / 2 + state.offset_y};
    const float span_x = std::max(1.0f, width - node_width - 22) * state.zoom;
    const float span_y = std::max(1.0f, height - node_height - 22) * state.zoom;
    std::vector<GraphNode> graph_nodes;
    graph_nodes.reserve(nodes.size());
    for (const auto& node : nodes) {
        const float x = static_cast<float>(std::clamp(numeric_field(node, "x", 0.5), 0.0, 1.0));
        const float y = static_cast<float>(std::clamp(numeric_field(node, "y", 0.5), 0.0, 1.0));
        graph_nodes.push_back({node,
                               string_field(node, "id"),
                               {center.x + (x - 0.5f) * span_x, center.y + (y - 0.5f) * span_y}});
    }
    const auto lookup_node = [&](std::string_view id) {
        return id.empty() ? graph_nodes.end() : std::ranges::find(graph_nodes, id, &GraphNode::id);
    };
    float nearest_distance = 8.0f;
    std::string_view nearest_link;
    for (const auto& link : links) {
        const auto from = lookup_node(string_field(link, "from"));
        const auto to = lookup_node(string_field(link, "to"));
        if (from == graph_nodes.end() || to == graph_nodes.end()) {
            continue;
        }
        const auto id = string_field(link, "id");
        const bool active = !id.empty() && state.selection == Selection::link && state.selected == id;
        draw.AddLine(from->position, to->position, active ? accent : packed(Color::info),
                     active ? 3.0f : 1.5f);
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
        const ImVec2 start{std::round(node.position.x - node_width / 2),
                           std::round(node.position.y - node_height / 2)};
        const ImVec2 end{start.x + node_width, start.y + node_height};
        if (inspect_graph && !node.id.empty() && mouse.x >= start.x && mouse.x <= end.x &&
            mouse.y >= start.y && mouse.y <= end.y) {
            inspect(state, Selection::node, node.id);
        }
        const bool active =
            !node.id.empty() && state.selection == Selection::node && state.selected == node.id;
        draw.AddRectFilled(start, end, packed(active ? Color::selected : Color::raised),
                           theme::control_radius);
        draw.AddRect(start, end, active ? accent : border, theme::control_radius,
                     active ? theme::selection_width : theme::border_width);
        const auto label = string_field(node.data, "label", node.id);
        if (label.empty()) {
            continue;
        }
        auto* font = ImGui::GetFont();
        const float font_size = node_width < 84 ? 13.0f : 15.0f;
        const float wrap_width = node_width - 8;
        const ImVec4 label_clip{start.x + 4, start.y + 2, end.x - 4, end.y - 2};
        const auto* label_end = label.data() + label.size();
        const auto extent = font->CalcTextSizeA(font_size, FLT_MAX, wrap_width, label.data(), label_end);
        draw.AddText(
            font, font_size,
            {start.x + (node_width - extent.x) / 2, start.y + std::max(2.0f, (node_height - extent.y) / 2)},
            foreground, label.data(), label_end, wrap_width, &label_clip);
    }
    if (nodes.empty()) {
        constexpr char message[] = "No sample nodes";
        const auto size = ImGui::CalcTextSize(message);
        draw.AddText({corner.x + (width - size.x) / 2, corner.y + (height - size.y) / 2},
                     packed(Color::secondary_text), message);
    }
    draw.PopClipRect();
    draw.AddRect(corner, bottom, border, theme::square_radius, theme::border_width);
}

const Json* selected_item(const Json& items, std::string_view id)
{
    if (id.empty()) {
        return nullptr;
    }
    const auto item =
        std::ranges::find_if(items, [&](const Json& value) { return string_field(value, "id") == id; });
    return item == items.end() ? nullptr : &*item;
}

const Json* selection_data(const TopologyViewState& state, const Json& topology)
{
    switch (state.selection) {
    case Selection::node:
        return selected_item(array_field(topology, "nodes"), state.selected);
    case Selection::link:
        return selected_item(array_field(topology, "links"), state.selected);
    case Selection::route:
        return selected_item(array_field(topology, "routes"), state.selected);
    }
    return nullptr;
}

std::string selection_details(Selection selection, const Json& item, const Json& nodes)
{
    const auto id = string_field(item, "id");
    std::string details{string_field(item, selection == Selection::route ? "name" : "label", id)};
    const auto append = [&](std::string_view part) {
        if (!part.empty()) {
            details += " · ";
            details += part;
        }
    };
    const auto type = string_field(item, "type");
    if (!type.empty()) {
        details += std::format(" ({})", type);
    }
    switch (selection) {
    case Selection::node:
        append(string_field(item, "address"));
        break;
    case Selection::link:
        append(std::format("{} to {}", node_label(nodes, string_field(item, "from")),
                           node_label(nodes, string_field(item, "to"))));
        break;
    case Selection::route: {
        std::string path;
        for (const auto& node : array_field(item, "path")) {
            if (!path.empty()) {
                path += " > ";
            }
            path += node_label(nodes, string_value(node));
        }
        append(path);
        break;
    }
    }
    append(string_field(item, "description"));
    return details;
}

void draw_toolbar(TopologyViewState& state, bool empty, bool node_selected)
{
    ImGui::PushFont(nullptr, theme::secondary_font_size);
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float actions_width = button_width("Add node") + gap + button_width("Delete node");
    const float zoom_width = button_width("-##zoom") + button_width("+##zoom") + button_width("Fit map") +
                             3 * gap + ImGui::CalcTextSize("250%").x;
    if (theme::button("Add node")) {
        state.node_name.fill('\0');
        state.reply.reset();
        ImGui::OpenPopup(add_dialog);
    }
    if (width >= actions_width) {
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!node_selected);
    if (theme::button("Delete node")) {
        state.delete_id = state.selected;
        state.reply.reset();
        ImGui::OpenPopup(delete_dialog);
    }
    ImGui::EndDisabled();
    if (!node_selected) {
        ImGui::SetItemTooltip("Select a node to delete it.");
    }
    const bool inline_zoom = width >= actions_width + gap + zoom_width;
    if (inline_zoom) {
        ImGui::SameLine();
    }
    draw_zoom(state, empty);
    const float used_width = zoom_width + (inline_zoom ? actions_width + gap : 0);
    if (width >= used_width + gap + button_width("Inspect items")) {
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(empty);
    if (theme::button("Inspect items")) {
        ImGui::OpenPopup(inspect_dialog);
    }
    ImGui::EndDisabled();
    ImGui::PopFont();
}

void inspect_items(TopologyViewState& state, const char* title, const Json& items, Selection selection)
{
    if (items.empty()) {
        return;
    }
    ImGui::SeparatorText(title);
    ImGui::PushID(title);
    for (const auto& item : items) {
        const auto id = string_field(item, "id");
        if (id.empty()) {
            continue;
        }
        const auto label = string_field(item, selection == Selection::route ? "name" : "label", id);
        const auto pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(id.data(), id.data() + id.size());
        if (ImGui::Selectable("##item", selection == state.selection && id == state.selected,
                              ImGuiSelectableFlags_NoAutoClosePopups, {0, ImGui::GetTextLineHeight()})) {
            inspect(state, selection, id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), label.data(),
                                            label.data() + label.size());
        ImGui::PopID();
    }
    ImGui::PopID();
}

void render_node_dialog(TopologyViewState& state, PreviewSession& session, const Json& nodes, bool adding)
{
    const char* title = adding ? add_dialog : delete_dialog;
    if (!ImGui::IsPopupOpen(title)) {
        return;
    }
    if (auto reply = session.take_reply(state.request_id)) {
        if (reply->status == PreviewReply::Status::timed_out) {
            reply->message = "The preview did not confirm the change.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = "The preview connection closed. Reconnect to check the canvas.";
        }
        state.reply = std::move(reply);
    }

    const bool pending = state.request_id.has_value();
    const auto* node = adding ? nullptr : selected_item(nodes, state.delete_id);
    const std::string_view name(state.node_name.data());
    const bool valid_name = name.size() <= 64 && name.find_first_not_of(' ') != std::string_view::npos &&
                            std::ranges::none_of(name, [](unsigned char c) { return c < ' ' || c == 0x7f; });
    const bool disabled = pending || !session.ready() || (adding ? !valid_name : !node);
    Dialog frame(title, 380, {{"Cancel", pending}, {adding ? "Add node" : "Delete node", disabled}});
    if (!frame) {
        return;
    }
    if (adding) {
        ImGui::BeginDisabled(pending);
        edit("Node name", state.node_name, "Enter a name");
        ImGui::EndDisabled();
        if (!name.empty() && !valid_name) {
            wrapped("Enter a name of up to 64 bytes without control characters.", Color::error);
        }
    } else if (node) {
        wrapped(std::format("Delete {} from the preview? Its connections and routes will also be removed.",
                            string_field(*node, "label", state.delete_id)));
    } else if (!pending && !state.reply && session.ready()) {
        wrapped("That sample node is unavailable.", Color::error);
    }
    if (pending) {
        wrapped("Applying to preview…", Color::info);
    } else if (state.reply && !state.reply->ok()) {
        feedback(*state.reply);
    } else if (!session.ready()) {
        wrapped("The preview connection is unavailable.", Color::error);
    }

    const int action = frame.footer();
    if (action == 0 || (state.reply && state.reply->ok())) {
        ImGui::CloseCurrentPopup();
    } else if (action == 1) {
        Json request = adding ? Json{{"action", "add_node"}, {"name", state.node_name.data()}}
                              : Json{{"action", "delete_node"}, {"id", state.delete_id}};
        state.request_id = session.submit(std::move(request), 5000);
        state.reply.reset();
        if (!state.request_id) {
            state.reply =
                PreviewReply{PreviewReply::Status::disconnected, "The preview connection is unavailable."};
        }
    }
}

} // namespace

bool TopologyViewState::canvas_at(float x, float y) const
{
    const auto& bounds = visible_bounds;
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

void TopologyViewState::pan(float dx, float dy)
{
    const float width = visible_bounds.right - visible_bounds.left;
    const float height = visible_bounds.bottom - visible_bounds.top;
    if (width > 0 && height > 0) {
        offset_x = std::clamp(offset_x + dx, -width, width);
        offset_y = std::clamp(offset_y + dy, -height, height);
    }
}

void render_topology_dialogs(TopologyViewState& state, PreviewSession& session)
{
    const auto& topology = field(session.data(), "topology");
    const auto& nodes = array_field(topology, "nodes");
    render_node_dialog(state, session, nodes, true);
    render_node_dialog(state, session, nodes, false);

    Dialog frame(inspect_dialog, 380, {{"Close"}});
    if (frame) {
        if (session.ready()) {
            inspect_items(state, "Nodes", nodes, Selection::node);
            inspect_items(state, "Connections", array_field(topology, "links"), Selection::link);
            inspect_items(state, "Routes", array_field(topology, "routes"), Selection::route);
        } else {
            wrapped("Sample data unavailable.", Color::warning);
        }
        if (frame.footer() == 0) {
            ImGui::CloseCurrentPopup();
        }
    }
}

void render_topology(TopologyViewState& state, PreviewSession& session, bool narrow)
{
    const auto& topology = field(session.data(), "topology");
    const auto& nodes = array_field(topology, "nodes");
    const auto& links = array_field(topology, "links");
    const auto& routes = array_field(topology, "routes");
    const auto* selected = selection_data(state, topology);
    if (!selected) {
        state.selected.clear();
    }
    if (nodes.empty()) {
        state.zoom = 1;
        state.offset_x = state.offset_y = 0;
    }
    draw_toolbar(state, nodes.empty() && links.empty() && routes.empty(),
                 selected && state.selection == Selection::node);

    ImGui::PushFont(nullptr, theme::annotation_font_size);
    const float detail_height = (narrow ? 3 : 2) * ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("Selection details", {0, detail_height}, ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground)) {
        if (state.reply && !ImGui::IsPopupOpen(add_dialog) && !ImGui::IsPopupOpen(delete_dialog)) {
            feedback(*state.reply);
        } else if (selected) {
            wrapped(selection_details(state.selection, *selected, nodes));
        } else {
            wrapped(nodes.empty() ? "Use Add node to place a sample node on the canvas."
                                  : "Drag to pan. Select a node or connection, or use Inspect items.",
                    Color::secondary_text);
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();
    draw_topology(state, nodes, links);
    render_topology_dialogs(state, session);
}

std::string describe_topology(const TopologyViewState& state, const Json& topology)
{
    const auto& nodes = array_field(topology, "nodes");
    const auto links = array_field(topology, "links").size();
    const auto routes = array_field(topology, "routes").size();
    auto description =
        std::format("{} node{}, {} connection{}, {} route{}.", nodes.size(), nodes.size() == 1 ? "" : "s",
                    links, links == 1 ? "" : "s", routes, routes == 1 ? "" : "s");
    if (const auto* selected = selection_data(state, topology)) {
        description += " Selected " + selection_details(state.selection, *selected, nodes) + '.';
    }
    return description;
}

} // namespace wave
