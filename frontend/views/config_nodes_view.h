#pragma once

#include "preview_session.h"

#include <imgui.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wave {

struct ConfigNodesState {
    struct SettingInput {
        std::string text;
        std::string error;
    };

    struct Bounds {
        float left = 0;
        float top = 0;
        float right = 0;
        float bottom = 0;

        bool contains(float x, float y) const;
    };

    enum class Request {
        load,
        save,
        save_import
    };
    enum class Dialog {
        none,
        add,
        edit,
        remove,
        json,
        edit_json,
        confirm_import,
        reload
    };

    int config_id = 0;
    nlohmann::json saved;
    nlohmann::json draft;
    nlohmann::json revision;
    std::optional<std::uint64_t> request;
    Request operation = Request::load;
    bool load_requested = false;
    bool dirty = false;
    bool graph_editable = false;
    std::optional<PreviewReply> reply;

    Dialog dialog = Dialog::none;
    std::string node_name;
    std::string node_type;
    std::string node_next;
    bool custom_json = false;
    std::string settings_text;
    std::map<std::string, SettingInput, std::less<>> invalid_setting_inputs;
    std::string json_text;
    nlohmann::json import_config;
    std::string error;

    std::optional<std::size_t> selected;
    std::optional<std::size_t> link_from;
    std::optional<std::size_t> grabbed;
    std::optional<std::size_t> last_clicked_node;
    std::optional<std::size_t> touch_node;
    bool dragged = false;
    bool pressed_output = false;
    Bounds viewport;
    ImVec2 canvas_origin;
    std::vector<Bounds> node_bounds;
    float pan_x = 0;
    float pan_y = 0;
    float zoom = 1.0f;

    bool zoom_at(float x, float y, float steps);
    bool begin_touch(float x, float y);
    void drag_touch(float dx, float dy);
};

void render_config_nodes(ConfigNodesState& state, PreviewSession& session, int config_id, bool narrow);
std::string describe_config_nodes(const ConfigNodesState& state);

} // namespace wave
