#include "users_view.h"

#include "browser_ui.h"
#include "preview_session.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using Dialog = UsersViewState::Dialog;
using Column = UsersViewState::Column;

constexpr ImVec4 mint{0.41f, 0.90f, 0.77f, 1.0f};
constexpr ImVec4 amber{0.97f, 0.75f, 0.47f, 1.0f};
constexpr const char* dialog_id = "###user-dialog";

const Json& field(const Json& object, std::string_view key)
{
    static const Json empty;
    const auto match = object.find(key);
    return match == object.end() ? empty : *match;
}

std::string_view text_field(const Json& object, std::string_view key)
{
    const auto& value = field(object, key);
    return value.is_string() ? std::string_view(value.get_ref<const std::string&>()) : std::string_view{};
}

template <typename Number>
Number number_field(const Json& object, std::string_view key, Number fallback = 0)
{
    const auto& value = field(object, key);
    return value.is_number() ? value.get<Number>() : fallback;
}

bool bool_field(const Json& object, std::string_view key)
{
    const auto& value = field(object, key);
    return value.is_boolean() && value.get<bool>();
}

template <std::size_t Size>
void copy_field(std::array<char, Size>& destination, std::string_view value)
{
    const auto length = std::min(value.size(), Size - 1);
    std::copy_n(value.begin(), length, destination.begin());
    destination[length] = '\0';
}

struct UserRow {
    const Json& record;
    int id;
    std::string_view name;
    bool enabled;
    double upload_rate;
    double download_rate;
    double upload_total;
    double download_total;
    int remaining_minutes;
};

UserRow read_user(const Json& user)
{
    return {user, number_field<int>(user, "id"), text_field(user, "name"), bool_field(user, "enabled"),
            number_field<double>(user, "upload_rate"), number_field<double>(user, "download_rate"),
            number_field<double>(user, "upload_total"), number_field<double>(user, "download_total"),
            number_field(user, "remaining_minutes", -1)};
}

UserDraft read_draft(const UserRow& user)
{
    UserDraft draft;
    draft.id = user.id;
    draft.enabled = user.enabled;
    draft.remaining_minutes = user.remaining_minutes;
    copy_field(draft.name, user.name);
    copy_field(draft.notes, text_field(user.record, "notes"));

    const auto& options = field(user.record, "options");
    draft.protocol = text_field(options, "protocol") == "tcp_websocket";
    draft.upload_limit = number_field<int>(options, "upload_limit");
    draft.download_limit = number_field<int>(options, "download_limit");
    draft.traffic_limit = number_field<int>(options, "traffic_limit");
    draft.ip_limit = number_field<int>(options, "ip_limit");
    return draft;
}

std::string remaining(int minutes)
{
    if (minutes < 0) return "Unlimited";
    if (minutes >= 1440) return std::format("{}d {}h", minutes / 1440, minutes / 60 % 24);
    if (minutes >= 60) return std::format("{}h {}m", minutes / 60, minutes % 60);
    return std::format("{}m", minutes);
}

void open_dialog(UsersViewState& state, Dialog dialog)
{
    state.dialog = dialog;
    state.open_dialog = true;
    state.draft = {};
    state.message = {};
    state.configuration.clear();
    state.qr.reset();
    state.pending_action.clear();
    state.local_feedback.clear();
}

void open_user_dialog(UsersViewState& state, Dialog dialog, const UserRow& user)
{
    open_dialog(state, dialog);
    state.draft = read_draft(user);
    if (dialog == Dialog::share) {
        state.configuration = text_field(user.record, "configuration");
        if (!state.configuration.empty() && state.configuration.size() <= 512) {
            state.qr = qrcodegen::QrCode::encodeText(state.configuration.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        }
    }
}

bool send(UsersViewState& state, PreviewSession& session, Json request)
{
    state.local_feedback.clear();
    if (!session.send(std::move(request))) {
        state.local_feedback = "Preview connection unavailable. Try again after reconnecting.";
        return false;
    }
    return true;
}

void submit(UsersViewState& state, PreviewSession& session, const char* action, Json request)
{
    request["action"] = action;
    const auto revision = session.feedback_revision();
    if (send(state, session, std::move(request))) {
        state.pending_action = action;
        state.feedback_revision = revision;
    }
}

void row_options(UsersViewState& state, const UserRow& user)
{
    if (ImGui::Button("Options")) ImGui::OpenPopup("User options");
    if (!ImGui::BeginPopup("User options")) return;

    if (ImGui::MenuItem("Edit")) open_user_dialog(state, Dialog::edit, user);
    if (ImGui::MenuItem("Notes")) open_user_dialog(state, Dialog::notes, user);
    if (ImGui::MenuItem("Share configuration")) open_user_dialog(state, Dialog::share, user);
    if (ImGui::MenuItem("Reset traffic")) open_user_dialog(state, Dialog::reset_traffic, user);
    if (ImGui::BeginMenu("Bot actions")) {
        if (ImGui::MenuItem("Copy demo invite")) {
            ImGui::SetClipboardText(std::format("wave-preview://workspace/bot/invite/{}", user.id).c_str());
            state.local_feedback = "Demo invite copied.";
        }
        if (ImGui::MenuItem("Send configuration")) open_user_dialog(state, Dialog::configuration, user);
        if (ImGui::MenuItem("Send message")) open_user_dialog(state, Dialog::message, user);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) open_user_dialog(state, Dialog::remove, user);
    ImGui::EndPopup();
}

void enabled_control(UsersViewState& state, PreviewSession& session, const UserRow& user, bool compact)
{
    bool enabled = user.enabled;
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? mint : amber);
    if (ImGui::Checkbox(compact ? "##enabled" : (enabled ? "Enabled##enabled" : "Disabled##enabled"), &enabled)) {
        send(state, session, {{"action", "set_user_enabled"}, {"id", user.id}, {"enabled", enabled}});
    }
    if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", enabled ? "Enabled" : "Disabled");
    ImGui::PopStyleColor();
}

template <typename Value>
int comparison(const Value& left, const Value& right)
{
    return (right < left) - (left < right);
}

int compare_users(const UserRow& left, const UserRow& right, Column column)
{
    switch (column) {
    case Column::name: return comparison(left.name, right.name);
    case Column::enabled: return comparison(left.enabled, right.enabled);
    case Column::upload_rate: return comparison(left.upload_rate, right.upload_rate);
    case Column::download_rate: return comparison(left.download_rate, right.download_rate);
    case Column::upload_total: return comparison(left.upload_total, right.upload_total);
    case Column::download_total: return comparison(left.download_total, right.download_total);
    case Column::remaining: {
        const auto rank = [](int minutes) { return minutes < 0 ? INT_MAX : minutes; };
        return comparison(rank(left.remaining_minutes), rank(right.remaining_minutes));
    }
    default: return comparison(left.id, right.id);
    }
}

std::vector<const UserRow*> sorted_users(const std::vector<UserRow>& users, const UsersViewState& state)
{
    std::vector<const UserRow*> order;
    for (const auto& user : users) {
        if (state.search.PassFilter(user.name.data(), user.name.data() + user.name.size())) order.push_back(&user);
    }
    std::ranges::sort(order, [&](const UserRow* left, const UserRow* right) {
        const auto sort = compare_users(*left, *right, state.sort_column);
        return sort == 0 ? left->id < right->id : (state.descending ? sort > 0 : sort < 0);
    });
    return order;
}

void desktop_users(UsersViewState& state, PreviewSession& session, const std::vector<UserRow>& users)
{
    constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                           ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("Users table", 9, flags, {0, 0})) return;

    constexpr auto fixed = ImGuiTableColumnFlags_WidthFixed;
    ImGui::TableSetupColumn("ID", fixed | ImGuiTableColumnFlags_DefaultSort, 32, static_cast<ImGuiID>(Column::id));
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1, static_cast<ImGuiID>(Column::name));
    ImGui::TableSetupColumn("State", fixed, 91, static_cast<ImGuiID>(Column::enabled));
    ImGui::TableSetupColumn("Actions", fixed | ImGuiTableColumnFlags_NoSort, 67, static_cast<ImGuiID>(Column::actions));
    ImGui::TableSetupColumn("Upload / s", fixed, 87, static_cast<ImGuiID>(Column::upload_rate));
    ImGui::TableSetupColumn("Download / s", fixed, 95, static_cast<ImGuiID>(Column::download_rate));
    ImGui::TableSetupColumn("Total up", fixed, 77, static_cast<ImGuiID>(Column::upload_total));
    ImGui::TableSetupColumn("Total down", fixed, 80, static_cast<ImGuiID>(Column::download_total));
    ImGui::TableSetupColumn("Time left", fixed, 81, static_cast<ImGuiID>(Column::remaining));
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
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 30);
            if (ImGui::TableNextColumn()) ImGui::TextDisabled("%d", user.id);
            if (ImGui::TableNextColumn()) {
                const auto* end = user.name.data() + user.name.size();
                ImGui::TextUnformatted(user.name.data(), end);
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(user.name.data(), end).x > ImGui::GetContentRegionAvail().x) {
                    ImGui::SetTooltip("%.*s", static_cast<int>(user.name.size()), user.name.data());
                }
            }
            if (ImGui::TableNextColumn()) enabled_control(state, session, user, false);
            if (ImGui::TableNextColumn()) row_options(state, user);
            if (ImGui::TableNextColumn()) ImGui::Text("%.2f MiB", user.upload_rate);
            if (ImGui::TableNextColumn()) ImGui::Text("%.2f MiB", user.download_rate);
            if (ImGui::TableNextColumn()) ImGui::Text("%.1f GiB", user.upload_total);
            if (ImGui::TableNextColumn()) ImGui::Text("%.1f GiB", user.download_total);
            if (ImGui::TableNextColumn()) ImGui::TextUnformatted(remaining(user.remaining_minutes).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void compact_users(UsersViewState& state, PreviewSession& session, const std::vector<UserRow>& users)
{
    constexpr const char* choices[] = {"ID", "Name", "Enabled", "Upload rate", "Download rate",
                                       "Total upload", "Total download", "Time left"};
    constexpr Column columns[] = {Column::id, Column::name, Column::enabled, Column::upload_rate,
                                  Column::download_rate, Column::upload_total, Column::download_total, Column::remaining};
    ImGui::SetNextItemWidth(146);
    if (ImGui::Combo("##user-sort", &state.compact_sort, choices, IM_ARRAYSIZE(choices))) state.compact_page = 0;
    state.sort_column = columns[state.compact_sort];
    ImGui::SameLine();
    if (ImGui::Button(state.descending ? "Descending" : "Ascending")) {
        state.descending = !state.descending;
        state.compact_page = 0;
    }

    const auto order = sorted_users(users, state);
    const float row_height = 4 * ImGui::GetTextLineHeightWithSpacing() + 2 * ImGui::GetStyle().CellPadding.y;
    const float table_height = ImGui::GetContentRegionAvail().y - 2 * ImGui::GetFrameHeightWithSpacing();
    const int page_size = std::clamp(static_cast<int>(table_height / row_height), 2, 5);
    const int final_page = std::max(0, (static_cast<int>(order.size()) - 1) / page_size);
    state.compact_page = std::min(state.compact_page, final_page);

    ImGui::BeginDisabled(state.compact_page == 0);
    if (ImGui::Button("Previous")) --state.compact_page;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.compact_page == final_page);
    if (ImGui::Button("Next")) ++state.compact_page;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Page %d of %d", state.compact_page + 1, final_page + 1);

    if (order.empty()) {
        ImGui::TextDisabled("No matching users");
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4, 5});
    const bool table_open = ImGui::BeginTable("Compact users table", 3,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_SizingStretchProp);
    ImGui::PopStyleVar();
    if (!table_open) return;

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1);
    ImGui::TableSetupColumn("Traffic", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 68);
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
        ImGui::Text("Up %.2f MiB/s", user.upload_rate);
        ImGui::Text("Down %.2f MiB/s", user.download_rate);
        ImGui::TextDisabled("Up %.1f GiB", user.upload_total);
        ImGui::TextDisabled("Down %.1f GiB", user.download_total);

        ImGui::TableNextColumn();
        enabled_control(state, session, user, true);
        ImGui::SameLine(0, 3);
        ImGui::TextColored(user.enabled ? mint : amber, "%s", user.enabled ? "On" : "Off");
        row_options(state, user);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void user_form(UserDraft& draft, bool initial_focus)
{
    if (initial_focus) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##user-name", "User name", draft.name.data(), draft.name.size());
    ImGui::Checkbox("Enabled", &draft.enabled);

    if (ImGui::CollapsingHeader("Connection and limits", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* protocols[] = {"TCP", "TCP + WebSocket"};
        ImGui::SetNextItemWidth(145);
        ImGui::Combo("Protocol", &draft.protocol, protocols, IM_ARRAYSIZE(protocols));
        ImGui::TextWrapped("Set traffic, rate, and IP limits to 0 for unlimited.");
        ImGui::SetNextItemWidth(86);
        ImGui::InputInt("Upload cap (MiB/s)", &draft.upload_limit, 0);
        ImGui::SetNextItemWidth(86);
        ImGui::InputInt("Download cap (MiB/s)", &draft.download_limit, 0);
        ImGui::SetNextItemWidth(86);
        ImGui::InputInt("Traffic cap (GiB)", &draft.traffic_limit, 0);
        ImGui::SetNextItemWidth(86);
        ImGui::InputInt("IP cap", &draft.ip_limit, 0);
        ImGui::SetNextItemWidth(86);
        ImGui::InputInt("Minutes left", &draft.remaining_minutes, 0);
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
        return;
    }
    if (draft.remaining_minutes < -1 || std::min({draft.upload_limit, draft.download_limit, draft.traffic_limit, draft.ip_limit}) < 0) {
        state.local_feedback = "Enter nonnegative limits or -1 for unlimited time.";
        return;
    }

    Json request = {{"name", draft.name.data()}, {"enabled", draft.enabled}, {"notes", draft.notes.data()},
                    {"remaining_minutes", draft.remaining_minutes},
                    {"options", {{"protocol", draft.protocol == 0 ? "tcp" : "tcp_websocket"},
                        {"upload_limit", draft.upload_limit}, {"download_limit", draft.download_limit},
                        {"traffic_limit", draft.traffic_limit}, {"ip_limit", draft.ip_limit}}}};
    if (editing) request["id"] = draft.id;
    submit(state, session, editing ? "edit_user" : "create_user", std::move(request));
}

void export_qr(const qrcodegen::QrCode& code)
{
    constexpr int quiet = 4;
    const int size = code.getSize() + 2 * quiet;
    auto svg = std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 {} {}\" shape-rendering=\"crispEdges\">"
                           "<rect width=\"100%\" height=\"100%\" fill=\"white\"/><path d=\"", size, size);
    for (int y = 0; y < code.getSize(); ++y) {
        for (int x = 0; x < code.getSize(); ++x) {
            if (code.getModule(x, y)) svg += std::format("M{} {}h1v1h-1z", x + quiet, y + quiet);
        }
    }
    svg += "\"/></svg>";
    download_preview_file("wave-demo-qr.svg", svg, "image/svg+xml");
}

void configuration_dialog(UsersViewState& state)
{
    ImGui::TextWrapped("Share the preview configuration for %s.", state.draft.name.data());
    if (!state.qr) {
        ImGui::TextDisabled("Configuration unavailable");
        return;
    }

    constexpr int quiet = 4;
    const int count = state.qr->getSize();
    const float module = std::max(1.0f, std::floor(std::min(235.0f, ImGui::GetContentRegionAvail().x) / (count + 2 * quiet)));
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
    if (ImGui::Button("Copy configuration")) {
        ImGui::SetClipboardText(state.configuration.c_str());
        state.local_feedback = "Preview configuration copied.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Download QR")) export_qr(*state.qr);
}

const char* dialog_title(Dialog dialog)
{
    switch (dialog) {
    case Dialog::create: return "Create user";
    case Dialog::edit: return "Edit user";
    case Dialog::notes: return "User notes";
    case Dialog::share: return "Share configuration";
    case Dialog::reset_traffic: return "Reset traffic";
    case Dialog::remove: return "Delete user";
    case Dialog::broadcast: return "Broadcast preview message";
    case Dialog::bulk_configuration: return "Send configurations to all";
    case Dialog::configuration: return "Send preview configuration";
    case Dialog::message: return "Send preview message";
    }
    return "User action";
}

void close_dialog(UsersViewState& state)
{
    ImGui::CloseCurrentPopup();
    state.dialog.reset();
    state.pending_action.clear();
    state.local_feedback.clear();
}

void render_dialog(UsersViewState& state, PreviewSession& session)
{
    if (!state.dialog) return;
    const bool opening = state.open_dialog;
    if (opening) {
        ImGui::OpenPopup(dialog_id);
        state.open_dialog = false;
    }

    const auto dialog = *state.dialog;
    const auto name = std::format("{}{}", dialog_title(dialog), dialog_id);
    fit_preview_dialog(460);
    bool open = true;
    if (!ImGui::BeginPopupModal(name.c_str(), &open, ImGuiWindowFlags_NoSavedSettings)) {
        if (!ImGui::IsPopupOpen(dialog_id)) state.dialog.reset();
        return;
    }

    if (!state.pending_action.empty()) {
        const auto* feedback = session.feedback_for(state.pending_action);
        if (session.status() != PreviewStatus::ready) {
            state.local_feedback = "Preview connection unavailable.";
            state.pending_action.clear();
        } else if (feedback && feedback->revision > state.feedback_revision) {
            if (feedback->ok) {
                close_dialog(state);
                ImGui::EndPopup();
                return;
            }
            state.local_feedback = feedback->message;
            state.pending_action.clear();
        }
    }

    const bool pending = !state.pending_action.empty();
    ImGui::BeginDisabled(pending);
    switch (dialog) {
    case Dialog::create:
    case Dialog::edit:
        user_form(state.draft, opening);
        if (ImGui::Button(dialog == Dialog::create ? "Create" : "Save changes")) {
            submit_user(state, session, dialog == Dialog::edit);
        }
        break;
    case Dialog::notes:
        ImGui::Text("%s", state.draft.name.data());
        ImGui::TextWrapped("%s", state.draft.notes[0] ? state.draft.notes.data() : "No notes for this user.");
        if (ImGui::Button("Edit notes")) state.dialog = Dialog::edit;
        break;
    case Dialog::share:
        configuration_dialog(state);
        break;
    case Dialog::reset_traffic:
    case Dialog::remove: {
        const bool remove = dialog == Dialog::remove;
        ImGui::TextWrapped(remove ? "Delete the preview user %s?" : "Reset upload and download totals for %s?",
                           state.draft.name.data());
        if (ImGui::Button(remove ? "Delete user" : "Reset totals")) {
            submit(state, session, remove ? "delete_user" : "reset_user_traffic", {{"id", state.draft.id}});
        }
        break;
    }
    case Dialog::broadcast:
    case Dialog::message:
        if (dialog == Dialog::broadcast) ImGui::TextUnformatted("Preview audience: all users");
        else ImGui::Text("Preview recipient: %s", state.draft.name.data());
        ImGui::InputTextMultiline("##preview-message", state.message.data(), state.message.size(), {-1, 105});
        if (ImGui::Button("Simulate send")) {
            if (std::string_view(state.message.data()).find_first_not_of(" \r\n\t") == std::string_view::npos) {
                state.local_feedback = "Enter a message to send.";
            } else {
                Json request = {{"kind", dialog == Dialog::broadcast ? "broadcast" : "message"}, {"message", state.message.data()}};
                if (dialog == Dialog::message) request["id"] = state.draft.id;
                submit(state, session, "user_bot_action", std::move(request));
            }
        }
        break;
    case Dialog::bulk_configuration:
    case Dialog::configuration:
        if (dialog == Dialog::bulk_configuration) ImGui::TextWrapped("Simulate sending the latest configuration to all users.");
        else ImGui::TextWrapped("Simulate sending a configuration to %s.", state.draft.name.data());
        if (ImGui::Button("Simulate send")) {
            Json request = {{"kind", "configuration"}};
            if (dialog == Dialog::configuration) request["id"] = state.draft.id;
            submit(state, session, "user_bot_action", std::move(request));
        }
        break;
    }
    ImGui::EndDisabled();

    if (dialog != Dialog::share) ImGui::SameLine();
    if (ImGui::Button(dialog == Dialog::share || dialog == Dialog::notes ? "Close" : "Cancel") || !open) {
        close_dialog(state);
    } else if (pending) {
        ImGui::TextDisabled("Applying to preview…");
    } else if (!state.local_feedback.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::TextWrapped("%s", state.local_feedback.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

} // namespace

void render_users(UsersViewState& state, PreviewSession& session, bool narrow)
{
    ImGui::PushID("users");
    ImGui::TextUnformatted("Users");
    const bool ready = session.status() == PreviewStatus::ready;
    const bool live = bool_field(session.data(), "users_live");
    const float width = ImGui::GetContentRegionAvail().x;
    const bool compact = narrow || width < 745.0f;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Create user")) open_dialog(state, Dialog::create);
    ImGui::SameLine();
    if (ImGui::Button(live ? "Live data" : "Paused")) {
        send(state, session, {{"action", "set_users_live"}, {"live", !live}});
    }
    if (width >= 285.0f) ImGui::SameLine();
    if (ImGui::Button("Bot actions")) ImGui::OpenPopup("User bot actions");
    if (ImGui::BeginPopup("User bot actions")) {
        if (ImGui::MenuItem("Send configurations to all")) open_dialog(state, Dialog::bulk_configuration);
        if (ImGui::MenuItem("Broadcast message")) open_dialog(state, Dialog::broadcast);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();

    if (!compact) ImGui::SameLine();
    ImGui::SetNextItemWidth(compact ? -64.0f : 190.0f);
    if (state.search.DrawWithHint("Search##users", "Name")) state.compact_page = 0;

    if (!ready) {
        ImGui::TextDisabled("%s", session.status() == PreviewStatus::loading ? "Loading preview users…" : "Preview users unavailable.");
    } else {
        const auto action = session.feedback_action();
        const auto feedback = session.feedback();
        if (!state.dialog && !state.local_feedback.empty()) {
            ImGui::TextWrapped("%s", state.local_feedback.c_str());
        } else if (!feedback.empty() && (action.ends_with("_user") || action.starts_with("set_user") ||
                   action.starts_with("user_") || action == "reset_user_traffic")) {
            ImGui::PushStyleColor(ImGuiCol_Text, session.feedback_ok() ? mint : amber);
            ImGui::TextWrapped("%.*s", static_cast<int>(feedback.size()), feedback.data());
            ImGui::PopStyleColor();
        }

        std::vector<UserRow> users;
        for (const auto& record : field(session.data(), "users")) {
            const auto row = read_user(record);
            if (!row.name.empty()) users.push_back(row);
        }
        if (compact) compact_users(state, session, users);
        else desktop_users(state, session, users);
    }
    render_dialog(state, session);
    ImGui::PopID();
}

} // namespace wave
