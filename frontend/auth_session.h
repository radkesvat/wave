#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace wave {

inline constexpr char new_password_requirements[] =
    "Use at least 6 characters, including a letter A–Z or a–z and a digit 0–9. Symbols are optional.";
inline constexpr char base_path_format_error[] =
    "Enter a path such as /panel with a leading slash and no trailing slash.";

class AuthSession {
  public:
    enum class Page {
        checking,
        setup,
        sign_in,
        panel,
        base_path
    };
    enum class Action {
        setup,
        sign_in,
        sign_out,
        update_administrator,
        base_path
    };

    void start();
    void submit(Action action, const nlohmann::json& input);
    void request_failed(int status);
    void set_identity(const char* username, const char* session_id);

    Page page() const
    {
        return page_;
    }
    bool pending() const
    {
        return pending_;
    }
    const std::string& error() const
    {
        return error_;
    }
    Action action() const
    {
        return action_;
    }
    void clear_error()
    {
        error_.clear();
    }
    const std::string& username() const
    {
        return username_;
    }
    const std::string& session_id() const
    {
        return session_id_;
    }
    const std::string& base_path() const
    {
        return base_path_;
    }

  private:
    Page page_ = Page::checking;
    Action action_ = Action::sign_in;
    bool pending_ = false;
    std::string error_;
    std::string username_;
    std::string session_id_;
    std::string base_path_;
};

} // namespace wave
