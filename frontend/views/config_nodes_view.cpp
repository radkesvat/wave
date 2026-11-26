#include "config_nodes_view.h"

#include "config_json_import.h"
#include "core_config_json.h"
#include "dialog.h"
#include "view_helpers.h"
#include "waterwall_node_links.h"
#include "waterwall_nodes.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace wave {
namespace {

using namespace view;
using theme::Color;
using theme::packed;
using State = ConfigNodesState;
using Bounds = State::Bounds;

constexpr float card_width = 148;
constexpr float card_height = 58;
constexpr float leading = 26;
constexpr float column_width = 205;
constexpr float row_height = 88;
constexpr float zoom_min = 0.2f;
constexpr float zoom_max = 2.f;
constexpr float zoom_step = 1.25f;
constexpr float world_limit = 100000.f;
constexpr double request_timeout = 5000;

const Json& nodes(const State& state)
{
    return array_field(state.draft, "nodes");
}

std::string_view name_of(const Json& node)
{
    return string_field(node, "name");
}

bool can_send(std::string_view type, const Json& settings)
{
    // WaterWall v1.6.0, tunnels/*/instance/node.c can_have_next.
    constexpr std::array terminal_types{"APIServer",       "AuthenticationServer", "BlackHole",
                                        "SpeedTestServer", "TcpConnector",         "TcpUdpConnector",
                                        "UdpConnector"};
    return std::ranges::find(terminal_types, type) == terminal_types.end() &&
           allows_node_next(type, settings);
}

bool can_send(const Json& node)
{
    return can_send(string_field(node, "type"), field(node, "settings"));
}

bool can_receive(std::string_view type)
{
    // The can_have_prev flag is disabled on these chain heads.
    constexpr std::array head_types{"AuthenticationClient", "PacketGenerator",   "SpeedTestClient",
                                    "TcpListener",          "TcpUdpListener",    "TesterClient",
                                    "UdpListener",          "UdpStatelessSocket"};
    return std::ranges::find(head_types, type) == head_types.end();
}

std::optional<std::size_t> find_node(const Json& list, std::string_view name)
{
    if (name.empty()) {
        return {};
    }
    const auto found = std::ranges::find(list, name, name_of);
    return found == list.end() ? std::nullopt : std::optional{static_cast<std::size_t>(found - list.begin())};
}

void rename_links(Json& node, std::string_view old_name, std::string_view new_name)
{
    if (string_field(node, "next") == old_name) {
        node["next"] = new_name;
    }
    visit_node_links(node, [&](Json& container, const NodeLinkField&, const char* key) {
        if (string_field(container, key) == old_name) {
            container[key] = new_name;
        }
    });
}

std::string_view settings_reference_from(const Json& list, std::string_view name)
{
    for (const auto& node : list) {
        if (name_of(node) == name) {
            continue;
        }
        bool referenced = false;
        visit_node_links(node, [&](const Json& container, const NodeLinkField& setting, const char* key) {
            referenced |= node_link_active(container, setting) && string_field(container, key) == name;
        });
        if (referenced) {
            return name_of(node);
        }
    }
    return {};
}

struct Link {
    std::size_t from;
    std::size_t to;
    const NodeLinkField* setting = nullptr;

    bool is_chain() const
    {
        return !setting || setting->kind == NodeLinkKind::branch;
    }
};

std::vector<Link> links(const Json& list)
{
    std::map<std::string_view, std::size_t, std::less<>> names;
    for (std::size_t index = 0; index < list.size(); ++index) {
        names.emplace(name_of(list[index]), index);
    }
    std::vector<Link> result;
    for (std::size_t index = 0; index < list.size(); ++index) {
        const auto& node = list[index];
        std::set<std::pair<std::string_view, std::size_t>> shown;
        const auto append = [&](std::string_view target, const NodeLinkField* setting) {
            const auto found = names.find(target);
            if (!target.empty() && found != names.end() &&
                (!setting || shown.emplace(setting->key, found->second).second)) {
                result.push_back({index, found->second, setting});
            }
        };
        if (can_send(node)) {
            append(string_field(node, "next"), nullptr);
        }
        visit_node_links(node, [&](const Json& container, const NodeLinkField& setting, const char* key) {
            if (node_link_active(container, setting)) {
                append(string_field(container, key), &setting);
            }
        });
    }
    return result;
}

struct NodePorts {
    bool input = false;
    bool output = false;
};

std::vector<NodePorts> visible_ports(const Json& list, const std::vector<Link>& edges)
{
    std::vector<NodePorts> ports(list.size());
    for (const auto& edge : edges) {
        if (edge.is_chain()) {
            ports[edge.from].output = true;
            ports[edge.to].input = true;
        }
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& node = list[i];
        const auto type = string_field(node, "type");
        const bool adapter = type == "TunDevice" || type == "RawSocket";
        const auto connected = ports[i];
        // A device can be placed at either end. Show both choices until it is connected.
        ports[i] = {can_receive(type) && (!adapter || connected.input || !connected.output),
                    can_send(node) && (!adapter || connected.output || !connected.input)};
    }
    return ports;
}

std::vector<ImVec2> positions(const Json& list, const std::vector<Link>& edges)
{
    std::vector<std::vector<std::size_t>> outgoing(list.size());
    std::vector<std::size_t> incoming(list.size());
    for (const auto& edge : edges) {
        if (!edge.is_chain()) {
            continue;
        }
        outgoing[edge.from].push_back(edge.to);
        ++incoming[edge.to];
    }
    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (incoming[i] == 0) {
            queue.push_back(i);
        }
    }
    std::vector<std::size_t> depths(list.size());
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto from = queue[i];
        for (const auto to : outgoing[from]) {
            depths[to] = std::max(depths[to], depths[from] + 1);
            if (--incoming[to] == 0) {
                queue.push_back(to);
            }
        }
    }
    std::vector<std::size_t> depth_rows(list.size() + 1);
    std::vector<ImVec2> result;
    result.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& layout = field(list[i], "panel-settings");
        const auto depth = depths[i];
        const auto x = leading + depth * column_width;
        const auto y = leading + depth_rows[depth]++ * row_height;
        result.emplace_back(
            std::clamp(static_cast<float>(numeric_field(layout, "x", x)), -world_limit, world_limit),
            std::clamp(static_cast<float>(numeric_field(layout, "y", y)), -world_limit, world_limit));
    }
    return result;
}

void change(State& state)
{
    state.dirty = true;
    state.reply.reset();
    state.error.clear();
}

void move_node(State& state, std::size_t index, ImVec2 position, float dx, float dy)
{
    auto& layout = state.draft["nodes"][index]["panel-settings"];
    if (!layout.is_object()) {
        layout = Json::object();
    }
    layout["x"] = std::clamp(position.x + dx / state.zoom, -world_limit, world_limit);
    layout["y"] = std::clamp(position.y + dy / state.zoom, -world_limit, world_limit);
    change(state);
}

std::optional<std::size_t> node_at(const State& state, float x, float y)
{
    for (std::size_t i = state.node_bounds.size(); i > 0; --i) {
        if (state.node_bounds[i - 1].contains(x, y)) {
            return i - 1;
        }
    }
    return {};
}

bool makes_cycle(const Json& list, std::size_t source, std::size_t target)
{
    const auto edges = links(list);
    std::vector<bool> visited(list.size());
    std::vector<std::size_t> pending{target};
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto from = pending[i];
        if (from == source) {
            return true;
        }
        if (visited[from]) {
            continue;
        }
        visited[from] = true;
        for (const auto& edge : edges) {
            if (edge.from == from && edge.is_chain()) {
                pending.push_back(edge.to);
            }
        }
    }
    return false;
}

void connect(State& state, std::size_t source, std::size_t target)
{
    state.link_from.reset();
    const auto& list = nodes(state);
    if (name_of(list[target]).empty() || !can_receive(string_field(list[target], "type"))) {
        state.error = "This node cannot receive a next connection.";
        return;
    }
    if (makes_cycle(nodes(state), source, target)) {
        state.error = "This connection would make a circular chain.";
        return;
    }
    state.draft["nodes"][source]["next"] = name_of(nodes(state)[target]);
    state.selected = source;
    change(state);
}

void submit(State& state, PreviewSession& session, State::Request operation)
{
    state.operation = operation;
    state.load_requested = true;
    state.reply.reset();
    state.error.clear();
    Json request = {{"id", state.config_id}};
    if (operation != State::Request::load) {
        request["action"] = "save_core_config";
        request["config"] = operation == State::Request::save_import ? state.import_config : state.draft;
        request["revision"] = state.revision;
    } else {
        request["action"] = "get_core_config";
    }
    state.request = session.submit(std::move(request), request_timeout);
    if (!state.request) {
        state.error = "The configuration connection is unavailable.";
    }
}

void reset_graph_view(State& state)
{
    state.selected.reset();
    state.link_from.reset();
    state.grabbed.reset();
    state.last_clicked_node.reset();
    state.touch_node.reset();
    state.dragged = false;
    state.pressed_output = false;
    state.viewport = {};
    state.node_bounds.clear();
    state.pan_x = 0;
    state.pan_y = 0;
    state.zoom = 1;
}

void poll(State& state, PreviewSession& session)
{
    auto result = session.take_reply(state.request);
    if (!result) {
        return;
    }
    if (result->ok()) {
        const auto& config = field(result->result, "config");
        const auto& revision = field(result->result, "revision");
        if (!config.is_object() || !field(config, "nodes").is_array() || revision.is_null()) {
            state.error = "Wave did not return the configuration and its revision.";
            return;
        }
        state.saved = config;
        state.draft = config;
        state.revision = revision;
        state.dirty = false;
        if (state.operation == State::Request::save_import) {
            reset_graph_view(state);
        } else {
            state.link_from.reset();
            state.grabbed.reset();
            state.last_clicked_node.reset();
            state.touch_node.reset();
            if (state.selected && *state.selected >= nodes(state).size()) {
                state.selected.reset();
            }
        }
        if (state.operation == State::Request::load) {
            return;
        }
    } else if (result->status == PreviewReply::Status::timed_out) {
        result->message = "Wave did not confirm the request. Reload to check the saved configuration.";
    } else if (result->status == PreviewReply::Status::disconnected) {
        result->message = "The configuration connection closed. Reconnect to continue.";
    }
    state.reply = std::move(result);
}

bool config_active(const PreviewSession& session, int id)
{
    const auto& configs = array_field(field(field(session.data(), "server"), "core"), "configs");
    return std::ranges::any_of(configs, [id](const Json& config) {
        return integer_field(config, "id") == id && bool_field(config, "active");
    });
}

void add_text(ImDrawList& draw, ImVec2 where, ImU32 color, std::string_view value, float size,
              const ImVec4& clip)
{
    if (!value.empty()) {
        draw.AddText(ImGui::GetFont(), size, where, color, value.data(), value.data() + value.size(), 0,
                     &clip);
    }
}

void draw_link(ImDrawList& draw, ImVec2 start, ImVec2 end, ImU32 color)
{
    const float bend = std::max(22.f, std::abs(end.x - start.x) * 0.46f);
    draw.AddBezierCubic(start, {start.x + bend, start.y}, {end.x - bend, end.y}, end, color, 1.6f);
    draw.AddTriangleFilled(end, {end.x - 7, end.y - 4}, {end.x - 7, end.y + 4}, color);
}

void draw_links(ImDrawList& draw, const State& state, const std::vector<Link>& edges)
{
    std::vector<int> outputs(state.node_bounds.size());
    std::vector<int> inputs(state.node_bounds.size());
    for (const auto& edge : edges) {
        if (edge.setting) {
            ++outputs[edge.from];
            ++inputs[edge.to];
        }
    }
    std::vector<int> output_slots(outputs.size());
    std::vector<int> input_slots(inputs.size());
    const auto offset = [&](int slot, int count) {
        return (8.f + 15.f * slot / std::max(1, count - 1)) * state.zoom;
    };
    for (const auto& edge : edges) {
        const auto& from = state.node_bounds[edge.from];
        const auto& to = state.node_bounds[edge.to];
        ImVec2 start{from.right, (from.top + from.bottom) / 2};
        ImVec2 end{to.left, (to.top + to.bottom) / 2};
        if (!edge.setting) {
            draw_link(draw, start, end, packed(Color::info));
            continue;
        }
        start.y += offset(output_slots[edge.from]++, outputs[edge.from]);
        end.y += offset(input_slots[edge.to]++, inputs[edge.to]);
        const auto color = packed(edge.is_chain() ? Color::warning : Color::secondary_text);
        draw_link(draw, start, end, color);
        if (state.zoom >= 0.48f) {
            const auto* label = edge.setting->label;
            const auto size = std::clamp(12.f * state.zoom, 9.f, 16.f);
            const auto width = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, label).x;
            const ImVec2 text_at{(start.x + end.x - width) / 2, (start.y + end.y - size) / 2};
            draw.AddRectFilled({text_at.x - 2, text_at.y}, {text_at.x + width + 2, text_at.y + size},
                               packed(Color::canvas));
            draw.AddText(ImGui::GetFont(), size, text_at, color, label);
        }
    }
}

bool draw_graph(State& state, float height)
{
    const auto corner = ImGui::GetCursorScreenPos();
    state.canvas_origin = corner;
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const ImVec2 bottom{corner.x + width, corner.y + height};
    auto& draw = *ImGui::GetWindowDrawList();
    const auto mouse = ImGui::GetIO().MousePos;
    const auto& list = nodes(state);
    const auto edges = links(list);
    const auto ports = visible_ports(list, edges);
    const auto layout = positions(list, edges);
    draw.AddRectFilled(corner, bottom, packed(Color::canvas));
    draw.PushClipRect(corner, bottom, true);
    const auto visible_min = draw.GetClipRectMin();
    const auto visible_max = draw.GetClipRectMax();
    state.viewport = {visible_min.x, visible_min.y, visible_max.x, visible_max.y};
    state.node_bounds.clear();
    const auto zoom = state.zoom;
    for (const auto& pos : layout) {
        const auto left = corner.x + state.pan_x + zoom * pos.x;
        const auto top = corner.y + state.pan_y + zoom * pos.y;
        state.node_bounds.push_back({left, top, left + card_width * zoom, top + card_height * zoom});
    }
    const auto hit = node_at(state, mouse.x, mouse.y);
    const auto output_at = [&](std::size_t index) {
        const auto& box = state.node_bounds[index];
        return list[index].is_object() && ports[index].output && mouse.x >= box.right - 16 * zoom &&
               mouse.x <= box.right + 7 &&
               std::abs(mouse.y - (box.top + box.bottom) / 2) <= std::max(9.f, 9 * zoom);
    };
    const auto hit_output = [&]() -> std::optional<std::size_t> {
        for (std::size_t i = state.node_bounds.size(); i > 0; --i) {
            if (output_at(i - 1)) {
                return i - 1;
            }
        }
        return {};
    }();
    const bool clicked = ImGui::InvisibleButton("##node-graph", {width, height});
    bool edit = false;
    if (ImGui::IsItemActivated()) {
        state.pressed_output = state.graph_editable && hit_output.has_value();
        state.grabbed = state.pressed_output ? hit_output : hit;
        state.dragged = false;
        if (state.pressed_output || ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) != 2 ||
            state.last_clicked_node != hit) {
            state.last_clicked_node.reset();
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4)) {
        const auto delta = ImGui::GetIO().MouseDelta;
        state.dragged = true;
        if (!state.grabbed || !state.graph_editable) {
            state.pan_x += delta.x;
            state.pan_y += delta.y;
        } else if (!state.pressed_output && list[*state.grabbed].is_object()) {
            state.selected = state.grabbed;
            state.link_from.reset();
            move_node(state, *state.grabbed, layout[*state.grabbed], delta.x, delta.y);
        }
    }
    if (clicked && state.graph_editable && state.pressed_output && state.grabbed && state.dragged && hit &&
        hit != state.grabbed) {
        connect(state, *state.grabbed, *hit);
    } else if (clicked && !state.dragged) {
        if (state.graph_editable && state.link_from && hit && hit != state.link_from) {
            connect(state, *state.link_from, *hit);
        } else if (state.graph_editable && hit_output) {
            state.selected = hit_output;
            state.link_from = state.link_from == hit_output ? std::nullopt : hit_output;
        } else {
            state.link_from.reset();
            state.selected = hit;
            const bool card_click = state.graph_editable && hit && state.grabbed == hit &&
                                    !state.pressed_output && list[*hit].is_object();
            edit = card_click && state.last_clicked_node == hit;
            state.last_clicked_node = card_click ? hit : std::nullopt;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        state.link_from.reset();
    }

    constexpr float grid = 28;
    const auto grid_color = packed(Color::plot_grid);
    for (float x = corner.x + std::fmod(state.pan_x, grid); x < bottom.x; x += grid) {
        draw.AddLineV(std::round(x), corner.y, bottom.y, grid_color);
    }
    for (float y = corner.y + std::fmod(state.pan_y, grid); y < bottom.y; y += grid) {
        draw.AddLineH(corner.x, bottom.x, std::round(y), grid_color);
    }
    const auto accent = packed(Color::accent);
    draw_links(draw, state, edges);
    const auto preview_from = ImGui::IsItemActive() && state.pressed_output ? state.grabbed : state.link_from;
    if (preview_from && state.viewport.contains(mouse.x, mouse.y)) {
        const auto& bounds = state.node_bounds[*preview_from];
        draw_link(draw, {bounds.right, (bounds.top + bounds.bottom) / 2}, mouse, accent);
    }
    const auto border = packed(Color::control_border);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& box = state.node_bounds[i];
        const bool active = state.selected == i;
        const ImVec2 left{box.left, box.top};
        const ImVec2 right{box.right, box.bottom};
        draw.AddRectFilled(left, right, packed(active ? Color::selected : Color::raised),
                           theme::control_radius);
        draw.AddRect(left, right, active ? accent : border, theme::control_radius,
                     active ? theme::selection_width : theme::border_width);
        const float mid = (box.top + box.bottom) / 2;
        const float dot = std::clamp(4 * zoom, 2.f, 6.f);
        if (ports[i].input) {
            draw.AddCircleFilled({box.left, mid}, dot, packed(Color::info));
        }
        if (ports[i].output) {
            draw.AddCircleFilled({box.right, mid}, dot, state.link_from == i ? accent : packed(Color::info));
        }
        if (zoom >= 0.48f) {
            const ImVec4 clip{box.left + 6 * zoom, box.top, box.right - 6 * zoom, box.bottom};
            add_text(draw, {clip.x, box.top + 9 * zoom}, packed(Color::primary_text), name_of(list[i]),
                     std::clamp(16 * zoom, 9.f, 22.f), clip);
            add_text(draw, {clip.x, box.top + 32 * zoom}, packed(Color::secondary_text),
                     string_field(list[i], "type"), std::clamp(13 * zoom, 8.f, 18.f), clip);
        }
    }
    if (list.empty()) {
        constexpr char message[] = "No nodes in this configuration";
        const auto size = ImGui::CalcTextSize(message);
        draw.AddText({corner.x + (width - size.x) / 2, corner.y + (height - size.y) / 2},
                     packed(Color::secondary_text), message);
    }
    draw.PopClipRect();
    draw.AddRect(corner, bottom, border);
    return edit;
}

constexpr char add_title[] = "Add node";
constexpr char edit_title[] = "Edit node";
constexpr char remove_title[] = "Delete node";
constexpr char json_title[] = "Saved configuration JSON";
constexpr char edit_json_title[] = "Edit configuration JSON";
constexpr char import_title[] = "Resolve and save configuration";
constexpr char import_notice[] =
    "Wave replaces variables with their values and discards variable definitions, "
    "comments, and commits metadata. Saving replaces this config’s nodes.";
constexpr char reload_title[] = "Reload configuration";

const char* title(State::Dialog dialog)
{
    switch (dialog) {
    case State::Dialog::add:
        return add_title;
    case State::Dialog::edit:
        return edit_title;
    case State::Dialog::remove:
        return remove_title;
    case State::Dialog::json:
        return json_title;
    case State::Dialog::edit_json:
        return edit_json_title;
    case State::Dialog::confirm_import:
        return import_title;
    case State::Dialog::reload:
        return reload_title;
    case State::Dialog::none:
        return "";
    }
    return "";
}

void open_dialog(State& state, State::Dialog dialog)
{
    state.dialog = dialog;
    state.error.clear();
    state.link_from.reset();
    state.last_clicked_node.reset();
    if (dialog == State::Dialog::add) {
        state.node_name.clear();
        state.node_type.clear();
        state.node_next.clear();
        state.settings_text = "{}";
    } else if (dialog == State::Dialog::edit) {
        const auto& node = nodes(state)[*state.selected];
        state.node_name = name_of(node);
        state.node_type = string_field(node, "type");
        state.node_next = string_field(node, "next");
        const auto& settings = field(node, "settings");
        state.settings_text = settings.is_null() ? "{}" : settings.dump(2);
    } else if (dialog == State::Dialog::json) {
        state.json_text = state.saved.dump(2);
    } else if (dialog == State::Dialog::edit_json) {
        state.json_text = state.draft.dump(2);
        state.import_config = {};
    } else if (dialog == State::Dialog::confirm_import) {
        state.reply.reset();
    }
    ImGui::OpenPopup(title(dialog));
}

void close_dialog(State& state)
{
    ImGui::CloseCurrentPopup();
    if (state.dialog == State::Dialog::confirm_import) {
        state.import_config = {};
    }
    state.dialog = State::Dialog::none;
    state.error.clear();
}

Json parse_graph_json(std::string_view text, bool& too_deep)
{
    return Json::parse(
        text,
        [&too_deep](int depth, auto, auto&) {
            too_deep |= depth > max_core_config_depth;
            return !too_deep;
        },
        false);
}

bool apply_node(State& state, bool adding)
{
    auto& list = state.draft["nodes"];
    const auto name = std::string_view(state.node_name);
    const auto duplicate = find_node(list, name);
    const auto current = adding ? std::optional<std::size_t>{} : state.selected;
    if (!valid_core_node_name(name)) {
        state.error = "Enter a node name of up to 128 bytes without control characters.";
        return false;
    }
    if (duplicate && duplicate != current) {
        state.error = "A node already uses this name.";
        return false;
    }
    if (state.node_type.empty()) {
        state.error = "Choose a node type.";
        return false;
    }
    bool too_deep = false;
    auto settings = parse_graph_json(state.settings_text, too_deep);
    if (too_deep || !settings.is_object()) {
        state.error = too_deep ? "JSON can contain up to 64 levels." : "Node settings must be a JSON object.";
        return false;
    }
    const bool allow_next = can_send(state.node_type, settings);
    const auto next = allow_next ? find_node(list, state.node_next) : std::nullopt;
    if (current && next && makes_cycle(list, *current, *next)) {
        state.error = "This connection would make a circular chain.";
        return false;
    }
    const auto previous_name = current ? std::string(name_of(list[*current])) : std::string{};
    auto& node = adding ? list.emplace_back(Json::object()) : list[*current];
    const auto previous_settings = node.find("settings");
    node["name"] = state.node_name;
    node["type"] = state.node_type;
    if (!settings.empty() || previous_settings != node.end() || adding) {
        node["settings"] = std::move(settings);
    }
    if (state.node_next.empty() || !allow_next) {
        node.erase("next");
    } else {
        node["next"] = state.node_next;
    }
    if (current && previous_name != name && !previous_name.empty()) {
        for (auto& entry : list) {
            rename_links(entry, previous_name, name);
        }
    }
    state.selected = adding ? list.size() - 1 : *current;
    change(state);
    return true;
}

bool apply_config_json(State& state)
{
    auto [document, needs_confirmation, error] = parse_config_json_import(state.json_text);
    if (!error.empty()) {
        state.error = std::move(error);
        return false;
    }
    auto& imported = document.is_object() ? document["nodes"] : document;
    if (!imported.is_array()) {
        state.error = "Paste a JSON array or a config.json with a nodes array.";
        return false;
    }

    std::set<std::string_view> names;
    std::set<std::string_view> local_database_paths;
    for (const auto& node : array_field(state.saved, "nodes")) {
        const auto path = string_field(field(node, "settings"), "db-path");
        if (string_field(node, "type") == "AuthenticationServer" && !path.empty()) {
            local_database_paths.insert(path);
        }
    }
    for (auto& node : imported) {
        const auto name = name_of(node);
        const auto type = string_field(node, "type");
        if (!valid_core_node_name(name) || !valid_core_node_name(type)) {
            state.error = "Each node needs a valid name and type of up to 128 bytes.";
            return false;
        }
        if (!names.insert(name).second) {
            state.error = std::format("Duplicate node name: {}.", name);
            return false;
        }
        // Wave assigns local database paths on save. Keep paths already used by this config.
        auto settings = node.find("settings");
        if (type == "AuthenticationServer" && settings != node.end() && settings->is_object() &&
            !local_database_paths.contains(string_field(*settings, "db-path"))) {
            settings->erase("db-path");
        }
    }

    if (!needs_confirmation && imported == nodes(state)) {
        return true;
    }
    auto draft = state.draft;
    draft["nodes"] = std::move(imported);
    if (needs_confirmation) {
        draft.erase("variables");
        draft.erase("commits");
    }
    if (draft.dump(2).size() + 1 > max_core_config_bytes) {
        state.error = "Wave can save configs up to 256 KiB.";
        return false;
    }
    if (needs_confirmation) {
        state.import_config = std::move(draft);
        return true;
    }
    state.draft = std::move(draft);
    reset_graph_view(state);
    change(state);
    state.dirty = state.draft != state.saved;
    return true;
}

bool select_target(const State& state, const char* label, std::string& target, bool adding,
                   const NodeLinkField* setting = nullptr)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    bool changed = false;
    if (ImGui::BeginCombo("##target", target.empty() ? "None" : target.c_str())) {
        if (ImGui::Selectable("None", target.empty()) && !target.empty()) {
            target.clear();
            changed = true;
        }
        const auto& list = nodes(state);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& node = list[i];
            const auto name = name_of(node);
            const auto type = string_field(node, "type");
            const bool compatible =
                setting ? node_link_target_type_matches(*setting, type, field(node, "settings"))
                        : can_receive(type);
            if (name.empty() || (!adding && state.selected == i) || !compatible) {
                continue;
            }
            ImGui::PushID(static_cast<int>(i));
            const auto pos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##node", target == name, 0, {0, ImGui::GetTextLineHeight()}) &&
                target != name) {
                target = name;
                changed = true;
            }
            ImGui::GetWindowDrawList()->AddText(pos, packed(Color::primary_text), name.data(),
                                                name.data() + name.size());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

void edit_setting_links(State& state, Json& settings, bool adding)
{
    if (!settings.is_object()) {
        return;
    }
    for (const auto& setting : node_link_fields(state.node_type)) {
        const auto& container = setting.object ? field(settings, setting.object) : settings;
        if (setting.collection || (!container.is_null() && !container.is_object())) {
            continue;
        }
        const auto* key = node_link_key(container, setting);
        auto target = std::string(string_field(container, key));
        if (select_target(state, setting.label, target, adding, &setting)) {
            auto& editable = setting.object ? settings[setting.object] : settings;
            if (target.empty()) {
                erase_node_link(editable, setting);
            } else {
                editable[key] = target;
            }
            state.settings_text = settings.dump(2);
        }
    }
}

void edit_node(State& state, bool adding)
{
    Dialog popup(title(state.dialog), 560, {{"Cancel"}, {adding ? "Add node" : "Apply changes"}});
    if (!popup) {
        return;
    }
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##node-name", &state.node_name);
    ImGui::TextUnformatted("Type");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##node-type", state.node_type.empty() ? "Choose a WaterWall node type"
                                                                 : state.node_type.c_str())) {
        for (const auto type : waterwall_node_types) {
            if (ImGui::Selectable(type.data(), state.node_type == type)) {
                state.node_type = type;
            }
        }
        ImGui::EndCombo();
    }
    const bool router = state.node_type == "Router" || state.node_type == "SniffRouter";
    bool too_deep = false;
    auto settings =
        node_link_fields(state.node_type).empty() ? Json{} : parse_graph_json(state.settings_text, too_deep);
    if (!too_deep) {
        edit_setting_links(state, settings, adding);
    }
    if (can_send(state.node_type, settings)) {
        select_target(state, router ? "Default next node" : "Next node", state.node_next, adding);
    }
    ImGui::TextUnformatted("Settings JSON");
    if (router) {
        wrapped(state.node_type == "Router" ? "Set conditional branches in rules using target."
                                            : "Set conditional branches in routes using next.",
                Color::secondary_text);
    }
    ImGui::InputTextMultiline("##node-settings", &state.settings_text, {-1, 160});
    if (!state.error.empty()) {
        wrapped(state.error, Color::error);
    }
    const auto action = popup.footer();
    if (action == 0 || (action == 1 && apply_node(state, adding))) {
        close_dialog(state);
    }
}

void render_dialog(State& state, PreviewSession& session)
{
    if (state.dialog == State::Dialog::none) {
        return;
    }
    if (!ImGui::IsPopupOpen(title(state.dialog))) {
        if (state.dialog == State::Dialog::confirm_import) {
            state.import_config = {};
        }
        state.dialog = State::Dialog::none;
        state.error.clear();
        return;
    }
    if (state.dialog == State::Dialog::add || state.dialog == State::Dialog::edit) {
        if (state.graph_editable) {
            edit_node(state, state.dialog == State::Dialog::add);
        } else {
            Dialog popup(title(state.dialog), 440, {{"Close"}});
            if (popup) {
                wrapped("The configuration is read-only.", Color::warning);
                if (popup.footer() == 0) {
                    close_dialog(state);
                }
            }
        }
        return;
    }
    if (state.dialog == State::Dialog::json) {
        Dialog popup(json_title, 740, {{"Close"}, {"Copy JSON"}});
        if (popup) {
            ImGui::InputTextMultiline("##saved-json", &state.json_text, {-1, 340},
                                      ImGuiInputTextFlags_ReadOnly);
            const auto action = popup.footer();
            if (action == 0) {
                close_dialog(state);
            } else if (action == 1) {
                ImGui::SetClipboardText(state.json_text.c_str());
            }
        }
        return;
    }
    if (state.dialog == State::Dialog::edit_json) {
        bool confirm_import = false;
        {
            Dialog popup(edit_json_title, 740, {{"Cancel"}, {"Apply nodes", !state.graph_editable}});
            if (popup) {
                wrapped("Paste a config.json or a nodes array. Applying replaces this config’s draft nodes.");
                ImGui::BeginDisabled(!state.graph_editable);
                ImGui::InputTextMultiline("##config-json", &state.json_text, {-1, 340});
                ImGui::EndDisabled();
                if (!state.error.empty()) {
                    wrapped(state.error, Color::error);
                }
                const auto action = popup.footer();
                if (action == 0 || (action == 1 && apply_config_json(state))) {
                    confirm_import = state.import_config.is_object();
                    close_dialog(state);
                }
            }
        }
        if (confirm_import) {
            open_dialog(state, State::Dialog::confirm_import);
        }
        return;
    }
    if (state.dialog == State::Dialog::confirm_import) {
        const bool pending = state.request.has_value();
        Dialog popup(import_title, 500, {{"Cancel", pending}, {"Resolve and save", !state.graph_editable}});
        if (popup) {
            if (state.reply && state.reply->ok()) {
                close_dialog(state);
                return;
            }
            wrapped(import_notice);
            if (pending) {
                wrapped("Saving configuration…", Color::info);
            } else if (state.reply) {
                feedback(*state.reply);
            }
            if (!state.error.empty()) {
                wrapped(state.error, Color::error);
            }
            const auto action = popup.footer();
            if (action == 0) {
                close_dialog(state);
            } else if (action == 1) {
                submit(state, session, State::Request::save_import);
            }
        }
        return;
    }
    if (state.dialog == State::Dialog::reload) {
        Dialog popup(reload_title, 460, {{"Cancel"}, {"Reload", !session.ready()}});
        if (popup) {
            wrapped("Reload the saved configuration and discard your unsaved changes?");
            const auto action = popup.footer();
            if (action >= 0) {
                close_dialog(state);
                if (action == 1) {
                    submit(state, session, State::Request::load);
                }
            }
        }
        return;
    }
    auto& list = state.draft["nodes"];
    const auto name = name_of(list[*state.selected]);
    const auto referring_node = settings_reference_from(list, name);
    Dialog popup(remove_title, 460,
                 {{"Cancel"}, {"Delete node", !state.graph_editable || !referring_node.empty()}});
    if (!popup) {
        return;
    }
    if (!referring_node.empty()) {
        wrapped(
            std::format("Remove the reference in {}'s settings before deleting {}.", referring_node, name),
            Color::warning);
    } else {
        wrapped(std::format("Delete {} and its connections from this configuration?", name));
    }
    const auto action = popup.footer();
    if (action == 1) {
        for (auto& entry : list) {
            if (!name.empty() && string_field(entry, "next") == name) {
                entry.erase("next");
            }
        }
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(*state.selected));
        state.selected.reset();
        change(state);
    }
    if (action >= 0) {
        close_dialog(state);
    }
}

void zoom_by(State& state, float factor, ImVec2 anchor = {})
{
    const auto zoom = std::clamp(state.zoom * factor, zoom_min, zoom_max);
    const float scale_delta = zoom / state.zoom - 1.f;
    state.pan_x += (state.pan_x - anchor.x) * scale_delta;
    state.pan_y += (state.pan_y - anchor.y) * scale_delta;
    state.zoom = zoom;
}

void fit_graph(State& state, float width, float height)
{
    const auto layout = positions(nodes(state), links(nodes(state)));
    if (layout.empty()) {
        return;
    }
    const auto [left, right] = std::ranges::minmax_element(layout, {}, &ImVec2::x);
    const auto [top, bottom] = std::ranges::minmax_element(layout, {}, &ImVec2::y);
    constexpr float inset = 24;
    const auto zoom_x = std::max(1.f, width - 2 * inset) / (right->x - left->x + card_width);
    const auto zoom_y = std::max(1.f, height - 2 * inset) / (bottom->y - top->y + card_height);
    state.zoom = std::clamp(std::min(zoom_x, zoom_y), zoom_min, 1.f);
    state.pan_x = inset - left->x * state.zoom;
    state.pan_y = inset - top->y * state.zoom;
}

struct GraphStatus {
    std::string_view message;
    Color color = Color::secondary_text;
};

GraphStatus graph_status(const State& state, bool active)
{
    if (state.request) {
        return {state.operation == State::Request::load ? "Loading configuration…" : "Saving configuration…"};
    }
    if (!state.error.empty() && state.dialog == State::Dialog::none) {
        return {state.error, Color::error};
    }
    if (state.reply && !state.reply->ok() && !state.reply->message.empty()) {
        return {state.reply->message, Color::error};
    }
    if (active) {
        return {"Stop the core to edit this configuration.", Color::warning};
    }
    if (state.link_from) {
        return {"Select the node to connect."};
    }
    if (state.reply && !state.reply->message.empty()) {
        return {state.reply->message, Color::success};
    }
    return {state.dirty ? "Unsaved changes" : ""};
}

bool draw_menu_status(GraphStatus status)
{
    if (status.message.empty() || status.message.find('\n') != std::string_view::npos) {
        return false;
    }
    const auto start = status.message.data();
    const auto end = start + status.message.size();
    const auto cursor = ImGui::GetCursorScreenPos();
    const auto& style = ImGui::GetStyle();
    auto& draw = *ImGui::GetWindowDrawList();
    const float x = draw.GetClipRectMax().x - style.FramePadding.x - ImGui::CalcTextSize(start, end).x;
    if (x < cursor.x + style.ItemSpacing.x) {
        return false;
    }
    draw.AddText({x, cursor.y + style.FramePadding.y}, packed(status.color), start, end);
    return true;
}

void render_graph_pane(State& state, PreviewSession& session, bool active, bool narrow)
{
    const float height =
        std::max((narrow ? 230.f : 280.f) + ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y);
    auto dialog = State::Dialog::none;
    constexpr auto flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("Graph pane", {0, height}, ImGuiChildFlags_NavFlattened, flags)) {
        bool fit = false;
        GraphStatus status;
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Nodes")) {
                const bool selected = state.selected && *state.selected < nodes(state).size();
                if (ImGui::MenuItem("Add node", nullptr, false, state.graph_editable)) {
                    dialog = State::Dialog::add;
                }
                if (ImGui::MenuItem("Edit node", nullptr, false, state.graph_editable && selected)) {
                    dialog = State::Dialog::edit;
                }
                if (ImGui::MenuItem("Delete node", nullptr, false, state.graph_editable && selected)) {
                    dialog = State::Dialog::remove;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Config")) {
                if (ImGui::MenuItem("Save changes", nullptr, false, state.graph_editable && state.dirty)) {
                    submit(state, session, State::Request::save);
                }
                if (ImGui::MenuItem("Edit JSON", nullptr, false, state.graph_editable)) {
                    dialog = State::Dialog::edit_json;
                }
                if (ImGui::MenuItem("View saved JSON")) {
                    dialog = State::Dialog::json;
                }
                if (ImGui::MenuItem("Reload", nullptr, false, !state.request && session.ready())) {
                    if (state.dirty) {
                        dialog = State::Dialog::reload;
                    } else {
                        submit(state, session, State::Request::load);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Zoom in", nullptr, false, state.zoom < zoom_max)) {
                    zoom_by(state, zoom_step);
                }
                if (ImGui::MenuItem("Zoom out", nullptr, false, state.zoom > zoom_min)) {
                    zoom_by(state, 1.f / zoom_step);
                }
                fit = ImGui::MenuItem("Fit graph", nullptr, false, !nodes(state).empty());
                ImGui::EndMenu();
            }
            status = graph_status(state, active);
            if (draw_menu_status(status)) {
                status.message = {};
            }
            ImGui::EndMenuBar();
        }
        if (!status.message.empty()) {
            wrapped(status.message, status.color);
        }
        const auto available = ImGui::GetContentRegionAvail();
        const float canvas_height = std::max(1.f, available.y);
        if (fit) {
            fit_graph(state, available.x, canvas_height);
        }
        if (draw_graph(state, canvas_height)) {
            dialog = State::Dialog::edit;
        }
    }
    ImGui::EndChild();
    if (dialog != State::Dialog::none) {
        open_dialog(state, dialog);
    }
}

} // namespace

bool ConfigNodesState::Bounds::contains(float x, float y) const
{
    return x >= left && x < right && y >= top && y < bottom;
}

bool ConfigNodesState::zoom_at(float x, float y, float steps)
{
    if (!viewport.contains(x, y)) {
        return false;
    }
    zoom_by(*this, std::pow(zoom_step, steps), {x - canvas_origin.x, y - canvas_origin.y});
    return true;
}

bool ConfigNodesState::begin_touch(float x, float y)
{
    if (dialog != Dialog::none || !viewport.contains(x, y)) {
        return false;
    }
    touch_node = graph_editable ? node_at(*this, x, y) : std::nullopt;
    return true;
}

void ConfigNodesState::drag_touch(float dx, float dy)
{
    if (touch_node && graph_editable && !request && *touch_node < nodes(*this).size() &&
        nodes(*this)[*touch_node].is_object()) {
        auto layout = positions(nodes(*this), links(nodes(*this)));
        move_node(*this, *touch_node, layout[*touch_node], dx, dy);
        selected = touch_node;
        link_from.reset();
    } else {
        pan_x += dx;
        pan_y += dy;
    }
}

void render_config_nodes(ConfigNodesState& state, PreviewSession& session, int config_id, bool narrow)
{
    if (state.config_id != config_id) {
        session.abandon(state.request);
        state = {};
        state.config_id = config_id;
    }
    state.viewport = {};
    if (!state.load_requested && session.ready()) {
        submit(state, session, State::Request::load);
    }
    poll(state, session);
    if (session.status() == PreviewStatus::unavailable) {
        wrapped("The configuration connection is unavailable.", Color::warning);
        if (theme::button("Reconnect")) {
            state.load_requested = state.saved.is_object();
            state.reply.reset();
            state.error.clear();
            session.start(true);
        }
    }
    if (!state.saved.is_object()) {
        if (state.request || session.status() == PreviewStatus::loading) {
            wrapped("Loading configuration…", Color::secondary_text);
        } else if (session.ready()) {
            wrapped("The configuration could not be loaded.", Color::warning);
            if (theme::button("Retry")) {
                submit(state, session, State::Request::load);
            }
        }
        if (state.reply) {
            feedback(*state.reply);
        }
        if (!state.error.empty()) {
            wrapped(state.error, Color::error);
        }
        return;
    }

    const bool active = config_active(session, config_id);
    state.graph_editable = !active && !state.request && session.ready();
    render_graph_pane(state, session, active, narrow);
    render_dialog(state, session);
}

std::string describe_config_nodes(const ConfigNodesState& state)
{
    if (!state.saved.is_object()) {
        if (state.reply && !state.reply->message.empty()) {
            return "Nodes. " + state.reply->message;
        }
        return state.error.empty() ? "Nodes. Loading configuration." : "Nodes. " + state.error;
    }
    const auto& list = nodes(state);
    const auto count = list.size();
    auto description = std::format("Nodes. {} {}.", count, count == 1 ? "node" : "nodes");
    if (state.selected && *state.selected < count) {
        const auto& node = list[*state.selected];
        description += std::format(" Selected {}. Type {}.", name_of(node), string_field(node, "type"));
        if (const auto next = string_field(node, "next"); can_send(node) && !next.empty()) {
            description += std::format(" Next {}.", next);
        }
        visit_node_links(node, [&](const Json& container, const NodeLinkField& setting, const char* key) {
            if (const auto target = string_field(container, key);
                node_link_active(container, setting) && !target.empty()) {
                description += std::format(" {} {}.", setting.label, target);
            }
        });
    }
    if (state.dialog != State::Dialog::none) {
        description += std::format(" {}.", title(state.dialog));
        if (state.dialog == State::Dialog::confirm_import) {
            description += std::string(" ") + import_notice;
        }
    }
    if (state.dirty) {
        description += " Unsaved changes.";
    }
    if (state.reply && !state.reply->message.empty()) {
        description += ' ' + state.reply->message;
    }
    if (!state.error.empty()) {
        description += ' ' + state.error;
    }
    return description;
}

} // namespace wave
