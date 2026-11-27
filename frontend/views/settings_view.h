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

struct SettingsViewState {
    enum class Tab {
        panel,
        core
    };

    Tab tab = Tab::panel;
    CoreSettingsDraft core;
    SessionSettingsDraft sessions;
    nlohmann::json source;
    std::array<char, 161> domain{};
    std::array<char, 161> certificate_path{};
    std::array<char, 161> private_key_path{};
    bool dirty = false;
    std::optional<std::uint64_t> apply_request;
    std::string administrator;
    std::array<char, 65> admin_username{};
    std::array<char, 257> current_password{};
    std::array<char, 257> new_password{};
};

void render_settings(SettingsViewState& state, AuthSession& auth, PreviewSession& session, bool narrow);
std::string describe_settings(const SettingsViewState& state, const AuthSession& auth);

} // namespace wave
