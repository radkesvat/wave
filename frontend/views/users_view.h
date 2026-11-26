#pragma once

#include "theme.h"

#include <imgui.h>
#include <qrcodegen.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace wave {

class PreviewSession;

struct UserDraft {
    int id = 0;
    std::array<char, 65> name{};
    bool enabled = true;
    int remaining_minutes = -1;
    std::array<char, 769> notes{};
    int protocol = 0;
    int upload_limit = 0;
    int download_limit = 0;
    int traffic_limit = 0;
    int ip_limit = 0;
};

struct UsersViewState {
    enum class Dialog {
        create,
        edit,
        notes,
        share,
        reset_traffic,
        remove,
        broadcast,
        bulk_configuration,
        configuration,
        message
    };
    enum class Column {
        id,
        name,
        enabled,
        actions,
        upload_rate,
        download_rate,
        upload_total,
        download_total,
        remaining,
        count
    };

    struct SwitchRequest {
        std::optional<std::uint64_t> request_id;
        std::string error;
    };

    ImGuiTextFilter search;
    Column sort_column = Column::id;
    bool descending = false;
    int compact_sort = 0;
    int compact_page = 0;
    std::array<float, static_cast<std::size_t>(Column::count)> desktop_default_widths{};
    std::map<int, SwitchRequest> switches;
    std::optional<Dialog> dialog;
    bool open_dialog = false;
    UserDraft draft;
    UserDraft original_draft;
    std::array<char, 513> message{};
    std::string configuration;
    std::optional<qrcodegen::QrCode> qr;
    std::optional<std::uint64_t> request_id;
    std::string local_feedback;
    theme::Color local_feedback_color = theme::Color::error;
    bool reveal_dialog_feedback = false;
};

void render_users(UsersViewState& state, PreviewSession& session);
void render_users_dialog(UsersViewState& state, PreviewSession& session);

} // namespace wave
