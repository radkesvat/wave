#pragma once

#include "preview_session.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wave {

struct TopologyViewState {
    enum class Selection {
        node,
        link,
        route
    };

    struct Bounds {
        float left = 0;
        float top = 0;
        float right = 0;
        float bottom = 0;
    };

    Bounds visible_bounds;
    Selection selection = Selection::node;
    std::string selected;
    float zoom = 1.0f;
    float offset_x = 0;
    float offset_y = 0;
    bool graph_dragged = false;
    std::array<char, 129> node_name{};
    std::string delete_id;
    std::optional<std::uint64_t> request_id;
    std::optional<PreviewReply> reply;

    bool canvas_at(float x, float y) const;
    void pan(float dx, float dy);
};

void render_topology(TopologyViewState& state, PreviewSession& session, bool narrow);
void render_topology_dialogs(TopologyViewState& state, PreviewSession& session);
std::string describe_topology(const TopologyViewState& state, const nlohmann::json& topology);

} // namespace wave
