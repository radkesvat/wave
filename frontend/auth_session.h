#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace wave {

class AuthSession {
  public:
    enum class Page {
        checking,
        setup,
        sign_in,
        panel
    };
    enum class Action {
        setup,
        sign_in,
        sign_out,
        update_administrator
    };

    void start();
    void submit(Action action, const nlohmann::json& input);
    void request_failed(int status);
    void set_username(const char* username);

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

  private:
    Page page_ = Page::checking;
    Action action_ = Action::sign_in;
    bool pending_ = false;
    std::string error_;
    std::string username_;
};

} // namespace wave
