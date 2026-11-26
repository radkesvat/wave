#include "bot_view.h"

#include "browser_ui.h"
#include "view_helpers.h"

#include <utility>

namespace wave {

namespace {

using namespace view;
using theme::Color;
using theme::rgba;

struct Option {
    const char* key;
    const char* label;
};

constexpr std::array notification_options{
    Option{"login_failed", "Demo sign-in failed"},    Option{"login_succeeded", "Demo sign-in succeeded"},
    Option{"settings_changed", "Settings changed"},   Option{"high_cpu", "High CPU usage"},
    Option{"high_memory", "High memory usage"},       Option{"server_reboot", "Server reboot"},
    Option{"panel_started", "Panel started"},         Option{"user_expired", "User duration expired"},
    Option{"traffic_limit", "Traffic limit reached"}, Option{"user_created", "User created"},
    Option{"user_disabled", "User disabled"},         Option{"user_enabled", "User enabled"},
    Option{"user_deleted", "User deleted"},           Option{"user_support", "User requests support"},
};

constexpr std::array permission_options{
    Option{"status", "View status"},
    Option{"traffic", "View traffic usage"},
    Option{"ip_limit", "View address limit"},
    Option{"speed_limit", "View speed limit"},
    Option{"traffic_limit", "View traffic limit"},
    Option{"expiration", "View time remaining"},
    Option{"notes", "View user notes"},
};

void sync_bot(BotViewState& state, const Json& bot)
{
    if (bot == state.source || !bot.is_object()) {
        return;
    }
    if (!state.settings_dirty) {
        state.notifications = bool_field(bot, "notifications");
        state.allow_status = bool_field(bot, "allow_status");
        state.allow_config = bool_field(bot, "allow_config");
        const auto& rules = field(bot, "notification_rules");
        for (std::size_t i = 0; i < notification_options.size(); ++i) {
            const auto& rule = field(rules, notification_options[i].key);
            state.rules[i] = {bool_field(rule, "enabled"), bool_field(rule, "sound")};
        }
        const auto& permissions = field(bot, "permissions");
        for (std::size_t i = 0; i < permission_options.size(); ++i) {
            state.permissions[i] = bool_field(permissions, permission_options[i].key);
        }
    }
    if (!state.overrides_dirty) {
        const auto& overrides = field(bot, "overrides");
        fill(state.domain, string_field(overrides, "domain"));
        fill(state.sni, string_field(overrides, "sni"));
        fill(state.ws_host, string_field(overrides, "ws_host"));
        state.port = integer_field(overrides, "port");
    }
    state.source = bot;
}

void bot_setup_dialog(BotViewState& state, PreviewSession& session, const Json& bot)
{
    fit_preview_dialog(500);
    if (!theme::begin_modal("Set up a sample bot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!ImGui::IsPopupOpen("Set up a sample bot")) {
            session.abandon(state.connect_request);
        }
        return;
    }
    if (auto reply = session.take_reply(state.connect_request)) {
        state.setup = reply->ok() ? BotViewState::Setup::complete : BotViewState::Setup::token;
        if (reply->status == PreviewReply::Status::timed_out) {
            reply->message = "Preview did not confirm the bot connection. Try again.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = "The preview connection closed. Try again after reconnecting.";
        }
        state.connect_reply = std::move(reply);
    }
    switch (state.setup) {
    case BotViewState::Setup::intro:
        ImGui::SeparatorText("1. Bot setup");
        ImGui::TextWrapped("Connect a sample bot to preview notifications, user configuration and messages.");
        if (ImGui::Button("Next")) {
            state.setup = BotViewState::Setup::token;
        }
        break;
    case BotViewState::Setup::token:
        ImGui::SeparatorText("2. Demo token");
        edit("Sample token", state.token, "wave-preview-token");
        ImGui::TextWrapped("Use the supplied sample token or enter another demo value.");
        ImGui::BeginDisabled(std::string_view(state.token.data()).size() < 4);
        if (ImGui::Button("Connect sample bot")) {
            state.connect_request =
                session.submit({{"action", "bot_connect"}, {"token", state.token.data()}}, 5000);
            if (state.connect_request) {
                state.setup = BotViewState::Setup::connecting;
                state.connect_reply.reset();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Back")) {
            state.setup = BotViewState::Setup::intro;
        }
        if (state.connect_reply) {
            feedback(*state.connect_reply);
        }
        break;
    case BotViewState::Setup::connecting:
        ImGui::TextColored(rgba(Color::info), "Connecting the sample bot...");
        break;
    case BotViewState::Setup::complete:
        ImGui::PushStyleColor(ImGuiCol_Text, rgba(Color::success));
        ImGui::SeparatorText("3. Sample bot connected");
        ImGui::PopStyleColor();
        wrapped(string_field(bot, "admin_link"));
        if (ImGui::Button("Done")) {
            ImGui::CloseCurrentPopup();
        }
        break;
    }
    if (state.setup != BotViewState::Setup::complete) {
        if (state.setup != BotViewState::Setup::connecting) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Close")) {
            session.abandon(state.connect_request);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void bot_connection(BotViewState& state, PreviewSession& session, const Json& bot, bool narrow)
{
    const bool connected = bool_field(bot, "connected");
    if (connected) {
        const auto name = string_field(bot, "name", "Wave sample bot");
        ImGui::TextColored(rgba(Color::success), "Sample bot connected");
        wrapped(name);
    } else {
        ImGui::TextColored(rgba(Color::warning), "Sample bot disconnected");
    }
    if (ImGui::Button(connected ? "Open setup demo" : "Set up bot")) {
        session.abandon(state.connect_request);
        state.setup = BotViewState::Setup::intro;
        state.connect_reply.reset();
        fill(state.token, "wave-preview-token");
        ImGui::OpenPopup("Set up a sample bot");
    }
    if (connected) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Copy access link")) {
            const std::string link{string_field(bot, "admin_link")};
            ImGui::SetClipboardText(link.c_str());
            state.access_link_copied = !link.empty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Disconnect")) {
            ImGui::OpenPopup("Disconnect sample bot");
        }
    }
    if (state.access_link_copied && connected) {
        ImGui::TextColored(rgba(Color::success), "Sample access link copied");
    }
    feedback(session, {"bot_connect", "bot_disconnect"});
    bot_setup_dialog(state, session, bot);
    fit_preview_dialog(380);
    if (theme::begin_modal("Disconnect sample bot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Disconnect the bot from this preview process?");
        if (ImGui::Button("Confirm disconnect") && session.send({{"action", "bot_disconnect"}})) {
            state.access_link_copied = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void submit_bot_save(BotViewState::Save& save, PreviewSession& session, Json settings)
{
    save.request =
        session.submit({{"action", "apply_bot_settings"}, {"settings", std::move(settings)}}, 5000);
    if (save.request) {
        save.reply.reset();
    } else {
        save.reply =
            PreviewReply{PreviewReply::Status::disconnected, "The preview connection is unavailable."};
    }
}

bool finish_bot_save(BotViewState::Save& save, PreviewSession& session, bool& dirty)
{
    auto reply = session.take_reply(save.request);
    if (!reply) {
        return false;
    }
    if (reply->status == PreviewReply::Status::timed_out) {
        reply->message = "Preview did not confirm the update. Try again.";
    } else if (reply->status == PreviewReply::Status::disconnected) {
        reply->message = "The preview connection closed. Try again after reconnecting.";
    }
    const bool saved = reply->ok();
    save.reply = std::move(reply);
    if (saved) {
        dirty = false;
    }
    return saved;
}

void show_bot_save(const BotViewState::Save& save)
{
    if (save.request) {
        ImGui::TextColored(rgba(Color::info), "Saving sample bot settings...");
    } else if (save.reply) {
        feedback(*save.reply);
    }
}

void bot_settings(BotViewState& state, PreviewSession& session)
{
    ImGui::BeginDisabled(state.settings_save.request.has_value());
    state.settings_dirty |= ImGui::Checkbox("Admin notifications", &state.notifications);
    state.settings_dirty |= ImGui::Checkbox("Allow status requests", &state.allow_status);
    state.settings_dirty |= ImGui::Checkbox("Allow demo configuration requests", &state.allow_config);

    if (ImGui::CollapsingHeader("Notification rules", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("bot-notifications", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 54);
            ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 49);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < notification_options.size(); ++i) {
                ImGui::PushID(notification_options[i].key);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                wrapped(notification_options[i].label);
                ImGui::TableNextColumn();
                state.settings_dirty |= ImGui::Checkbox("##enabled", &state.rules[i].enabled);
                ImGui::TableNextColumn();
                state.settings_dirty |= ImGui::Checkbox("##sound", &state.rules[i].sound);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("User permissions")) {
        for (std::size_t i = 0; i < permission_options.size(); ++i) {
            state.settings_dirty |= ImGui::Checkbox(permission_options[i].label, &state.permissions[i]);
        }
    }

    ImGui::BeginDisabled(!state.settings_dirty);
    if (ImGui::Button("Save bot settings")) {
        Json rules = Json::object();
        for (std::size_t i = 0; i < notification_options.size(); ++i) {
            rules[notification_options[i].key] = {{"enabled", state.rules[i].enabled},
                                                  {"sound", state.rules[i].sound}};
        }
        Json permissions = Json::object();
        for (std::size_t i = 0; i < permission_options.size(); ++i) {
            permissions[permission_options[i].key] = state.permissions[i];
        }
        submit_bot_save(state.settings_save, session,
                        {{"notifications", state.notifications},
                         {"allow_status", state.allow_status},
                         {"allow_config", state.allow_config},
                         {"notification_rules", std::move(rules)},
                         {"permissions", std::move(permissions)}});
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    show_bot_save(state.settings_save);
}

void bot_overrides(BotViewState& state, PreviewSession& session)
{
    ImGui::BeginDisabled(state.overrides_save.request.has_value());
    ImGui::TextWrapped("Use sample overrides in the configuration that this preview bot shows.");
    state.overrides_dirty |= edit("Domain", state.domain, "preview.example");
    state.overrides_dirty |= edit("Server name (SNI)", state.sni);
    state.overrides_dirty |= edit("WebSocket host", state.ws_host);
    state.overrides_dirty |= edit_number("Port override", state.port);
    const bool valid = state.port >= 0 && state.port <= 65535;
    if (!valid) {
        wrapped("Enter 0 for the default port or a value from 1 to 65535.", Color::error);
    }
    ImGui::BeginDisabled(!state.overrides_dirty || !valid);
    if (ImGui::Button("Save overrides")) {
        submit_bot_save(state.overrides_save, session,
                        {{"overrides",
                          {{"domain", state.domain.data()},
                           {"sni", state.sni.data()},
                           {"ws_host", state.ws_host.data()},
                           {"port", state.port}}}});
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    show_bot_save(state.overrides_save);
}

void bot_actions(BotViewState& state, PreviewSession& session, const Json& bot)
{
    constexpr const char* audiences[]{"All preview users", "Enabled preview users", "Preview admins"};
    ImGui::TextUnformatted("Message audience");
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##bot-audience", &state.audience, audiences, IM_ARRAYSIZE(audiences));
    ImGui::TextUnformatted("Demo message");
    ImGui::InputTextMultiline("##bot-message", state.message.data(), state.message.size(), {-1, 76});
    ImGui::BeginDisabled(!state.message[0]);
    if (ImGui::Button("Message preview")) {
        ImGui::OpenPopup("Preview bot message");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Test status")) {
        session.send(
            {{"action", "bot_action"}, {"kind", "test status"}, {"audience", audiences[state.audience]}});
    }
    const auto message = string_field(bot, "last_message");
    if (!message.empty()) {
        ImGui::SeparatorText("Latest simulated message");
        wrapped(message);
    }
    feedback(session, {"bot_action"});

    fit_preview_dialog(460);
    if (theme::begin_modal("Preview bot message", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(audiences[state.audience]);
        ImGui::Separator();
        wrapped(state.message.data());
        if (ImGui::Button("Simulate send") && session.send({{"action", "bot_action"},
                                                            {"kind", "message"},
                                                            {"message", state.message.data()},
                                                            {"audience", audiences[state.audience]}})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

void render_bot(BotViewState& state, PreviewSession& session, bool narrow)
{
    const auto& bot = field(session.data(), "bot");
    const bool settings_saved = finish_bot_save(state.settings_save, session, state.settings_dirty);
    const bool overrides_saved = finish_bot_save(state.overrides_save, session, state.overrides_dirty);
    if (settings_saved || overrides_saved) {
        state.source = nullptr;
    }
    sync_bot(state, bot);
    if (ImGui::BeginTabBar("bot-views",
                           ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (ImGui::BeginTabItem("Connection")) {
            bot_connection(state, session, bot, narrow);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            bot_settings(state, session);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overrides")) {
            bot_overrides(state, session);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Actions")) {
            bot_actions(state, session, bot);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace wave
