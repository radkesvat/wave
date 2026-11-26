#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace wave {

struct TopologyViewState {
    enum class Tab {
        none,
        inside,
        outside
    };

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
        float width = 0;
        float height = 0;
        bool graph_dragged = false;

        void pan(float dx, float dy);
    };

    struct Bounds {
        float left = 0;
        float top = 0;
        float right = 0;
        float bottom = 0;
    };

    Map inside_map;
    Map outside_map;
    Tab visible_map = Tab::none;
    Bounds visible_bounds;

    Tab map_at(float x, float y) const;
    void pan(Tab map, float dx, float dy);
};

void render_topology(TopologyViewState& state, const nlohmann::json& topology, bool narrow);

} // namespace wave
