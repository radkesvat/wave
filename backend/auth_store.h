#pragma once

#include <cstdint>
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
    };

    enum class SetupResult {
        created,
        closed
    };
    enum class SignInResult {
        accepted,
        invalid,
        busy
    };
    enum class UpdateResult {
        updated,
        invalid,
        unauthorized,
        busy
    };

    static constexpr int session_seconds = 24 * 60 * 60;

    explicit AuthStore(std::shared_ptr<PanelDatabase> database);
    ~AuthStore();

    bool installed() const;
    static bool valid_username(std::string_view username);
    static bool valid_password(std::string_view password);
    SetupResult setup(std::string_view username, std::string_view password);
    SignInResult sign_in(std::string_view username, std::string_view password, std::string& token);
    std::optional<Session> session(std::string_view token) const;
    void sign_out(std::string_view token);
    UpdateResult update_admin(std::string_view token, std::string_view username,
                              std::string_view current_password, std::string_view new_password);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
