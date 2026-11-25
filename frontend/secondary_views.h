#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace wave {

class PreviewSession;

enum class SecondaryView { server, settings, console, topology, bot, about };

struct SecondaryViewsState {
    struct Settings {
        nlohmann::json source;
        std::array<char, 81> name{};
        std::array<char, 65> username{};
        std::array<char, 97> password{};
        std::array<char, 161> domain{};
        std::array<char, 161> websocket_path{};
        std::array<char, 161> certificate_path{};
        std::array<char, 161> private_key_path{};
        std::array<char, 81> site_template{};
        int port = 2030;
        bool multiplex = false;
        bool dirty = false;
        bool applying = false;
        bool apply_timed_out = false;
        double apply_deadline = 0;
        std::uint64_t apply_feedback = 0;
        std::string backup;
    } settings;

    struct Console {
        std::array<char, 241> command{};
        std::array<char, 97> search{};
        int history_position = -1;
        std::size_t output_size = 0;
        std::string last_line;
        bool autoscroll = true;
        bool copied = false;
    } console;

    struct Topology {
        enum class Selection { node, link, route };
        Selection selection = Selection::node;
        std::string selected;
        float zoom = 1.0f;
        float offset_x = 0;
        float offset_y = 0;
        bool graph_dragged = false;
    } inside_map, outside_map;

    struct Bot {
        struct Notification {
            bool enabled = true;
            bool sound = false;
        };
        enum class Setup { intro, token, connecting, complete };

        nlohmann::json source;
        std::array<Notification, 14> rules{};
        std::array<bool, 7> permissions{};
        std::array<char, 161> domain{};
        std::array<char, 161> sni{};
        std::array<char, 161> ws_host{};
        std::array<char, 81> token{};
        std::array<char, 481> message{};
        int port = 0;
        int audience = 0;
        double connect_deadline = 0;
        std::uint64_t connect_feedback = 0;
        Setup setup = Setup::intro;
        bool notifications = false;
        bool allow_status = false;
        bool allow_config = false;
        bool settings_dirty = false;
        bool overrides_dirty = false;
        bool access_link_copied = false;
        bool connect_timed_out = false;
    } bot;
};

void render_secondary(SecondaryViewsState& state, SecondaryView view, PreviewSession& session,
                      bool narrow);

} // namespace wave
