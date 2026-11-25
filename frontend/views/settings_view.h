#pragma once

#include "preview_session.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wave {

struct SettingsViewState {
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
};

void render_settings(SettingsViewState& state, PreviewSession& session, bool narrow);

} // namespace wave
