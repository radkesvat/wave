#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace wave {

struct TopologyViewState {
    struct Map {
        enum class Selection {
            node,
            link,
            route
        };
        Selection selection = Selection::node;
        std::string selected;
        float zoom = 1.0f;
        float offset_x = 0;
        float offset_y = 0;
        bool graph_dragged = false;
    };
    Map inside_map;
    Map outside_map;
};

void render_topology(TopologyViewState& state, const nlohmann::json& topology, bool narrow);

} // namespace wave
