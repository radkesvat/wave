#pragma once

#include "preview_session.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace wave {

class PreviewSession;

enum class SecondaryView { server, settings, console, bot = 4, about };

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
        std::optional<std::uint64_t> apply_request;
        std::optional<PreviewReply> apply_reply;
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

    struct Bot {
        struct Notification {
            bool enabled = true;
            bool sound = false;
        };
        struct Save {
            std::optional<std::uint64_t> request;
            std::optional<PreviewReply> reply;
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
        std::optional<std::uint64_t> connect_request;
        std::optional<PreviewReply> connect_reply;
        Save settings_save;
        Save overrides_save;
        Setup setup = Setup::intro;
        bool notifications = false;
        bool allow_status = false;
        bool allow_config = false;
        bool settings_dirty = false;
        bool overrides_dirty = false;
        bool access_link_copied = false;
    } bot;
};

void render_secondary(SecondaryViewsState& state, SecondaryView view, PreviewSession& session,
                      bool narrow);

} // namespace wave
