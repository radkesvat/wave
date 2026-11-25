#pragma once

#include <imgui.h>
#include <qrcodegen.hpp>

#include <array>
#include <cstdint>
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
    enum class Dialog { create, edit, notes, share, reset_traffic, remove, broadcast, bulk_configuration, configuration, message };
    enum class Column { id, name, enabled, actions, upload_rate, download_rate, upload_total, download_total, remaining };

    ImGuiTextFilter search;
    Column sort_column = Column::id;
    bool descending = false;
    int compact_sort = 0;
    int compact_page = 0;
    std::optional<Dialog> dialog;
    bool open_dialog = false;
    UserDraft draft;
    std::array<char, 513> message{};
    std::string configuration;
    std::optional<qrcodegen::QrCode> qr;
    std::string pending_action;
    std::uint64_t feedback_revision = 0;
    std::string local_feedback;
};

void render_users(UsersViewState& state, PreviewSession& session, bool narrow);

} // namespace wave
