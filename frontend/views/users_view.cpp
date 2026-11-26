#include "users_view.h"

#include "browser_ui.h"
#include "dialog.h"
#include "preview_session.h"
#include "view_helpers.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using UserDialog = UsersViewState::Dialog;
using Column = UsersViewState::Column;
using theme::Color;
using view::button_width;

constexpr const char* dialog_id = "###user-dialog";
constexpr const char* unavailable = "Preview connection unavailable. Try again after reconnecting.";

template <std::size_t Size> void copy_field(std::array<char, Size>& destination, std::string_view value)
{
    const auto length = std::min(value.size(), Size - 1);
    std::copy_n(value.begin(), length, destination.begin());
    destination[length] = '\0';
}

UserDraft make_draft(const UserRecord& user)
{
    UserDraft draft;
    draft.id = user.id;
    draft.enabled = user.enabled;
    draft.remaining_minutes = user.remaining_minutes;
    copy_field(draft.name, user.name);
    copy_field(draft.notes, user.notes);
    draft.protocol = user.protocol == UserProtocol::tcp_websocket;
    draft.upload_limit = user.upload_limit;
    draft.download_limit = user.download_limit;
    draft.traffic_limit = user.traffic_limit;
    draft.ip_limit = user.ip_limit;
    return draft;
}

std::string remaining(int minutes)
{
    if (minutes < 0) {
        return "Unlimited";
    }
    if (minutes >= 1440) {
        return std::format("{}d {}h", minutes / 1440, minutes / 60 % 24);
    }
    if (minutes >= 60) {
        return std::format("{}h {}m", minutes / 60, minutes % 60);
    }
    return std::format("{}m", minutes);
}

void open_dialog(UsersViewState& state, PreviewSession& session, UserDialog dialog)
{
    session.abandon(state.request_id);
    state.dialog = dialog;
    state.open_dialog = true;
    state.draft = {};
    state.message = {};
    state.configuration.clear();
    state.qr.reset();
    state.local_feedback.clear();
    state.reveal_dialog_feedback = false;
}

void open_user_dialog(UsersViewState& state, PreviewSession& session, UserDialog dialog,
                      const UserRecord& user)
{
    open_dialog(state, session, dialog);
    state.draft = make_draft(user);
    if (dialog == UserDialog::edit || dialog == UserDialog::notes) {
        state.original_draft = state.draft;
    }
    if (dialog == UserDialog::share) {
        state.configuration = user.configuration;
        if (!state.configuration.empty() && state.configuration.size() <= 512) {
            state.qr =
                qrcodegen::QrCode::encodeText(state.configuration.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        }
    }
}

void close_dialog(UsersViewState& state, PreviewSession& session)
{
    ImGui::CloseCurrentPopup();
    state.dialog.reset();
    session.abandon(state.request_id);
    state.local_feedback.clear();
}

bool send(UsersViewState& state, PreviewSession& session, Json request)
{
    state.local_feedback.clear();
    if (!session.send(std::move(request))) {
        state.local_feedback = unavailable;
        state.local_feedback_color = Color::error;
        return false;
    }
    return true;
}

void submit(UsersViewState& state, PreviewSession& session, const char* action, Json request)
{
    request["action"] = action;
    state.request_id = session.submit(std::move(request));
    state.local_feedback = state.request_id ? "" : unavailable;
    state.local_feedback_color = Color::error;
}

void row_options(UsersViewState& state, PreviewSession& session, const UserRecord& user)
{
    if (theme::button("Options")) {
        ImGui::OpenPopup("User options");
    }
    if (!ImGui::BeginPopup("User options")) {
        return;
    }

    if (ImGui::MenuItem("Edit")) {
        open_user_dialog(state, session, UserDialog::edit, user);
    }
    if (ImGui::MenuItem("Notes")) {
        open_user_dialog(state, session, UserDialog::notes, user);
    }
    if (ImGui::MenuItem("Share configuration")) {
        open_user_dialog(state, session, UserDialog::share, user);
    }
    if (ImGui::MenuItem("Reset traffic")) {
        open_user_dialog(state, session, UserDialog::reset_traffic, user);
    }
    if (ImGui::BeginMenu("Bot actions")) {
        if (ImGui::MenuItem("Copy demo invite")) {
            ImGui::SetClipboardText(std::format("wave-preview://workspace/bot/invite/{}", user.id).c_str());
            state.local_feedback = "Demo invite copied.";
            state.local_feedback_color = Color::success;
        }
        if (ImGui::MenuItem("Send configuration")) {
            open_user_dialog(state, session, UserDialog::configuration, user);
        }
        if (ImGui::MenuItem("Send message")) {
            open_user_dialog(state, session, UserDialog::message, user);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) {
        open_user_dialog(state, session, UserDialog::remove, user);
    }
    ImGui::EndPopup();
}

struct SwitchSize {
    float width;
    float height;
    ImVec2 hit;
};

SwitchSize switch_size(bool compact)
{
    return compact ? SwitchSize{38, 22, {44, 44}}
                   : SwitchSize{50, 28, {54, std::max(30.0f, ImGui::GetFrameHeight())}};
}

void update_switch_requests(UsersViewState& state, PreviewSession& session,
                            const std::vector<UserRecord>& users)
{
    for (auto it = state.switches.begin(); it != state.switches.end();) {
        auto& [user_id, attempt] = *it;
        if (const auto reply = session.take_reply(attempt.request_id); reply && !reply->ok()) {
            switch (reply->status) {
            case PreviewReply::Status::timed_out:
                attempt.error = "The preview did not confirm the change. Try again.";
                break;
            case PreviewReply::Status::disconnected:
                attempt.error = unavailable;
                break;
            default:
                attempt.error = reply->message;
                break;
            }
        }
        if (attempt.request_id) {
            ++it;
        } else if (attempt.error.empty()) {
            it = state.switches.erase(it);
        } else if (std::ranges::find(users, user_id, &UserRecord::id) == users.end()) {
            state.local_feedback = std::move(attempt.error);
            state.local_feedback_color = Color::error;
            it = state.switches.erase(it);
        } else {
            ++it;
        }
    }
}

void enabled_control(UsersViewState& state, PreviewSession& session, const UserRecord& user, bool compact)
{
    const auto size = switch_size(compact);
    const auto it = state.switches.find(user.id);
    const bool pending = it != state.switches.end() && it->second.request_id.has_value();
    const char* error = it == state.switches.end() ? nullptr : it->second.error.c_str();
    ImGui::BeginDisabled(pending);
    const bool toggled = ImGui::InvisibleButton("##enabled", size.hit, ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();

    if (ImGui::IsItemVisible()) {
        const auto origin = ImGui::GetItemRectMin();
        const ImVec2 min{origin.x + (size.hit.x - size.width) / 2, origin.y + (size.hit.y - size.height) / 2};
        const ImVec2 max{min.x + size.width, min.y + size.height};
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const bool failed = error && *error;
        const auto track = user.enabled ? Color::accent : (hovered ? Color::hover : Color::raised);
        const auto border = failed ? Color::error : Color::control_border;
        const auto thumb = user.enabled ? Color::canvas : Color::primary_text;
        const float radius = size.height / 2;
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, theme::packed(track), radius);
        if (!user.enabled || failed) {
            draw->AddRect(min, max, theme::packed(border), radius, theme::border_width);
        }
        draw->AddCircleFilled({user.enabled ? max.x - radius : min.x + radius, min.y + radius}, radius - 3,
                              theme::packed(thumb));
        if (hovered && !pending) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) ||
            (ImGui::GetIO().NavVisible && ImGui::IsItemFocused())) {
            const char* label = user.enabled ? "Enabled" : "Disabled";
            if (failed) {
                ImGui::SetTooltip("%s\n%s", label, error);
            } else {
                ImGui::SetTooltip("%s", label);
            }
        }
    }
    if (toggled) {
        auto& attempt = state.switches[user.id];
        attempt.request_id = session.submit(
            {{"action", "set_user_enabled"}, {"id", user.id}, {"enabled", !user.enabled}}, 5000);
        attempt.error = attempt.request_id ? "" : unavailable;
    }
}

template <typename Value> int comparison(const Value& left, const Value& right)
{
    return (right < left) - (left < right);
}

int compare_users(const UserRecord& left, const UserRecord& right, Column column)
{
    switch (column) {
    case Column::name:
        return comparison(left.name, right.name);
    case Column::enabled:
        return comparison(left.enabled, right.enabled);
    case Column::upload_rate:
        return comparison(left.upload_rate, right.upload_rate);
    case Column::download_rate:
        return comparison(left.download_rate, right.download_rate);
    case Column::upload_total:
        return comparison(left.upload_total, right.upload_total);
    case Column::download_total:
        return comparison(left.download_total, right.download_total);
    case Column::remaining: {
        const auto rank = [](int minutes) { return minutes < 0 ? INT_MAX : minutes; };
        return comparison(rank(left.remaining_minutes), rank(right.remaining_minutes));
    }
    default:
        return comparison(left.id, right.id);
    }
}

std::vector<const UserRecord*> sorted_users(const std::vector<UserRecord>& users, const UsersViewState& state)
{
    std::vector<const UserRecord*> order;
    for (const auto& user : users) {
        if (state.search.PassFilter(user.name.data(), user.name.data() + user.name.size())) {
            order.push_back(&user);
        }
    }
    std::ranges::sort(order, [&](const UserRecord* left, const UserRecord* right) {
        const auto sort = compare_users(*left, *right, state.sort_column);
        return sort == 0 ? left->id < right->id : (state.descending ? sort > 0 : sort < 0);
    });
    return order;
}

struct DesktopColumn {
    const char* label;
    Column id;
    const char* sample;
    int width_weight;
    ImGuiTableColumnFlags flags = 0;
};

constexpr std::array desktop_columns{
    DesktopColumn{"ID", Column::id, "9999", 0, ImGuiTableColumnFlags_DefaultSort},
    DesktopColumn{"Name", Column::name, "Avery Brooks", 3},
    DesktopColumn{"State", Column::enabled, "", 0},
    DesktopColumn{"Actions", Column::actions, "", 0, ImGuiTableColumnFlags_NoSort},
    DesktopColumn{"Upload / s", Column::upload_rate, "100.00 MiB", 1},
    DesktopColumn{"Download / s", Column::download_rate, "100.00 MiB", 1},
    DesktopColumn{"Total up", Column::upload_total, "999.9 GiB", 1},
    DesktopColumn{"Total down", Column::download_total, "999.9 GiB", 1},
    DesktopColumn{"Time left", Column::remaining, "3650d 23h", 1},
};

struct DesktopTableWidths {
    std::array<float, desktop_columns.size()> minimum{};
    std::array<float, desktop_columns.size()> initial{};
    float minimum_table;
};

DesktopTableWidths measure_desktop_table(float available_width)
{
    DesktopTableWidths widths;
    const auto& style = ImGui::GetStyle();
    // ImGui adds a logical pixel at each outer edge and between columns.
    const float borders = desktop_columns.size() + 1;
    widths.minimum_table = style.ScrollbarSize + 2 * style.CellPadding.x * desktop_columns.size() + borders;
    const float sort_arrow_width = std::floor(ImGui::GetFontSize() * 0.65f + style.FramePadding.x);
    int total_weight = 0;
    for (std::size_t i = 0; i < desktop_columns.size(); ++i) {
        const auto& column = desktop_columns[i];
        const float label_width = ImGui::CalcTextSize(column.label).x;
        const float header_width =
            label_width + ((column.flags & ImGuiTableColumnFlags_NoSort) ? 0 : sort_arrow_width);
        float content = ImGui::CalcTextSize(column.sample).x;
        if (column.id == Column::name) {
            content = std::max(content + style.CellPadding.x, ImGui::GetFontSize() * 7);
        } else if (column.id == Column::enabled) {
            content = switch_size(false).hit.x;
        } else if (column.id == Column::actions) {
            content = button_width("Options");
        }
        widths.minimum[i] = std::ceil(std::max(header_width, content));
        widths.minimum_table += widths.minimum[i];
        total_weight += column.width_weight;
    }
    const float extra = std::max(0.0f, std::floor(available_width - widths.minimum_table));
    float allocated = 0;
    for (std::size_t i = 0; i < desktop_columns.size(); ++i) {
        const float growth = std::floor(extra * desktop_columns[i].width_weight / total_weight);
        widths.initial[i] = widths.minimum[i] + growth;
        allocated += growth;
    }
    widths.initial[static_cast<std::size_t>(Column::name)] += extra - allocated;
    return widths;
}

void setup_desktop_columns(UsersViewState& state, const DesktopTableWidths& widths)
{
    auto& table = *ImGui::GetCurrentTable();
    const float font_scale = table.RefScale > 0 ? ImGui::GetFontSize() / table.RefScale : 1;
    // During layout, ImGui applies the queued drag before scaling the column widths.
    if (table.ResizedColumn != -1 && table.ResizedColumnNextWidth != FLT_MAX) {
        const auto column = static_cast<std::size_t>(table.ResizedColumn);
        const float minimum = widths.minimum[column] / font_scale;
        table.ResizedColumnNextWidth = std::max(table.ResizedColumnNextWidth, minimum);
    }

    for (std::size_t i = 0; i < desktop_columns.size(); ++i) {
        const auto& column = desktop_columns[i];
        ImGui::TableSetupColumn(column.label, column.flags | ImGuiTableColumnFlags_WidthFixed,
                                widths.initial[i], static_cast<ImGuiID>(column.id));

        float& request = table.Columns[i].WidthRequest;
        if (!table.IsInitializing && request > 0) {
            if (std::abs(request - state.desktop_default_widths[i]) < 0.5f) {
                request = widths.initial[i] / font_scale;
            } else {
                request = std::max(request, widths.minimum[i] / font_scale);
            }
        }
        state.desktop_default_widths[i] = widths.initial[i];
    }
}

void desktop_users(UsersViewState& state, PreviewSession& session, const std::vector<UserRecord>& users,
                   const DesktopTableWidths& widths)
{
    constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("Users table", desktop_columns.size(), flags, {0, 0})) {
        return;
    }

    setup_desktop_columns(state, widths);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount != 0) {
        state.sort_column = static_cast<Column>(specs->Specs[0].ColumnUserID);
        state.descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        specs->SpecsDirty = false;
    }
    const auto order = sorted_users(users, state);
    if (order.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("No matching users");
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(order.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& user = *order[i];
            ImGui::PushID(user.id);
            ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                switch_size(false).hit.y + 2 * ImGui::GetStyle().CellPadding.y);
            if (ImGui::TableNextColumn()) {
                ImGui::TextDisabled("%d", user.id);
            }
            if (ImGui::TableNextColumn()) {
                const auto* end = user.name.data() + user.name.size();
                ImGui::TextUnformatted(user.name.data(), end);
                if (ImGui::IsItemHovered() &&
                    ImGui::CalcTextSize(user.name.data(), end).x > ImGui::GetContentRegionAvail().x) {
                    ImGui::SetTooltip("%.*s", static_cast<int>(user.name.size()), user.name.data());
                }
            }
            if (ImGui::TableNextColumn()) {
                enabled_control(state, session, user, false);
            }
            if (ImGui::TableNextColumn()) {
                row_options(state, session, user);
            }
            if (ImGui::TableNextColumn()) {
                ImGui::Text("%.2f MiB", user.upload_rate);
            }
            if (ImGui::TableNextColumn()) {
                ImGui::Text("%.2f MiB", user.download_rate);
            }
            if (ImGui::TableNextColumn()) {
                ImGui::Text("%.1f GiB", user.upload_total);
            }
            if (ImGui::TableNextColumn()) {
                ImGui::Text("%.1f GiB", user.download_total);
            }
            if (ImGui::TableNextColumn()) {
                ImGui::TextUnformatted(remaining(user.remaining_minutes).c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void compact_users(UsersViewState& state, PreviewSession& session, const std::vector<UserRecord>& users)
{
    constexpr const char* choices[] = {
        "ID",           "Name",           "Enabled",  "Upload rate", "Download rate",
        "Total upload", "Total download", "Time left"};
    constexpr Column columns[] = {Column::id,
                                  Column::name,
                                  Column::enabled,
                                  Column::upload_rate,
                                  Column::download_rate,
                                  Column::upload_total,
                                  Column::download_total,
                                  Column::remaining};
    const auto& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const float sort_width =
        ImGui::CalcTextSize("Total download").x + ImGui::GetFrameHeight() + 2 * style.FramePadding.x;
    ImGui::SetNextItemWidth(
        std::max(1.0f, std::min(sort_width, width - button_width("Descending") - style.ItemSpacing.x)));
    if (ImGui::Combo("##user-sort", &state.compact_sort, choices, IM_ARRAYSIZE(choices))) {
        state.compact_page = 0;
    }
    state.sort_column = columns[state.compact_sort];
    ImGui::SameLine();
    if (theme::button(state.descending ? "Descending" : "Ascending")) {
        state.descending = !state.descending;
        state.compact_page = 0;
    }

    const auto order = sorted_users(users, state);
    const float traffic_font = width < 450 ? theme::annotation_font_size : theme::secondary_font_size;
    ImGui::PushFont(nullptr, traffic_font);
    const float traffic_text_width = ImGui::CalcTextSize("Down 10.00 MiB/s").x;
    const float traffic_width = std::min(traffic_text_width + 4, std::floor(width * 0.4f));
    const float traffic_height =
        (traffic_width >= traffic_text_width ? 4 : 5) * ImGui::GetTextLineHeightWithSpacing();
    ImGui::PopFont();
    const float row_height =
        std::max(traffic_height, 4 * ImGui::GetTextLineHeightWithSpacing()) + 2 * style.CellPadding.y;
    const float table_height = ImGui::GetContentRegionAvail().y - 2 * ImGui::GetFrameHeightWithSpacing();
    const int page_size = std::clamp(static_cast<int>(table_height / row_height), 2, 5);
    const int final_page = std::max(0, (static_cast<int>(order.size()) - 1) / page_size);
    state.compact_page = std::min(state.compact_page, final_page);

    ImGui::BeginDisabled(state.compact_page == 0);
    if (theme::button("Previous")) {
        --state.compact_page;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.compact_page == final_page);
    if (theme::button("Next")) {
        ++state.compact_page;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Page %d of %d", state.compact_page + 1, final_page + 1);

    if (order.empty()) {
        ImGui::TextDisabled("No matching users");
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4, 5});
    const bool table_open =
        ImGui::BeginTable("Compact users table", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp);
    ImGui::PopStyleVar();
    if (!table_open) {
        return;
    }

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1);
    ImGui::TableSetupColumn("Traffic", ImGuiTableColumnFlags_WidthFixed, traffic_width);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,
                            std::max(button_width("Options"), ImGui::CalcTextSize("Actions").x));
    ImGui::TableHeadersRow();
    const int first = state.compact_page * page_size;
    const int last = std::min(first + page_size, static_cast<int>(order.size()));
    for (int i = first; i < last; ++i) {
        const auto& user = *order[i];
        ImGui::PushID(user.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%.*s", static_cast<int>(user.name.size()), user.name.data());
        ImGui::TextDisabled("#%d", user.id);
        ImGui::TextWrapped("%s", remaining(user.remaining_minutes).c_str());

        ImGui::TableNextColumn();
        ImGui::PushFont(nullptr, traffic_font);
        ImGui::TextWrapped("Up %.2f MiB/s", user.upload_rate);
        ImGui::TextWrapped("Down %.2f MiB/s", user.download_rate);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgba(Color::secondary_text));
        ImGui::TextWrapped("Up %.1f GiB", user.upload_total);
        ImGui::TextWrapped("Down %.1f GiB", user.download_total);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::TableNextColumn();
        enabled_control(state, session, user, true);
        row_options(state, session, user);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void user_limit(const char* label, int& value, bool stacked)
{
    if (stacked) {
        view::edit_number(label, value);
    } else {
        ImGui::SetNextItemWidth(86);
        ImGui::InputInt(label, &value, 0);
    }
}

void user_form(UserDraft& draft, bool initial_focus)
{
    ImGui::TextUnformatted("User name");
    if (initial_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##user-name", draft.name.data(), draft.name.size());
    ImGui::Checkbox("Enabled", &draft.enabled);

    if (ImGui::CollapsingHeader("Connection and limits", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* protocols[] = {"TCP", "TCP + WebSocket"};
        const float protocol_width = button_width(protocols[1]) + ImGui::GetFrameHeight();
        const float label_spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float available_width = ImGui::GetContentRegionAvail().x;
        if (available_width < protocol_width + label_spacing + ImGui::CalcTextSize("Protocol").x) {
            ImGui::TextUnformatted("Protocol");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##protocol", &draft.protocol, protocols, IM_ARRAYSIZE(protocols));
        } else {
            ImGui::SetNextItemWidth(protocol_width);
            ImGui::Combo("Protocol", &draft.protocol, protocols, IM_ARRAYSIZE(protocols));
        }
        ImGui::TextWrapped("Set traffic, rate, and IP limits to 0 for unlimited.");
        const bool stacked =
            available_width < 86 + label_spacing + ImGui::CalcTextSize("Download cap (MiB/s)").x;
        user_limit("Upload cap (MiB/s)", draft.upload_limit, stacked);
        user_limit("Download cap (MiB/s)", draft.download_limit, stacked);
        user_limit("Traffic cap (GiB)", draft.traffic_limit, stacked);
        user_limit("IP cap", draft.ip_limit, stacked);
        user_limit("Minutes left", draft.remaining_minutes, stacked);
        ImGui::TextDisabled("Use -1 for unlimited time.");
    }
    ImGui::TextDisabled("Notes");
    ImGui::InputTextMultiline("##user-note", draft.notes.data(), draft.notes.size(), {-1, 68});
}

void submit_user(UsersViewState& state, PreviewSession& session, bool editing)
{
    const auto& draft = state.draft;
    if (std::string_view(draft.name.data()).find_first_not_of(" \r\n\t") == std::string_view::npos) {
        state.local_feedback = "Enter a user name.";
        state.local_feedback_color = Color::error;
        return;
    }
    if (draft.remaining_minutes < -1 ||
        std::min({draft.upload_limit, draft.download_limit, draft.traffic_limit, draft.ip_limit}) < 0) {
        state.local_feedback = "Enter nonnegative limits or -1 for unlimited time.";
        state.local_feedback_color = Color::error;
        return;
    }

    const auto& original = state.original_draft;
    const auto add_field = [editing](Json& target, const char* key, auto value, auto initial_value) {
        if (!editing || value != initial_value) {
            target[key] = value;
        }
    };
    Json request = Json::object();
    add_field(request, "name", std::string_view(draft.name.data()), std::string_view(original.name.data()));
    add_field(request, "enabled", draft.enabled, original.enabled);
    add_field(request, "notes", std::string_view(draft.notes.data()),
              std::string_view(original.notes.data()));
    add_field(request, "remaining_minutes", draft.remaining_minutes, original.remaining_minutes);

    Json options = Json::object();
    if (!editing || draft.protocol != original.protocol) {
        options["protocol"] = draft.protocol == 0 ? "tcp" : "tcp_websocket";
    }
    add_field(options, "upload_limit", draft.upload_limit, original.upload_limit);
    add_field(options, "download_limit", draft.download_limit, original.download_limit);
    add_field(options, "traffic_limit", draft.traffic_limit, original.traffic_limit);
    add_field(options, "ip_limit", draft.ip_limit, original.ip_limit);
    if (!options.empty()) {
        request["options"] = std::move(options);
    }
    if (editing) {
        if (request.empty()) {
            close_dialog(state, session);
            return;
        }
        request["id"] = draft.id;
    }
    submit(state, session, editing ? "edit_user" : "create_user", std::move(request));
}

void export_qr(const qrcodegen::QrCode& code)
{
    constexpr int quiet = 4;
    const int size = code.getSize() + 2 * quiet;
    auto svg = std::format(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 {} {}\" shape-rendering=\"crispEdges\">"
        "<rect width=\"100%\" height=\"100%\" fill=\"white\"/><path d=\"",
        size, size);
    for (int y = 0; y < code.getSize(); ++y) {
        for (int x = 0; x < code.getSize(); ++x) {
            if (code.getModule(x, y)) {
                svg += std::format("M{} {}h1v1h-1z", x + quiet, y + quiet);
            }
        }
    }
    svg += "\"/></svg>";
    download_preview_file("wave-demo-qr.svg", svg, "image/svg+xml");
}

void configuration_preview(const UsersViewState& state)
{
    ImGui::TextWrapped("Share the preview configuration for %s.", state.draft.name.data());
    if (!state.qr) {
        ImGui::TextDisabled("Configuration unavailable");
        return;
    }

    constexpr int quiet = 4;
    const int count = state.qr->getSize();
    const float module =
        std::max(1.0f, std::floor(std::min(235.0f, ImGui::GetContentRegionAvail().x) / (count + 2 * quiet)));
    const float size = (count + 2 * quiet) * module;
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size, origin.y + size}, IM_COL32_WHITE);
    for (int y = 0; y < count; ++y) {
        for (int x = 0; x < count; ++x) {
            if (state.qr->getModule(x, y)) {
                const auto left = origin.x + (x + quiet) * module;
                const auto top = origin.y + (y + quiet) * module;
                draw->AddRectFilled({left, top}, {left + module, top + module}, IM_COL32_BLACK);
            }
        }
    }
    ImGui::Dummy({size, size});
    ImGui::TextWrapped("%s", state.configuration.c_str());
}

struct DialogSpec {
    const char* title;
    float width;
    const char* secondary;
    const char* primary;
};

DialogSpec dialog_spec(UserDialog dialog)
{
    switch (dialog) {
    case UserDialog::create:
        return {"Create user", 460, "Cancel", "Create"};
    case UserDialog::edit:
        return {"Edit user", 460, "Cancel", "Save changes"};
    case UserDialog::notes:
        return {"User notes", 480, "Close", "Edit notes"};
    case UserDialog::share:
        return {"Share configuration", 510, "Close", "Copy configuration"};
    case UserDialog::reset_traffic:
        return {"Reset traffic", 350, "Cancel", "Reset totals"};
    case UserDialog::remove:
        return {"Delete user", 340, "Cancel", "Delete user"};
    case UserDialog::broadcast:
        return {"Broadcast preview message", 480, "Cancel", "Simulate send"};
    case UserDialog::bulk_configuration:
        return {"Send configurations to all", 420, "Cancel", "Simulate send"};
    case UserDialog::configuration:
        return {"Send preview configuration", 420, "Cancel", "Simulate send"};
    case UserDialog::message:
        return {"Send preview message", 460, "Cancel", "Simulate send"};
    }
    return {"User action", 350, "Cancel", "Confirm"};
}

void user_dialog_body(UsersViewState& state, UserDialog dialog, bool opening)
{
    switch (dialog) {
    case UserDialog::create:
    case UserDialog::edit:
        user_form(state.draft, opening);
        break;
    case UserDialog::notes:
        ImGui::TextWrapped("%s", state.draft.name.data());
        ImGui::TextWrapped("%s", state.draft.notes[0] ? state.draft.notes.data() : "No notes for this user.");
        break;
    case UserDialog::share:
        configuration_preview(state);
        break;
    case UserDialog::remove:
        ImGui::TextWrapped("Delete the preview user %s?", state.draft.name.data());
        break;
    case UserDialog::reset_traffic:
        ImGui::TextWrapped("Reset upload and download totals for %s?", state.draft.name.data());
        break;
    case UserDialog::broadcast:
    case UserDialog::message:
        if (dialog == UserDialog::broadcast) {
            ImGui::TextUnformatted("Preview audience: all users");
        } else {
            ImGui::TextWrapped("Preview recipient: %s", state.draft.name.data());
        }
        ImGui::InputTextMultiline("##preview-message", state.message.data(), state.message.size(), {-1, 105});
        break;
    case UserDialog::bulk_configuration:
    case UserDialog::configuration:
        if (dialog == UserDialog::bulk_configuration) {
            ImGui::TextWrapped("Simulate sending the latest configuration to all users.");
        } else {
            ImGui::TextWrapped("Simulate sending a configuration to %s.", state.draft.name.data());
        }
        break;
    }
}

void apply_user_dialog(UsersViewState& state, PreviewSession& session, UserDialog dialog)
{
    switch (dialog) {
    case UserDialog::create:
    case UserDialog::edit:
        submit_user(state, session, dialog == UserDialog::edit);
        break;
    case UserDialog::notes:
        state.dialog = UserDialog::edit;
        break;
    case UserDialog::share:
        ImGui::SetClipboardText(state.configuration.c_str());
        state.local_feedback = "Preview configuration copied.";
        state.local_feedback_color = Color::success;
        break;
    case UserDialog::reset_traffic:
    case UserDialog::remove:
        submit(state, session, dialog == UserDialog::remove ? "delete_user" : "reset_user_traffic",
               {{"id", state.draft.id}});
        break;
    case UserDialog::broadcast:
    case UserDialog::message: {
        if (std::string_view(state.message.data()).find_first_not_of(" \r\n\t") == std::string_view::npos) {
            state.local_feedback = "Enter a message to send.";
            state.local_feedback_color = Color::error;
            break;
        }
        Json request = {{"kind", dialog == UserDialog::broadcast ? "broadcast" : "message"},
                        {"message", state.message.data()}};
        if (dialog == UserDialog::message) {
            request["id"] = state.draft.id;
        }
        submit(state, session, "user_bot_action", std::move(request));
        break;
    }
    case UserDialog::bulk_configuration:
    case UserDialog::configuration: {
        Json request = {{"kind", "configuration"}};
        if (dialog == UserDialog::configuration) {
            request["id"] = state.draft.id;
        }
        submit(state, session, "user_bot_action", std::move(request));
        break;
    }
    }
}

void render_dialog(UsersViewState& state, PreviewSession& session)
{
    if (!state.dialog) {
        return;
    }
    const bool opening = std::exchange(state.open_dialog, false);
    if (opening) {
        ImGui::OpenPopup(dialog_id);
    }
    if (!ImGui::IsPopupOpen(dialog_id)) {
        state.dialog.reset();
        session.abandon(state.request_id);
        return;
    }

    const auto reply = session.take_reply(state.request_id);
    if (reply && !reply->ok()) {
        state.local_feedback = reply->status == PreviewReply::Status::disconnected
                                   ? "Preview connection unavailable."
                                   : reply->message;
        state.local_feedback_color = Color::error;
        state.reveal_dialog_feedback = !state.local_feedback.empty();
    }
    const bool pending = state.request_id.has_value();
    const auto dialog = *state.dialog;
    const auto spec = dialog_spec(dialog);
    const auto name = std::format("{}{}", spec.title, dialog_id);
    const bool sharing = dialog == UserDialog::share;
    const std::array<DialogAction, 3> actions{{{spec.secondary}, {spec.primary, pending}, {"Download QR"}}};
    const auto count = sharing ? (state.qr ? 3u : 1u) : 2u;
    bool open = true;
    wave::Dialog frame(name.c_str(), spec.width, std::span(actions).first(count), &open);
    if (!frame) {
        return;
    }

    ImGui::BeginDisabled(pending);
    user_dialog_body(state, dialog, opening);
    ImGui::EndDisabled();
    if (pending) {
        view::wrapped("Applying to preview…", Color::info);
    } else if (!state.local_feedback.empty()) {
        view::wrapped(state.local_feedback, state.local_feedback_color);
    }
    if (std::exchange(state.reveal_dialog_feedback, false)) {
        ImGui::SetScrollHereY(1);
    }

    const int action = frame.footer();
    if (action == 0 || !open || (reply && reply->ok())) {
        close_dialog(state, session);
    } else if (action == 1) {
        apply_user_dialog(state, session, dialog);
        state.reveal_dialog_feedback = state.request_id.has_value() || !state.local_feedback.empty();
    } else if (action == 2) {
        export_qr(*state.qr);
    }
}

} // namespace

void render_users(UsersViewState& state, PreviewSession& session)
{
    ImGui::PushID("users");
    ImGui::TextUnformatted("Users");
    const bool ready = session.status() == PreviewStatus::ready;
    const auto& users = session.users();
    const float width = ImGui::GetContentRegionAvail().x;
    const auto desktop_widths = measure_desktop_table(width);
    const bool compact = width < desktop_widths.minimum_table;
    update_switch_requests(state, session, users.rows);
    ImGui::BeginDisabled(!ready);
    if (theme::button("Create user")) {
        open_dialog(state, session, UserDialog::create);
    }
    ImGui::SameLine();
    const float live_width = std::max(button_width("Live data"), button_width("Paused"));
    const char* live_label = users.live ? "Live data" : "Paused";
    if (theme::button(live_label, {live_width, 0})) {
        send(state, session, {{"action", "set_users_live"}, {"live", !users.live}});
    }
    const auto& style = ImGui::GetStyle();
    if (width >=
        button_width("Create user") + live_width + button_width("Bot actions") + 2 * style.ItemSpacing.x) {
        ImGui::SameLine();
    }
    if (theme::button("Bot actions")) {
        ImGui::OpenPopup("User bot actions");
    }
    if (ImGui::BeginPopup("User bot actions")) {
        if (ImGui::MenuItem("Send configurations to all")) {
            open_dialog(state, session, UserDialog::bulk_configuration);
        }
        if (ImGui::MenuItem("Broadcast message")) {
            open_dialog(state, session, UserDialog::broadcast);
        }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();

    if (!compact) {
        ImGui::SameLine();
    }
    const float search_label_width = ImGui::CalcTextSize("Search").x + style.ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(compact ? -search_label_width : ImGui::GetFontSize() * 11);
    if (state.search.DrawWithHint("Search##users", "Name")) {
        state.compact_page = 0;
    }

    if (!ready) {
        ImGui::TextDisabled("%s", session.status() == PreviewStatus::loading ? "Loading preview users…"
                                                                             : "Preview users unavailable.");
    } else {
        if (!state.dialog && !state.local_feedback.empty()) {
            view::wrapped(state.local_feedback, state.local_feedback_color);
        } else if (const auto* feedback = session.feedback_for("set_users_live");
                   feedback && !feedback->ok && !feedback->message.empty()) {
            view::wrapped(feedback->message, Color::error);
        }

        if (compact) {
            compact_users(state, session, users.rows);
        } else {
            desktop_users(state, session, users.rows, desktop_widths);
        }
    }
    ImGui::PopID();
    render_users_dialog(state, session);
}

void render_users_dialog(UsersViewState& state, PreviewSession& session)
{
    ImGui::PushID("users");
    render_dialog(state, session);
    ImGui::PopID();
}

} // namespace wave
