#pragma once

#include "browser_ui.h"
#include "preview_session.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

struct ConfigUsersState {
    enum class Editor {
        none,
        add,
        edit,
        remove
    };

    enum class Section {
        account,
        access,
        limits,
        activity
    };

    enum class Operation {
        list,
        password,
        mutation
    };

    Editor editor = Editor::none;
    Section section = Section::account;
    Operation operation = Operation::list;
    nlohmann::json users;
    nlohmann::json selected_user;
    std::optional<std::uint64_t> request;
    std::string name;
    std::string email;
    std::string notes;
    std::string wireguard_ips;
    std::string password;
    std::string saved_password;
    std::map<std::string_view, std::string> numbers;
    std::string validation;
    std::string read_error;
    std::string user_id;
    bool open_account = false;
    bool enabled = true;
    bool editable = false;
    bool password_unavailable = false;
    bool passive_read = false;
    bool blocked = false;
    bool refresh_needed = true;
    std::size_t page = 0;
    std::size_t page_count = 0;
    std::size_t total = 0;
    int last_frame = -1;
    CanvasViewport scroll_bounds{};
};

void render_config_users(ConfigUsersState& state, PreviewSession& session, int config_id,
                         std::string_view node, bool offline_available, bool narrow);
std::string describe_config_users(const ConfigUsersState& state);

} // namespace wave
