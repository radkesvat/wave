#include "config_nodes_view.h"

#include "browser_ui.h"
#include "config_json_import.h"
#include "core_config_json.h"
#include "dialog.h"
#include "view_helpers.h"
#include "waterwall_node_links.h"
#include "waterwall_node_settings.h"
#include "waterwall_schematic.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <limits>
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
namespace spec = node_settings;

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
constexpr char schematic_unavailable[] = "The WaterWall schematic is unavailable.";

const Json& nodes(const State& state)
{
    return array_field(state.draft, "nodes");
}

std::string_view name_of(const Json& node)
{
    return string_field(node, "name");
}

bool can_send(const Json& node)
{
    return allows_node_next(string_field(node, "type"), field(node, "settings"));
}

std::string_view database_key(std::string_view type)
{
    const auto& node = waterwall_node(type);
    return bool_field(node, "user_database") ? string_field(node, "database_key") : std::string_view{};
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
        const bool adapter = bool_field(field(waterwall_node(type), "ports"), "dual_ended");
        const auto connected = ports[i];
        // A device can be placed at either end. Show both choices until it is connected.
        ports[i] = {allows_node_previous(type) && (!adapter || connected.input || !connected.output),
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
    if (name_of(list[target]).empty() || !allows_node_previous(string_field(list[target], "type"))) {
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
    if (!state.request && operation == State::Request::load) {
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
    const bool loading = state.operation == State::Request::load;
    if (result->ok()) {
        const auto& config = field(result->result, "config");
        const auto& revision = field(result->result, "revision");
        if (!config.is_object() || !field(config, "nodes").is_array() || revision.is_null()) {
            if (loading) {
                state.error = "Wave did not return the configuration and its revision.";
            } else {
                session.add_local_log(
                    "error",
                    "Wave did not return the saved configuration and its revision. Reload before retrying.");
            }
            return;
        }
        state.saved = config;
        state.draft = config;
        state.revision = revision;
        state.dirty = false;
        state.context_node.reset();
        if (state.operation == State::Request::save_import) {
            reset_graph_view(state);
            state.reply = PreviewReply{PreviewReply::Status::accepted, {}};
        } else {
            state.link_from.reset();
            state.grabbed.reset();
            state.last_clicked_node.reset();
            state.touch_node.reset();
            if (state.selected && *state.selected >= nodes(state).size()) {
                state.selected.reset();
            }
        }
    } else if (loading) {
        if (result->status == PreviewReply::Status::timed_out) {
            result->message = "Wave did not confirm the request. Reload to check the saved configuration.";
        } else if (result->status == PreviewReply::Status::disconnected) {
            result->message = "The configuration connection closed. Reconnect to continue.";
        }
        state.reply = std::move(result);
    }
}

bool config_active(const PreviewSession& session, int id)
{
    const auto& configs = array_field(field(field(session.data(), "server"), "core"), "configs");
    return std::ranges::any_of(configs, [id](const Json& config) {
        return field(config, "id") == id && bool_field(config, "active");
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

void draw_graph_corners(ImDrawList& draw, const ImGuiWindow& pane)
{
    const auto bounds = pane.Rect();
    const float radius =
        std::min(pane.WindowRounding, std::min(bounds.GetWidth(), bounds.GetHeight()) / 2 - 1);
    if (radius < 0.5f) {
        return;
    }

    // ImGui clips child content to a rectangle. Cover the parts outside the pane's curves.
    const auto background = packed(Color::canvas);
    draw.PathLineTo({bounds.Min.x, bounds.Max.y});
    draw.PathArcToFast({bounds.Min.x + radius, bounds.Max.y - radius}, radius, 6, 3);
    draw.PathFillConcave(background);
    draw.PathLineTo({bounds.Max.x, bounds.Max.y});
    draw.PathArcToFast({bounds.Max.x - radius, bounds.Max.y - radius}, radius, 3, 0);
    draw.PathFillConcave(background);
    // The pane border was drawn before the graph.
    draw.AddRect(bounds.Min, bounds.Max, packed(Color::divider), radius, pane.WindowBorderSize,
                 ImDrawFlags_RoundBottom);
}

bool draw_graph(State& state, float height, const ImGuiWindow& pane)
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
    draw_graph_corners(draw, pane);
    draw.PopClipRect();
    return edit;
}

constexpr char add_title[] = "Add node";
constexpr char edit_title[] = "Edit node";
constexpr char remove_title[] = "Delete node";
constexpr char json_title[] = "Saved configuration JSON";
constexpr char edit_json_title[] = "Edit configuration JSON";
constexpr char import_title[] = "Resolve and save configuration";
constexpr char reload_title[] = "Reload configuration";

const Json& import_schematic()
{
    return field(field(waterwall_schematic(), "config"), "import");
}

std::string_view import_notice()
{
    return string_field(import_schematic(), "confirm_notice", "Saving replaces this config’s nodes.");
}

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
        state.custom_json = false;
        state.settings_text = "{}";
        state.invalid_setting_inputs.clear();
    } else if (dialog == State::Dialog::edit) {
        const auto& node = nodes(state)[*state.selected];
        state.node_name = name_of(node);
        state.node_type = string_field(node, "type");
        state.node_next = string_field(node, "next");
        state.custom_json = bool_field(field(node, "panel-settings"), "custom-json");
        const auto& settings = field(node, "settings");
        state.settings_text = settings.is_null() ? "{}" : settings.dump(2);
        state.invalid_setting_inputs.clear();
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

void close_dialog(State& state, bool preserve_error = false)
{
    ImGui::CloseCurrentPopup();
    if (state.dialog == State::Dialog::json || state.dialog == State::Dialog::edit_json) {
        close_config_json_editor();
    } else if (state.dialog == State::Dialog::confirm_import) {
        state.import_config = {};
    }
    state.dialog = State::Dialog::none;
    if (!preserve_error) {
        state.error.clear();
    }
}

Json parse_node_settings(std::string_view text, std::string& error)
{
    constexpr std::string_view invalid = "Node settings must be a JSON object.";
    error.clear();
    if (text.size() > max_core_config_bytes) {
        error = "Wave can save configs up to 256 KiB.";
        return {};
    }
    auto settings = parse_plain_core_config_json(text, invalid, error);
    if (error.empty() && !settings.is_object()) {
        error = invalid;
    }
    return settings;
}

ImVec2 centered_node_position(const State& state, float canvas_top)
{
    const auto visible = visible_canvas();
    auto viewport = state.viewport;
    viewport.top = std::max(canvas_top, ImGui::GetWindowDrawList()->GetClipRectMin().y);
    const Bounds clipped{
        std::max(viewport.left, visible.left),
        std::max(viewport.top, visible.top),
        std::min(viewport.right, visible.left + visible.width),
        std::min(viewport.bottom, visible.top + visible.height),
    };
    const auto& camera = clipped.right > clipped.left && clipped.bottom > clipped.top ? clipped : viewport;
    const float x = ((camera.left + camera.right) / 2 - state.canvas_origin.x - state.pan_x) / state.zoom;
    const float y = ((camera.top + camera.bottom) / 2 - canvas_top - state.pan_y) / state.zoom;
    return {std::clamp(x - card_width / 2, -world_limit, world_limit),
            std::clamp(y - card_height / 2, -world_limit, world_limit)};
}

bool apply_node(State& state, bool adding)
{
    auto draft = state.draft;
    auto& list = draft["nodes"];
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
    auto settings = parse_node_settings(state.settings_text, state.error);
    if (!state.error.empty()) {
        return false;
    }
    if (!state.custom_json) {
        if (!state.invalid_setting_inputs.empty()) {
            const auto& [path, input] = *state.invalid_setting_inputs.begin();
            state.error =
                std::format("Invalid setting {}. {}", std::string_view(path).substr(1), input.error);
            return false;
        }
        state.error = spec::validate(state.node_type, settings);
        if (!state.error.empty()) {
            return false;
        }
    }
    const bool allow_next = allows_node_next(state.node_type, settings);
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
    if (adding) {
        const auto position = state.add_position;
        node["panel-settings"] = {{"x", position.x}, {"y", position.y}};
    }
    auto panel = node.find("panel-settings");
    if (state.custom_json) {
        if (panel != node.end() && !panel->is_object()) {
            state.error = "Node panel-settings must be an object.";
            return false;
        }
        node["panel-settings"]["custom-json"] = true;
    } else if (panel != node.end() && bool_field(*panel, "custom-json")) {
        panel->erase("custom-json");
        if (panel->empty()) {
            node.erase(panel);
        }
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
    const auto serialized = draft.dump(2);
    if (serialized.size() + 1 > max_core_config_bytes) {
        state.error = "Wave can save configs up to 256 KiB.";
        return false;
    }
    parse_plain_core_config_json(serialized, "Enter valid configuration JSON.", state.error);
    if (!state.error.empty()) {
        return false;
    }
    state.selected = adding ? list.size() - 1 : *current;
    state.draft = std::move(draft);
    change(state);
    return true;
}

bool apply_config_json(State& state)
{
    const auto text = read_config_json_editor_text();
    if (!text) {
        state.error = "Wave could not read the JSON editor.";
        return false;
    }
    auto [document, needs_confirmation, error] = parse_config_json_import(*text);
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
        const auto key = database_key(string_field(node, "type"));
        if (!key.empty()) {
            const auto path = string_field(field(node, "settings"), key);
            if (!path.empty()) {
                local_database_paths.insert(path);
            }
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
        const auto key = database_key(type);
        if (!key.empty() && settings != node.end() && settings->is_object() &&
            !local_database_paths.contains(string_field(*settings, key))) {
            settings->erase(std::string(key));
        }
    }

    if (!needs_confirmation && imported == nodes(state)) {
        return true;
    }
    auto draft = state.draft;
    draft["nodes"] = std::move(imported);
    if (needs_confirmation) {
        const auto& import = import_schematic();
        for (auto key : {string_field(import, "variables_key"), string_field(import, "commit_key")}) {
            if (!key.empty()) {
                draft.erase(std::string(key));
            }
        }
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
                   std::string_view help, const NodeLinkField* setting = nullptr,
                   bool empty_value_is_present = false)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    help_marker(help);
    ImGui::SetNextItemWidth(-1);
    bool changed = false;
    const auto* preview =
        target.empty() ? (empty_value_is_present ? "Invalid value" : "None") : target.c_str();
    if (ImGui::BeginCombo("##target", preview)) {
        if (ImGui::Selectable("None", target.empty() && !empty_value_is_present) &&
            (!target.empty() || empty_value_is_present)) {
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
                        : allows_node_previous(type);
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

bool is_link_container(std::string_view path, const NodeLinkField& link)
{
    for (const char* group : {link.object, link.collection}) {
        if (group) {
            if (!path.starts_with('/')) {
                return false;
            }
            path.remove_prefix(1);
            const auto delimiter = path.find('/');
            if (path.substr(0, delimiter) != group) {
                return false;
            }
            path.remove_prefix(delimiter == path.npos ? path.size() : delimiter);
        }
    }
    return link.collection ? path.starts_with('/') && path.size() > 1 &&
                                 path.substr(1).find_first_not_of("0123456789") == path.npos
                           : path.empty();
}

bool edit_setting_link(const State& state, Json& settings, bool adding, const NodeLinkField& setting,
                       std::string_view hint = {})
{
    const auto* key = node_link_key(settings, setting);
    auto target = std::string(string_field(settings, key));
    const std::string_view link_hint = setting.hint && *setting.hint ? setting.hint : hint;
    auto help = std::format("{}\n{}", key,
                            link_hint.empty() ? "Choose another node from this configuration." : link_hint);
    if (setting.activation_setting) {
        help += std::format("\nApplies when {} is {}.", setting.activation_setting,
                            setting.activation_is_boolean ? "enabled" : "set");
    }
    if (!select_target(state, setting.label, target, adding, help, &setting, settings.contains(key))) {
        return false;
    }
    if (target.empty()) {
        erase_node_link(settings, setting);
    } else {
        settings[key] = target;
    }
    return true;
}

std::string setting_help(const spec::Field& setting, std::string_view key)
{
    using enum spec::Kind;
    std::string help = setting.hint;
    const auto append = [&](std::string_view description) {
        if (!help.empty()) {
            help += '\n';
        }
        help += description;
    };
    if (setting.kind == text_or_list) {
        append("Enter one quoted JSON string or an array of quoted strings.");
    } else if (setting.kind == port_range) {
        append("The range includes both endpoints.");
    }
    const bool numeric =
        setting.kind == integer || setting.kind == number || setting.kind == integer_or_boolean;
    const bool bounded_number = numeric && (setting.minimum != std::numeric_limits<std::int32_t>::min() ||
                                            setting.maximum != std::numeric_limits<std::int32_t>::max());
    if ((bounded_number && setting.maximum <= 9007199254740991.) || setting.kind == port ||
        setting.kind == port_list || setting.kind == port_range) {
        append(std::format("Numeric values range from {} through {}.", setting.minimum, setting.maximum));
    }
    return help.empty() ? std::string{} : std::format("{}\n{}", key, help);
}

bool edit_setting_choice(Json& settings, const spec::Field& setting, const char* key)
{
    using enum spec::Kind;
    constexpr const char* boolean_options[] = {"Enabled", "Disabled"};
    const auto options = setting.kind == boolean ? std::span{boolean_options} : setting.choices;
    const auto stored = settings.find(key);
    const bool omitted = stored == settings.end();
    const auto option_value = [&](std::size_t index) -> Json {
        return setting.kind == boolean ? Json(index == 0) : Json(options[index]);
    };
    const auto is_selected = [&](const Json& value, std::size_t index) {
        if (setting.kind != choice || !setting.case_insensitive || !value.is_string()) {
            return value == option_value(index);
        }
        auto option = setting;
        option.choices = options.subspan(index, 1);
        return spec::valid_value(value, option);
    };
    const char* preview = setting.required ? "Choose a value" : "WaterWall default";
    if (!omitted) {
        preview = stored->is_null() && setting.nullable ? "Null" : "Invalid value";
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (is_selected(*stored, i)) {
                preview = options[i];
                break;
            }
        }
    }
    if (!ImGui::BeginCombo("##value", preview)) {
        return false;
    }
    bool changed = false;
    if (ImGui::Selectable(setting.required ? "Unset" : "WaterWall default", omitted) && !omitted) {
        settings.erase(key);
        changed = true;
    }
    if (setting.nullable &&
        ImGui::Selectable("Null", field(settings, key).is_null() && settings.contains(key))) {
        settings[key] = nullptr;
        changed = true;
    }
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (ImGui::Selectable(options[i], is_selected(field(settings, key), i))) {
            settings[key] = option_value(i);
            changed = true;
        }
    }
    ImGui::EndCombo();
    return changed;
}

Json parse_setting_input(const spec::Field& setting, const std::string& input, std::string& error)
{
    using enum spec::Kind;
    if (input.size() > max_core_config_bytes) {
        error = "Wave can save configs up to 256 KiB.";
        return {};
    }
    const bool special_port =
        setting.kind == port && (std::ranges::find(setting.choices, input) != setting.choices.end() ||
                                 (setting.allow_random_port && input.starts_with("random(")));
    auto value =
        special_port ? Json(input) : parse_plain_core_config_json(input, "Enter a valid value.", error);
    if (!error.empty() || spec::valid_value(value, setting)) {
        return value;
    }
    if (*setting.hint) {
        error = setting.hint;
    } else if (setting.kind == integer || setting.kind == number || setting.kind == port ||
               setting.kind == integer_or_boolean) {
        error = std::format("Enter a {} from {} to {}.", setting.kind == number ? "number" : "whole number",
                            setting.minimum, setting.maximum);
    } else {
        error = "Enter a supported value.";
    }
    return {};
}

bool edit_setting_input(State& state, Json& settings, const spec::Field& setting, const char* key,
                        const std::string& path)
{
    using enum spec::Kind;
    const auto& value = field(settings, key);
    const auto invalid = state.invalid_setting_inputs.find(path);
    const bool plain_text = setting.kind == text;
    const bool multiline = setting.kind == array || setting.kind == object || setting.kind == json ||
                           setting.kind == text_or_list;
    std::string input;
    if (invalid != state.invalid_setting_inputs.end()) {
        input = invalid->second.text;
    } else if (!value.is_null()) {
        input = !multiline && value.is_string() ? value.get<std::string>() : value.dump(multiline ? 2 : -1);
    }
    const auto hint = plain_text               ? (value == "" ? "Empty value" : "Enter text")
                      : setting.kind == number ? "Enter a number"
                                               : "Enter a whole number";
    const char* input_hint = hint;
    if (setting.kind == port_list) {
        input_hint = "Enter a port or [port, port]";
    } else if (setting.kind == port_range) {
        input_hint = "Enter [first port, last port]";
    } else if (setting.kind == port && !setting.choices.empty()) {
        input_hint = setting.allow_random_port ? "Enter a port, random(min,max), or select a mode"
                                               : "Enter a port or select a mode";
    }
    const bool typed = multiline ? ImGui::InputTextMultiline("##value", &input, {-1, 76})
                                 : ImGui::InputTextWithHint("##value", input_hint, &input);
    bool changed = false;
    if (typed) {
        state.invalid_setting_inputs.erase(path);
        if (input.empty()) {
            changed = settings.erase(key) != 0;
        } else if (plain_text) {
            settings[key] = input;
            changed = true;
        } else {
            std::string error;
            auto updated = parse_setting_input(setting, input, error);
            if (error.empty()) {
                settings[key] = std::move(updated);
                changed = true;
            } else {
                state.invalid_setting_inputs[path] = {std::move(input), std::move(error)};
            }
        }
    }
    const auto assign = [&](Json selected) {
        state.invalid_setting_inputs.erase(path);
        settings[key] = std::move(selected);
        changed = true;
    };
    if (setting.kind == port && !setting.choices.empty()) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##port-mode", "Special values")) {
            for (const auto* option : setting.choices) {
                if (ImGui::Selectable(option, field(settings, key) == option)) {
                    assign(option);
                }
            }
            ImGui::EndCombo();
        }
    } else if (setting.kind == integer_or_boolean) {
        if (ImGui::SmallButton("Enable")) {
            assign(true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Disable")) {
            assign(false);
        }
    }
    if (const auto error = state.invalid_setting_inputs.find(path);
        error != state.invalid_setting_inputs.end()) {
        wrapped(error->second.error, Color::error);
    } else if ((field(settings, key) == "" ||
                (multiline && settings.contains(key) && field(settings, key).is_null())) &&
               ImGui::SmallButton("Clear value")) {
        settings.erase(key);
        changed = true;
    }
    return changed;
}

bool edit_setting_fields(State& state, Json& settings, bool adding, std::span<const spec::Field> fields,
                         const std::string& path = {});

bool edit_setting_group(State& state, Json& settings, bool adding, const spec::Field& setting,
                        const char* key, const std::string& path)
{
    using enum spec::Kind;
    const auto help = setting_help(setting, key);
    const auto flags = help.empty() ? ImGuiTreeNodeFlags_SpanAvailWidth
                                    : ImGuiTreeNodeFlags_SpanLabelWidth | ImGuiTreeNodeFlags_AllowOverlap;
    const bool expanded = ImGui::TreeNodeEx("##group", flags, "%s", setting.label);
    if (help.empty()) {
        ImGui::SetItemTooltip("%s", key);
    } else {
        help_marker(help);
    }
    if (!expanded) {
        return false;
    }
    const bool is_object = setting.kind == spec::Kind::object;
    const auto item = settings.find(key);
    bool changed = false;
    auto value = item == settings.end() ? (is_object ? Json::object() : Json::array()) : *item;
    const bool valid = is_object ? value.is_object() : value.is_array();
    if (!valid) {
        wrapped("Use Custom JSON to repair this value, or clear it to start over.", Color::warning);
    } else if (is_object) {
        changed = edit_setting_fields(state, value, adding, setting.children, path);
        if (item == settings.end() && ImGui::SmallButton("Add empty group")) {
            changed = true;
        }
    } else {
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < value.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            auto& row = value[i];
            if (setting.kind == string_list) {
                auto input = row.is_string() ? row.get<std::string>() : row.dump();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##entry", &input)) {
                    row = std::move(input);
                    changed = true;
                }
            } else {
                ImGui::SeparatorText(std::format("Entry {}", i + 1).c_str());
                if (row.is_object()) {
                    changed |= edit_setting_fields(state, row, adding, setting.children,
                                                   std::format("{}/{}", path, i));
                } else {
                    wrapped("This entry needs a JSON object.", Color::warning);
                }
            }
            if (ImGui::SmallButton("Remove entry")) {
                remove = i;
            }
            ImGui::PopID();
        }
        if (remove) {
            value.erase(value.begin() + static_cast<Json::difference_type>(*remove));
            const auto prefix = path + '/';
            std::erase_if(state.invalid_setting_inputs,
                          [&](const auto& entry) { return entry.first.starts_with(prefix); });
            changed = true;
        }
        if (ImGui::SmallButton("Add entry")) {
            value.push_back(setting.kind == string_list ? Json("") : Json::object());
            changed = true;
        }
        if (setting.kind == string_list && item == settings.end()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Use empty list")) {
                changed = true;
            }
        }
    }
    if (item != settings.end() && ImGui::SmallButton("Clear value")) {
        const auto prefix = path + '/';
        std::erase_if(state.invalid_setting_inputs,
                      [&](const auto& entry) { return entry.first.starts_with(prefix); });
        settings.erase(key);
        changed = true;
    } else if (changed) {
        if (setting.kind == object_list && value.empty()) {
            settings.erase(key);
        } else {
            settings[key] = std::move(value);
        }
    }
    ImGui::TreePop();
    return changed;
}

bool edit_setting_fields(State& state, Json& settings, bool adding, std::span<const spec::Field> fields,
                         const std::string& path)
{
    using enum spec::Kind;
    bool changed = false;
    const auto links = node_link_fields(state.node_type);
    const auto matches = [&](const spec::Field& field, const NodeLinkField& link) {
        return field.kind == node_target && std::string_view(field.key) == link.key &&
               is_link_container(path, link);
    };
    for (const auto& setting : fields) {
        if (setting.kind == node_target) {
            const auto link =
                std::ranges::find_if(links, [&](const auto& link) { return matches(setting, link); });
            if (link != links.end()) {
                changed |= edit_setting_link(state, settings, adding, *link, setting.hint);
            } else {
                const NodeLinkField reference{setting.key, setting.label, NodeLinkKind::reference, nullptr,
                                              nullptr,     nullptr,       setting.aliases};
                changed |= edit_setting_link(state, settings, adding, reference, setting.hint);
            }
            continue;
        }
        ImGui::PushID(setting.key);
        const auto* key = spec::field_key(settings, setting);
        if (path.empty() && database_key(state.node_type) == setting.key) {
            wrapped("Wave assigns the user database path when you save.", Color::secondary_text);
        } else if ((setting.kind == object && !setting.children.empty()) || setting.kind == object_list ||
                   setting.kind == string_list) {
            changed |= edit_setting_group(state, settings, adding, setting, key, path + '/' + setting.key);
        } else {
            ImGui::TextUnformatted(setting.label);
            const auto help = setting_help(setting, key);
            if (help.empty()) {
                ImGui::SetItemTooltip("%s", key);
            } else {
                help_marker(help);
            }
            ImGui::SetNextItemWidth(-1);
            if (setting.kind == boolean || setting.kind == choice) {
                changed |= edit_setting_choice(settings, setting, key);
            } else {
                changed |= edit_setting_input(state, settings, setting, key, path + '/' + setting.key);
            }
        }
        ImGui::PopID();
    }
    for (const auto& link : links) {
        if (is_link_container(path, link) &&
            std::ranges::none_of(fields, [&](const auto& field) { return matches(field, link); })) {
            changed |= edit_setting_link(state, settings, adding, link);
        }
    }
    return changed;
}

void edit_node(State& state, bool adding)
{
    Dialog popup(title(state.dialog), 560, {{"Cancel"}, {adding ? "Add node" : "Apply changes"}});
    if (!popup) {
        return;
    }
    ImGui::TextUnformatted("Name");
    help_marker("Names must be unique in this configuration. Renaming updates the links and named references "
                "Wave recognizes.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##node-name", &state.node_name);
    ImGui::TextUnformatted("Type");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##node-type", state.node_type.empty() ? "Choose a WaterWall node type"
                                                                 : state.node_type.c_str())) {
        for (const auto& type : waterwall_node_types()) {
            if (ImGui::Selectable(type.data(), state.node_type == type) && state.node_type != type) {
                state.node_type = type;
                state.invalid_setting_inputs.clear();
                state.error.clear();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Settings JSON");
    ImGui::SameLine();
    if (ImGui::Checkbox("Custom JSON", &state.custom_json)) {
        state.error.clear();
        if (!state.custom_json) {
            parse_node_settings(state.settings_text, state.error);
            state.custom_json = !state.error.empty();
        }
        state.invalid_setting_inputs.clear();
    }
    help_marker("Editing Settings JSON enables Custom JSON and pauses the settings controls, including named "
                "targets. Clear the checkbox to use those controls once the JSON is valid. The node name and "
                "top-level Next remain editable. Wave checks the JSON and graph links before saving.");
    if (ImGui::InputTextMultiline("##node-settings", &state.settings_text, {-1, 134})) {
        state.custom_json = true;
        state.invalid_setting_inputs.clear();
        state.error.clear();
    }
    std::string settings_error;
    auto settings = parse_node_settings(state.settings_text, settings_error);
    if (!settings_error.empty()) {
        settings = Json::object();
    }
    if (!state.node_type.empty()) {
        const auto& node = waterwall_node(state.node_type);
        if (allows_node_next(state.node_type, settings)) {
            const auto& next = field(node, "next_field");
            const auto label = string_field(next, "label", "Next node");
            select_target(
                state, label.data(), state.node_next, adding,
                string_field(next, "hint",
                             "Choose the next node in this chain. The graph shows this connection in blue."));
        }
        ImGui::SeparatorText(std::format("{} settings", state.node_type).c_str());
        const auto fields = spec::fields(state.node_type);
        if (fields.empty() && node_link_fields(state.node_type).empty()) {
            if (node.is_object()) {
                wrapped(std::format("WaterWall {} exposes no settings for this node.", waterwall_version()),
                        Color::secondary_text);
            } else {
                wrapped("Use Custom JSON to edit this imported node’s settings.", Color::secondary_text);
            }
        } else {
            ImGui::BeginDisabled(state.custom_json || !settings_error.empty());
            if (edit_setting_fields(state, settings, adding, fields) && !state.custom_json &&
                settings_error.empty()) {
                state.settings_text = settings.dump(2);
                state.error.clear();
            }
            ImGui::EndDisabled();
        }
    }
    if (!state.error.empty()) {
        wrapped(state.error, Color::error);
    }
    const auto action = popup.footer();
    if (action == 0 || (action == 1 && apply_node(state, adding))) {
        close_dialog(state);
    }
}

void draw_config_json_editor(const std::string& initial_text, bool read_only)
{
    const auto visible = visible_canvas();
    const auto origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(1.0f, ImGui::GetContentRegionAvail().x),
                      std::clamp(visible.height - 200.0f, 120.0f, 420.0f)};
    ImGui::Dummy(size);
    const auto& window_clip = ImGui::GetCurrentWindowRead()->ClipRect;
    const float left = std::max(visible.left, window_clip.Min.x);
    const float top = std::max(visible.top, window_clip.Min.y);
    const float right = std::min(visible.left + visible.width, window_clip.Max.x);
    const float bottom = std::min(visible.top + visible.height, window_clip.Max.y);
    show_config_json_editor(initial_text, read_only, {origin.x, origin.y, size.x, size.y},
                            {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)});
}

void render_dialog(State& state, PreviewSession& session, bool reload_finished)
{
    if (state.dialog == State::Dialog::none) {
        return;
    }
    if (!ImGui::IsPopupOpen(title(state.dialog))) {
        if (state.dialog == State::Dialog::confirm_import) {
            state.import_config = {};
        }
        state.dialog = State::Dialog::none;
        if (!reload_finished) {
            state.error.clear();
        }
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
            draw_config_json_editor(state.json_text, true);
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
            Dialog popup(edit_json_title, 740,
                         {{"Cancel"}, {"Apply nodes", !state.graph_editable || !config_json_editor_ready()}});
            if (popup) {
                wrapped("Use Text mode to paste a config.json or nodes array. Applying replaces the draft "
                        "nodes.");
                draw_config_json_editor(state.json_text, !state.graph_editable);
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
        Dialog popup(import_title, 500, {{"Cancel", pending}, {"Resolve and save", !state.graph_editable}},
                     {.busy = pending});
        if (popup) {
            if (state.reply && state.reply->ok()) {
                close_dialog(state);
                return;
            }
            wrapped(import_notice());
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
        const bool pending = state.request.has_value();
        Dialog popup(reload_title, 460, {{"Cancel", pending}, {"Reload", pending || !session.ready()}},
                     {.busy = pending});
        if (popup) {
            if (reload_finished) {
                close_dialog(state, true);
                return;
            }
            wrapped("Reload the saved configuration and discard your unsaved changes?");
            const auto action = popup.footer();
            if (action == 0) {
                close_dialog(state);
            } else if (action == 1) {
                submit(state, session, State::Request::load);
                if (!state.request) {
                    close_dialog(state, true);
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

constexpr char unsaved_status[] = "Unsaved changes";

GraphStatus graph_status(const State& state, bool active)
{
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
    return {state.dirty ? unsaved_status : ""};
}

std::optional<ImVec2> menu_status_position(std::string_view message)
{
    if (message.empty() || message.find('\n') != std::string_view::npos) {
        return {};
    }
    const auto start = message.data();
    const auto end = start + message.size();
    const auto cursor = ImGui::GetCursorScreenPos();
    const auto& style = ImGui::GetStyle();
    const float x = ImGui::GetWindowDrawList()->GetClipRectMax().x - style.FramePadding.x -
                    ImGui::CalcTextSize(start, end).x;
    if (x < cursor.x + style.ItemSpacing.x) {
        return {};
    }
    return ImVec2{x, cursor.y + style.FramePadding.y};
}

void save_menu_item(State& state, PreviewSession& session)
{
    if (ImGui::MenuItem("Save", nullptr, false, state.graph_editable && state.dirty)) {
        submit(state, session, State::Request::save);
    }
}

State::Dialog node_edit_menu_items(State& state, std::optional<std::size_t> target)
{
    const auto& list = nodes(state);
    const bool editable =
        state.graph_editable && target && *target < list.size() && list[*target].is_object();
    if (ImGui::MenuItem("Edit node", nullptr, false, editable)) {
        state.selected = target;
        return State::Dialog::edit;
    }
    if (ImGui::MenuItem("Delete node", nullptr, false, editable)) {
        state.selected = target;
        return State::Dialog::remove;
    }
    return State::Dialog::none;
}

State::Dialog graph_context_menu(State& state, PreviewSession& session, float width, float height)
{
    constexpr char id[] = "##graph-context-menu";
    if (state.dialog == State::Dialog::none && !ImGui::IsItemActive() && ImGui::OpenPopupOnItemClick(id)) {
        const auto mouse = ImGui::GetIO().MousePos;
        state.context_node = node_at(state, mouse.x, mouse.y);
    }
    auto dialog = State::Dialog::none;
    if (ImGui::BeginPopup(id)) {
        if (ImGui::MenuItem("Add node", nullptr, false, state.graph_editable)) {
            dialog = State::Dialog::add;
        }
        save_menu_item(state, session);
        if (ImGui::MenuItem("Fit graph", nullptr, false, !nodes(state).empty())) {
            fit_graph(state, width, height);
        }
        if (state.context_node) {
            ImGui::Separator();
            const auto edit_dialog = node_edit_menu_items(state, state.context_node);
            if (edit_dialog != State::Dialog::none) {
                dialog = edit_dialog;
            }
        }
        ImGui::EndPopup();
    }
    return dialog;
}

void render_graph_pane(State& state, PreviewSession& session, bool active, bool narrow)
{
    auto& pane = *ImGui::GetCurrentWindow();
    const auto previous_content_max = pane.DC.CursorMaxPos;
    const auto spacing = ImGui::GetStyle().ItemSpacing.y;
    const auto border = pane.WindowBorderSize;
    const ImVec2 origin{pane.InnerRect.Min.x + border, ImGui::GetCursorScreenPos().y - spacing};
    const float width = std::max(1.f, pane.InnerRect.Max.x - border - origin.x);
    const float height = std::max((narrow ? 230.f : 280.f) + ImGui::GetFrameHeight(),
                                  pane.InnerRect.Max.y - border - pane.Scroll.y - origin.y);

    ImGui::SetCursorScreenPos(origin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgba(Color::divider));
    constexpr auto flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    const bool visible =
        ImGui::BeginChild("Graph pane", {width, height}, ImGuiChildFlags_NavFlattened, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    auto dialog = State::Dialog::none;
    if (visible) {
        bool fit = false;
        bool unsaved_status_below_menu = false;
        GraphStatus status;
        if (ImGui::BeginMenuBar()) {
            save_menu_item(state, session);
            if (ImGui::BeginMenu("Nodes")) {
                const bool add = ImGui::MenuItem("Add node", nullptr, false, state.graph_editable);
                const auto edit_dialog = node_edit_menu_items(state, state.selected);
                dialog = add ? State::Dialog::add : edit_dialog;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Config")) {
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
            if (state.request && state.dialog != State::Dialog::confirm_import &&
                state.dialog != State::Dialog::reload) {
                draw_busy_spinner(ImGui::GetFrameHeight());
            }
            status = graph_status(state, active);
            if (const auto position = menu_status_position(status.message)) {
                ImGui::GetWindowDrawList()->AddText(*position, packed(status.color), status.message.data(),
                                                    status.message.data() + status.message.size());
                status.message = {};
            }
            unsaved_status_below_menu =
                state.dialog == State::Dialog::add && !menu_status_position(unsaved_status);
            ImGui::EndMenuBar();
        }
        float add_canvas_top = ImGui::GetCursorScreenPos().y;
        const float status_inset = pane.WindowPadding.x - border;
        if (unsaved_status_below_menu) {
            // The draft status can take a row above the graph once Add is confirmed.
            const float wrap_width = std::max(1.f, ImGui::GetContentRegionAvail().x - status_inset);
            const float status_height = ImGui::CalcTextSize(unsaved_status, nullptr, false, wrap_width).y;
            add_canvas_top = std::trunc(add_canvas_top + status_height + ImGui::GetStyle().ItemSpacing.y);
        }
        if (!status.message.empty()) {
            ImGui::Indent(status_inset);
            wrapped(status.message, status.color);
            ImGui::Unindent(status_inset);
        }
        const auto available = ImGui::GetContentRegionAvail();
        const float canvas_height = std::max(1.f, available.y);
        if (fit) {
            fit_graph(state, available.x, canvas_height);
        }
        if (draw_graph(state, canvas_height, pane)) {
            dialog = State::Dialog::edit;
        }
        if (state.dialog == State::Dialog::add) {
            state.add_position = centered_node_position(state, add_canvas_top);
        }
        const auto context_dialog = graph_context_menu(state, session, available.x, canvas_height);
        if (context_dialog != State::Dialog::none) {
            dialog = context_dialog;
        }
    }
    ImGui::EndChild();
    // Drawing into the workspace padding should not add to its scrollable area.
    const float content_bottom = origin.y + height - std::max(0.f, pane.WindowPadding.y - border);
    pane.DC.CursorMaxPos.x = std::max(previous_content_max.x, pane.InnerRect.Max.x - pane.WindowPadding.x);
    pane.DC.CursorMaxPos.y = std::max(previous_content_max.y, content_bottom);
    pane.DC.CursorPos.y = content_bottom + spacing;

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
    state.schematic_unavailable = session.ready() && waterwall_version().empty();
    if (state.schematic_unavailable) {
        state.graph_editable = false;
        wrapped(schematic_unavailable, Color::warning);
        return;
    }
    if (!state.load_requested && session.ready()) {
        submit(state, session, State::Request::load);
    }
    const bool was_reloading = state.dialog == State::Dialog::reload && state.request.has_value();
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
            return;
        }
        if (session.ready()) {
            wrapped("The configuration could not be loaded.", Color::warning);
            if (theme::button("Retry")) {
                submit(state, session, State::Request::load);
            }
        }
        if (state.reply && !state.reply->message.empty()) {
            wrapped(state.reply->message, Color::error);
        }
        if (!state.error.empty()) {
            wrapped(state.error, Color::error);
        }
        return;
    }

    const bool active = config_active(session, config_id);
    state.graph_editable = !active && !state.request && session.ready();
    render_graph_pane(state, session, active, narrow);
    render_dialog(state, session, was_reloading && !state.request);
}

std::string describe_config_nodes(const ConfigNodesState& state)
{
    if (state.schematic_unavailable) {
        return std::string("Nodes. ") + schematic_unavailable;
    }
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
            description += ' ';
            description += import_notice();
        }
        if ((state.dialog == State::Dialog::add || state.dialog == State::Dialog::edit) &&
            state.custom_json) {
            description += " Custom JSON enabled.";
        }
    }
    if (state.dirty) {
        description += " Unsaved changes.";
    }
    if (state.request) {
        description += ' ';
        description += busy_description;
    } else if (state.reply && !state.reply->message.empty()) {
        description += ' ' + state.reply->message;
    }
    if (!state.error.empty()) {
        description += ' ' + state.error;
    }
    return description;
}

} // namespace wave
