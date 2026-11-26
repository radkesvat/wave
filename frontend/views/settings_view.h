#pragma once

#include "preview_session.h"

#include <array>
#include <cstdint>
#include <optional>

namespace wave {

class AuthSession;

struct SettingsViewState {
    nlohmann::json source;
    std::array<char, 161> domain{};
    std::array<char, 161> certificate_path{};
    std::array<char, 161> private_key_path{};
    bool dirty = false;
    std::optional<std::uint64_t> apply_request;
    std::optional<PreviewReply> apply_reply;
    std::string administrator;
    std::array<char, 65> admin_username{};
    std::array<char, 257> current_password{};
    std::array<char, 257> new_password{};
};

void render_settings(SettingsViewState& state, PreviewSession& session, bool narrow);
void render_settings_dialogs(SettingsViewState& state, PreviewSession& session);
void render_administrator_settings(SettingsViewState& state, AuthSession& auth);

} // namespace wave
