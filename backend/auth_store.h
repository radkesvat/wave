#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

class PanelDatabase;

class AuthStore {
  public:
    struct Session {
        std::string username;
        std::int64_t expires_at;
        std::string id;
    };

    struct IssuedSession {
        std::string token;
        int lifetime_seconds = 0;
    };

    enum class SetupResult {
        created,
        closed,
        invalid_password,
        weak_password
    };
    enum class SignInResult {
        accepted,
        invalid,
        busy
    };
    enum class UpdateResult {
        updated,
        invalid,
        invalid_password,
        weak_password,
        unauthorized,
        busy
    };
    enum class BasePathResult {
        saved,
        stale,
        unauthorized
    };

    explicit AuthStore(std::shared_ptr<PanelDatabase> database);
    ~AuthStore();

    bool installed() const;
    static bool valid_username(std::string_view username);
    static bool valid_password_input(std::string_view password);
    SetupResult setup(std::string_view username, std::string_view password);
    SignInResult sign_in(std::string_view username, std::string_view password, IssuedSession& session);
    std::optional<Session> session(std::string_view token) const;
    std::optional<std::string> monitored_panel_path(std::string_view token, std::string_view session_id,
                                                   std::string_view connected_path) const;
    void sign_out(std::string_view token);
    UpdateResult update_admin(std::string_view token, std::string_view username,
                              std::string_view current_password, std::string_view new_password);
    BasePathResult update_panel_base_path(std::string_view token, std::string_view previous,
                                         std::string_view path);
    static void validate_admin_backup(const nlohmann::json& admins);
    bool restore_admin(const nlohmann::json& admins, std::string_view session_id, std::string_view restored_base_path,
                       const std::function<void(const nlohmann::json& local_auth_state)>& commit);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
