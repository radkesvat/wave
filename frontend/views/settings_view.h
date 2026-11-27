#pragma once

#include "core_settings_view.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wave {

class AuthSession;

struct SessionSettingsDraft {
    std::optional<int> saved_days;
    std::string expiry_days;
    std::optional<std::uint64_t> request;
};

struct TlsSettingsDraft {
    nlohmann::json source;
    std::string domain;
    std::string certificate_chain;
    std::string private_key;
    bool enabled = false;
    bool dirty = false;
    bool restart_required = false;
    bool confirming = false;
    std::optional<std::uint64_t> request;
};

struct SettingsViewState {
    enum class Tab {
        panel,
        core
    };

    Tab tab = Tab::panel;
    CanvasViewport scroll_bounds{};
    CoreSettingsDraft core;
    SessionSettingsDraft sessions;
    TlsSettingsDraft tls;
    std::string administrator;
    std::array<char, 65> admin_username{};
    std::array<char, 257> current_password{};
    std::array<char, 257> new_password{};
};

void render_settings(SettingsViewState& state, AuthSession& auth, PreviewSession& session, bool narrow);
std::string describe_settings(const SettingsViewState& state, const AuthSession& auth);

} // namespace wave
