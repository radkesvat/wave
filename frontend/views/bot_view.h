#pragma once

#include "preview_session.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wave {

struct BotViewState {
    struct Notification {
        bool enabled = true;
        bool sound = false;
    };
    struct Save {
        std::optional<std::uint64_t> request;
        std::optional<PreviewReply> reply;
    };
    enum class Setup {
        intro,
        token,
        connecting,
        complete
    };
    enum class Dialog {
        none,
        setup,
        disconnect,
        message
    };

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
    Dialog requested_dialog = Dialog::none;
    std::string dialog_error;
    bool notifications = false;
    bool allow_status = false;
    bool allow_config = false;
    bool settings_dirty = false;
    bool overrides_dirty = false;
    bool access_link_copied = false;
};

void render_bot(BotViewState& state, PreviewSession& session, bool narrow);
void render_bot_dialogs(BotViewState& state, PreviewSession& session);

} // namespace wave
