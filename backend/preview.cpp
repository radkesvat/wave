#include "preview.h"

#include "auth.h"
#include "core_config_files.h"
#include "core_config_graph.h"
#include "core_config_name.h"
#include "core_config_users.h"
#include "core_settings.h"
#include "linux_metrics.h"
#include "panel_settings.h"
#include "panel_update.h"
#include "preview_backup.h"
#include "session_settings.h"
#include "waterwall.h"
#include "waterwall_logs.h"
#include "waterwall_schematic.h"
#include "waterwall_schematic_catalog.h"
#include "waterwall_schematic_file.h"
#include "waterwall_users.h"
#include "wave_version.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using UtcTime = std::chrono::sys_seconds;
using namespace std::chrono_literals;

constexpr auto host_sample_interval = 500ms;
constexpr auto core_update_check_interval = 24h;
constexpr std::size_t max_request_bytes = 8192;
constexpr std::size_t max_user_request_bytes = 32 * 1024;
constexpr auto max_frame_bytes = max_core_config_bytes + 1024;
constexpr std::size_t max_core_configs = 64;

bool core_in_use(std::string_view state)
{
    return state == "starting" || state == "running" || state == "stopping";
}

bool workspace_mutation(std::string_view action)
{
    constexpr std::string_view actions[] = {
        "install_core",        "start_core",
        "stop_core",           "restart_core",
        "update_core",         "check_core_updates",
        "check_panel_updates", "check_updates",
        "create_core_config",  "set_core_config_enabled",
        "remove_core_config",  "restore_preview_backup",
        "apply_settings",      "apply_tls_settings",
        "apply_core_settings", "apply_session_settings",
        "apply_theme",         "apply_donation",
        "save_core_config",    "create_config_user",
        "edit_config_user",    "delete_config_user",
    };
    return std::ranges::find(actions, action) != std::end(actions);
}

bool uses_core_schematic(std::string_view action)
{
    constexpr std::string_view actions[] = {
        "start_core",
        "restart_core",
        "install_core",
        "update_core",
        "apply_core_settings",
        "create_core_config",
        "get_core_config",
        "save_core_config",
        "get_config_users",
        "get_live_config_users",
        "get_config_user_password",
        "create_config_user",
        "edit_config_user",
        "delete_config_user",
    };
    return std::ranges::find(actions, action) != std::end(actions);
}

Json make_core()
{
    return {{"version", waterwall_version()},
            {"available_versions", Json::array({waterwall_version()})},
            {"checking_updates", false},
            {"capabilities", {{"lifecycle", true}, {"configs", true}}},
            {"configs", Json::array()}};
}

Json make_server()
{
    return {{"version", version},
            {"checking_updates", false},
            {"panel", {{"checking_updates", false}, {"update_check_generation", 0u}, {"update_available", false}}},
            {"core", make_core()}};
}

void sync_core_available_version(Json& core)
{
    const auto& available = core.at("available_versions");
    if (available.empty()) {
        core.erase("latest_version");
        core["update_available"] = false;
        return;
    }
    const auto latest = schematic_text(available.front());
    core["update_available"] = is_newer_waterwall_version(latest, waterwall_version());
    core["latest_version"] = latest;
}

Json make_settings()
{
    return {{"domain", "wave.example.invalid"},
            {"certificate_path", "preview/server.crt"},
            {"private_key_path", "preview/server.key"}};
}

Json make_data()
{
    return {{"logs", Json::array()}, {"server", make_server()}, {"settings", make_settings()}};
}

bool text_value(const Json& value, std::size_t limit, bool required = false)
{
    const auto* text = value.get_ptr<const std::string*>();
    return text && text->size() <= limit && text->find('\0') == std::string::npos &&
           (!required || text->find_first_not_of(" \t\r\n") != std::string::npos);
}

struct Donation {
    std::string answer;
    std::string answered_at;
    UtcTime time;
};

bool valid_donation_answer(std::string_view answer)
{
    return answer == "no" || answer == "below_5" || answer == "more_than_5";
}

UtcTime utc_now()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

std::optional<UtcTime> parse_utc_timestamp(std::string_view timestamp)
{
    if (timestamp.size() != 20 || timestamp[4] != '-' || timestamp[7] != '-' || timestamp[10] != 'T' ||
        timestamp[13] != ':' || timestamp[16] != ':' || timestamp[19] != 'Z') {
        return std::nullopt;
    }
    const auto decimal = [&](std::size_t first, std::size_t count) {
        int result = 0;
        for (const auto digit : timestamp.substr(first, count)) {
            if (digit < '0' || digit > '9') {
                return -1;
            }
            result = 10 * result + (digit - '0');
        }
        return result;
    };
    const int year = decimal(0, 4);
    const int month = decimal(5, 2);
    const int day = decimal(8, 2);
    const int hour = decimal(11, 2);
    const int minute = decimal(14, 2);
    const int second = decimal(17, 2);
    const auto date = std::chrono::year{year} / month / day;
    if (year <= 0 || !date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
           std::chrono::seconds{second};
}

std::optional<Donation> read_donation(const Json& saved)
{
    const auto response = saved.find("answer");
    const auto stamp = saved.find("answered_at");
    const auto* answer = response == saved.end() ? nullptr : response->get_ptr<const std::string*>();
    const auto* timestamp = stamp == saved.end() ? nullptr : stamp->get_ptr<const std::string*>();
    if (!answer || !valid_donation_answer(*answer) || !timestamp) {
        return std::nullopt;
    }
    const auto time = parse_utc_timestamp(*timestamp);
    if (!time || *time > utc_now()) {
        return std::nullopt;
    }
    return Donation{*answer, *timestamp, *time};
}

Json donation_settings(const std::optional<Donation>& donation)
{
    if (!donation) {
        return {{"reminder_due", true}};
    }
    const auto month = [](UtcTime time) {
        const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(time)};
        return date.year() / date.month();
    };
    return {{"answer", donation->answer},
            {"answered_at", donation->answered_at},
            {"reminder_due", month(donation->time) < month(utc_now())}};
}

bool integer_value(const Json& value, int minimum, int maximum)
{
    if (!value.is_number_integer()) {
        return false;
    }
    const double number = value.get<double>();
    return minimum <= number && number <= maximum;
}

template <typename Valid> bool update_field(const Json& input, Json& result, const char* key, Valid valid)
{
    if (const auto value = input.find(key); value != input.end()) {
        if (!valid(*value)) {
            return false;
        }
        result[key] = *value;
    }
    return true;
}

bool update_text(const Json& input, Json& result, const char* key, std::size_t limit, bool required = false)
{
    return update_field(input, result, key,
                        [&](const Json& value) { return text_value(value, limit, required); });
}

bool update_tls_settings(const Json& input, Json& result)
{
    return input.is_object() &&
           (input.contains("domain") || input.contains("certificate_path") ||
            input.contains("private_key_path")) &&
           update_text(input, result, "domain", 160, true) &&
           update_text(input, result, "certificate_path", 160) &&
           update_text(input, result, "private_key_path", 160);
}

Json panel_tls_settings(const Json& saved, const std::optional<TlsSettings>& active)
{
#ifdef WAVE_ENABLE_AUTH
    constexpr bool supported = true;
#else
    constexpr bool supported = false;
#endif
    auto string_field = [&](const char* key) {
        const auto field = saved.find(key);
        return field != saved.end() && field->is_string() ? field->get<std::string>() : std::string{};
    };
    const auto flag = saved.find("enabled");
    const bool enabled = flag != saved.end() && *flag == true;
    const auto domain = string_field("domain");
    const auto chain = string_field("certificate_chain");
    const auto key = string_field("private_key");
    const bool restart_required = enabled != active.has_value() ||
                                  (enabled && (domain != active->domain ||
                                               chain != active->certificate_chain.string() ||
                                               key != active->private_key.string()));
    return {{"enabled", enabled}, {"domain", domain}, {"certificate_chain", chain}, {"private_key", key},
            {"supported", supported}, {"restart_required", restart_required}};
}

struct Reply {
    bool ok;
    std::string feedback;
    bool changed = false;
    Json result = nullptr;
    bool feedback_in_logs_only = false;
};

struct CoreCatalogResult {
    std::vector<std::string> versions;
    std::string error;
};

struct CoreUpdate {
    std::shared_ptr<const WaterWallSchematic> schematic;
    std::unique_ptr<WaterWall> candidate;
};

class Preview {
  public:
    explicit Preview(std::shared_ptr<PanelDatabase> database)
        : database_(std::move(database)), config_files_(database_->directory()),
          config_users_(database_->directory()),
          core_(std::make_unique<WaterWall>(database_->directory(), waterwall_version())),
          core_logs_(database_->directory()),
          waterwall_bin_(waterwall_bin_directory(database_->directory()))
    {
        data_["settings"]["session_expiry_days"] = database_->session_expiry_days();
        const auto& stored = database_->initial_contents();
        data_["settings"]["tls"] = panel_tls_settings(stored.value("tls", Json::object()),
                                                       database_->settings().tls);
        const auto saved_theme = stored.find("theme");
        const auto* key = saved_theme == stored.end() ? nullptr : saved_theme->get_ptr<const std::string*>();
        const auto preset = key ? theme::parse_preset(*key).value_or(theme::Preset::standard)
                                : theme::Preset::standard;
        data_["settings"]["theme"] = theme::preset_key(preset);
        if (const auto donation = stored.find("donation"); donation != stored.end()) {
            donation_ = read_donation(*donation);
        }
        saved_core_settings_ = stored.value("core_settings", Json::object());
        reload_core_settings();
        load_core_configs(stored.value("core_configs", Json::array()));
        auto& core = data_["server"]["core"];
        core["available_versions"] = cached_waterwall_versions(waterwall_bin_);
        sync_core_available_version(core);
        if (database_->claim_waterwall_install()) {
            core_->install_if_missing(waterwall_schematic());
        }
        host_.sample(data_["server"]);
        sync_core();
        log("info", "Wave started.");
        sampler_ = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(mutex_);
            auto sampled_at = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                timer_.wait_until(lock, stop, sampled_at + host_sample_interval, [] { return false; });
                if (stop.stop_requested()) {
                    return;
                }
                sampled_at = std::chrono::steady_clock::now();
                host_.sample(data_["server"]);
                update_core();
                finish_panel_update_check();
                if (!panel_update_result_.valid() && !catalog_result_.valid() && !queued_catalog_check_) {
                    data_["server"]["checking_updates"] = false;
                }
                ++revision_;
            }
        });
    }

    std::string overview()
    {
        std::lock_guard lock(mutex_);
        const auto& server = data_["server"];
        const auto& downloads = server["download_history"];
        const auto count = std::min<std::size_t>(13, downloads.size());
        return Json{{"cpu_percent", server["cpu_percent"]},
                    {"memory_used_gib", server["memory_used_gib"]},
                    {"memory_total_gib", server["memory_total_gib"]},
                    {"uptime_minutes", server["uptime_minutes"]},
                    {"download_mib_per_second", Json(downloads.end() - count, downloads.end())}}
            .dump();
    }

    void serve(httplib::ws::WebSocket& socket, const std::function<bool()>& session)
    {
        // Only this handler writes to its socket. A short read timeout lets it
        // publish shared state changes while the current browser is idle.
        socket.set_read_timeout(100ms);
        std::uint64_t sent_revision = 0;
        std::uint64_t sent_schematic = 0;
        if (!session() || !socket.send(changed_snapshot(sent_revision, sent_schematic))) {
            return;
        }
        std::string message;
        while (const auto result = socket.read(message)) {
            if (!session()) {
                socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                return;
            }
            const auto frame = result == httplib::ws::Timeout
                                   ? changed_snapshot(sent_revision, sent_schematic)
                                   : handle_message(result == httplib::ws::Text ? std::string_view(message)
                                                                                : std::string_view{},
                                                    sent_revision, sent_schematic);
            if (!frame.empty()) {
                if (!session()) {
                    socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                    return;
                }
                if (!socket.send(frame)) {
                    return;
                }
            }
        }
    }

  private:
    void reload_core_settings()
    {
        auto normalized = core_settings::normalize(saved_core_settings_);
        data_.erase("core_settings_notice");
        if (!normalized.issue.empty()) {
            data_["core_settings_notice"] =
                "Review the settings shown here before saving. Saving replaces the previous core settings. " +
                normalized.issue;
        }
        data_["core_settings"] = std::move(normalized.settings);
    }

    void load_core_configs(const Json& saved_configs)
    {
        if (!saved_configs.is_array() || saved_configs.size() > max_core_configs) {
            throw std::runtime_error("core_configs in wave-db.json must be an array of up to 64 configs.");
        }
        auto& configs = data_["server"]["core"]["configs"];
        for (const auto& row : saved_configs) {
            const auto name = row.find("name");
            const auto id = row.find("id");
            const auto enabled = row.find("enabled");
            const auto* text = name == row.end() ? nullptr : name->get_ptr<const std::string*>();
            if (!text || text->empty() || core_config_name(*text) != *text || id == row.end() ||
                !integer_value(*id, 1, std::numeric_limits<int>::max() - 1) || enabled == row.end() ||
                !enabled->is_boolean() || std::ranges::any_of(configs, [&](const Json& config) {
                    return config["id"] == *id || config["name"] == *name;
                })) {
                throw std::runtime_error(
                    "core_configs in wave-db.json contains an invalid or duplicate config.");
            }
            configs.push_back({{"id", *id}, {"name", *name}, {"enabled", *enabled}, {"active", false}});
            const auto file = config_files_.read(*text);
            update_config_tabs(configs.back(), file.config);
            next_config_id_ = std::max(next_config_id_, id->get<int>() + 1);
        }
    }

    bool store_configs(const Json& configs)
    {
        auto stored = configs;
        for (auto& config : stored) {
            config.erase("active");
            config.erase("auth_nodes");
            config.erase("live_users");
        }
        try {
            database_->save_core_configs(stored);
        } catch (const std::runtime_error&) {
            return false;
        }
        return true;
    }

    Json snapshot() const
    {
        Json snapshot = {{"type", "snapshot"}, {"data", data_}};
        snapshot["data"]["settings"]["donation"] = donation_settings(donation_);
        return snapshot;
    }

    void attach_schematic(Json& frame, std::uint64_t& sent_schematic) const
    {
        frame["schematic_revision"] = schematic_revision_;
        if (sent_schematic != schematic_revision_) {
            frame["schematic"] = waterwall_schematic();
            sent_schematic = schematic_revision_;
        }
    }

    std::string changed_snapshot(std::uint64_t& sent_revision, std::uint64_t& sent_schematic)
    {
        std::lock_guard lock(mutex_);
        if (revision_ == sent_revision) {
            return {};
        }
        auto frame = snapshot();
        attach_schematic(frame, sent_schematic);
        sent_revision = revision_;
        return frame.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    std::string handle_message(std::string_view message, std::uint64_t& sent_revision,
                               std::uint64_t& sent_schematic)
    {
        const bool invalid_frame =
            message.size() > max_frame_bytes || message.find('\0') != std::string_view::npos;
        bool too_deep = false;
        auto request = invalid_frame ? Json{}
                                     : Json::parse(
                                           message,
                                           [&too_deep](int depth, auto, auto&) {
                                               too_deep |= depth > max_core_config_depth + 1;
                                               return !too_deep;
                                           },
                                           false);
        if (too_deep) {
            request = nullptr;
        }
        const auto action_value = request.find("action");
        const std::string_view action = action_value != request.end() && text_value(*action_value, 48, true)
                                            ? action_value->get_ref<const std::string&>()
                                            : std::string_view{};
        const auto bytes = message.size();
        const bool too_large =
            bytes > max_request_bytes &&
            !(action == "save_core_config" ||
              (action == "apply_core_settings" && bytes <= core_settings::max_bytes + 256) ||
              (action == "restore_preview_backup" && bytes <= max_preview_restore_request_bytes) ||
              ((action == "create_config_user" || action == "edit_config_user") &&
               bytes <= max_user_request_bytes));

        std::unique_lock lock(mutex_);
        const auto revision = request.find("schematic_revision");
        const bool allow_missing_revision =
            schematic_revision_ == 1 && waterwall_version() == bundled_waterwall_version;
        const bool stale = uses_core_schematic(action) &&
                           (revision == request.end()
                                ? !allow_missing_revision
                                : !revision->is_number_unsigned() || *revision != schematic_revision_);
        auto reply = action.empty() ? Reply{false, "Send a JSON panel action."}
                     : too_large    ? Reply{false, "That panel request is too large."}
                     : stale        ? Reply{false, "WaterWall changed version. Reload the form and retry."}
                     : action == "get_live_config_users" ? get_live_config_users(request, lock)
                                                         : dispatch(action, request);
        if (!reply.ok && !reply.feedback.empty() && workspace_mutation(action)) {
            log("error", reply.feedback);
            reply.changed = true;
            reply.feedback_in_logs_only = true;
        }
        sync_core();
        if (reply.changed) {
            ++revision_;
        }
        auto frame = snapshot();
        attach_schematic(frame, sent_schematic);
        frame["action"] = action;
        frame["ok"] = reply.ok;
        frame["feedback"] = reply.feedback;
        if (reply.feedback_in_logs_only) {
            frame["feedback_in_logs_only"] = true;
        }
        if (!reply.result.is_null()) {
            frame["result"] = std::move(reply.result);
        }
        if (const auto id = request.find("request_id");
            id != request.end() && (id->is_number_integer() || text_value(*id, 80))) {
            frame["request_id"] = *id;
        }
        sent_revision = revision_;
        return frame.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    void add_log(std::string_view source, std::string_view timestamp, std::string_view level,
                 std::string_view message)
    {
        auto& logs = data_["logs"];
        const auto same_source = [&](const Json& row) { return row["source"] == source; };
        if (std::ranges::count_if(logs, same_source) == log_history_per_source) {
            logs.erase(std::ranges::find_if(logs, same_source));
        }
        logs.push_back({{"source", source}, {"ts", timestamp}, {"level", level}, {"message", message}});
    }

    void log(std::string_view level, const std::string& message)
    {
        const auto now = std::time(nullptr);
        std::tm date{};
        localtime_r(&now, &date);
        char timestamp[9];
        const auto length = std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &date);
        add_log("panel", {timestamp, length}, level, message);
    }

    Reply complete(std::string feedback, std::string_view level = "success")
    {
        log(level, feedback);
        return {.ok = true,
                .feedback = std::move(feedback),
                .changed = true,
                .feedback_in_logs_only = level == "success" || level == "info"};
    }

    void update_core()
    {
        core_->update();
        for (const auto& entry : core_logs_.read()) {
            add_log(entry.source, entry.timestamp.substr(11, 8), entry.level, entry.message);
        }
        if (restart_pending_ && !core_in_use(core_->state())) {
            restart_pending_ = false;
            const auto reply = start_core();
            if (!reply.ok) {
                restart_error_ = "Could not restart WaterWall. " + reply.feedback;
            }
        }
        sync_core();
        update_release_installation();
        update_catalog();
    }

    void sync_core()
    {
        auto& core = data_["server"]["core"];
        const auto state = core_->state();
        if (!core_in_use(state)) {
            core_logs_.stop();
            live_configs_.clear();
            for (auto& config : core["configs"]) {
                config["active"] = false;
            }
        }
        const auto& error = core_error();
        if (error.empty()) {
            if (const auto previous = core.find("state"); previous != core.end()) {
                if (*previous == "installing" && state == "stopped") {
                    log("success", "WaterWall installed.");
                    next_catalog_check_ = {};
                } else if (*previous == "starting" && state == "running") {
                    log("success", "WaterWall started.");
                } else if (*previous == "stopping" && state == "stopped") {
                    log("success", "WaterWall stopped.");
                }
            }
            core.erase("error");
        } else if (!core.contains("error") || core["error"] != error) {
            core["error"] = error;
            log("error", error);
        }
        core["state"] = state;
    }

    const std::string& core_error() const
    {
        return restart_error_.empty() ? core_->error() : restart_error_;
    }

    Reply apply_settings(const Json& input)
    {
        const auto values = input.find("settings");
        auto settings = data_["settings"];
        if (values == input.end() || !update_tls_settings(*values, settings)) {
            return {false, "Enter a valid preview domain or PEM path."};
        }
        data_["settings"] = std::move(settings);
        return complete("Applied sample TLS settings.");
    }

    Reply apply_tls_settings(const Json& input)
    {
        const auto values = input.find("settings");
        if (values == input.end() || !values->is_object()) {
            return {false, "Choose whether to enable panel HTTPS."};
        }
        const auto enabled = values->find("enabled");
        if (enabled == values->end() || !enabled->is_boolean()) {
            return {false, "Choose whether to enable panel HTTPS."};
        }

        std::optional<TlsSettings> selected;
        if (*enabled == true) {
            const auto domain = values->find("domain");
            const auto chain = values->find("certificate_chain");
            const auto key = values->find("private_key");
            if (domain == values->end() || !text_value(*domain, max_request_bytes, true) ||
                chain == values->end() || !text_value(*chain, max_request_bytes, true) ||
                key == values->end() || !text_value(*key, max_request_bytes, true)) {
                return {false, "Enter the HTTPS hostname, certificate chain path, and private key path."};
            }
            selected = TlsSettings{domain->get<std::string>(), chain->get<std::string>(), key->get<std::string>()};
        }

        try {
            const auto saved = database_->save_tls_settings(selected);
            data_["settings"]["tls"] = panel_tls_settings(saved, database_->settings().tls);
        } catch (const std::runtime_error& error) {
            return {false, error.what()};
        }
        const bool restart = data_["settings"]["tls"]["restart_required"] == true;
        return complete(restart ? "Saved panel TLS settings. Restart Wave to apply them."
                                : "Saved panel TLS settings.");
    }

    Reply apply_session_settings(const Json& input)
    {
        const auto days = input.find("session_expiry_days");
        if (days == input.end() || !integer_value(*days, 1, max_session_expiry_days)) {
            return {false, "Enter a whole number from 1 to " + std::to_string(max_session_expiry_days) +
                               " for session expiry."};
        }
        const auto selected = days->get<int>();
        try {
            database_->save_session_expiry_days(selected);
        } catch (const std::runtime_error&) {
            return {false, "Could not save session expiry in wave-db.json."};
        }
        data_["settings"]["session_expiry_days"] = selected;
        return complete("Saved session expiry for future sign-ins.");
    }

    Reply apply_theme(const Json& input)
    {
        const auto value = input.find("theme");
        const auto* key = value == input.end() ? nullptr : value->get_ptr<const std::string*>();
        const auto preset = key ? theme::parse_preset(*key) : std::nullopt;
        if (!preset) {
            return {false, "Choose a valid panel theme."};
        }
        auto& current = data_["settings"]["theme"];
        if (current == *key) {
            return {true, "The panel theme is already selected."};
        }
        try {
            database_->save_theme(*preset);
        } catch (const std::runtime_error&) {
            return {false, "Could not save the panel theme in wave-db.json."};
        }
        current = *key;
        return complete("Saved panel theme.");
    }

    Reply apply_donation(const Json& input)
    {
        const auto value = input.find("answer");
        const auto* answer = value == input.end() ? nullptr : value->get_ptr<const std::string*>();
        if (!answer || !valid_donation_answer(*answer)) {
            return {false, "Choose a donation response."};
        }
        const auto now = utc_now();
        const auto seconds = std::chrono::system_clock::to_time_t(now);
        std::tm date{};
        char timestamp[21];
        if (!gmtime_r(&seconds, &date) ||
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &date) != 20) {
            return {false, "Could not read the UTC date for the donation response."};
        }
        try {
            database_->save_donation(*answer, timestamp);
        } catch (const std::runtime_error&) {
            return {false, "Could not save the donation response in wave-db.json."};
        }
        donation_ = Donation{*answer, timestamp, now};
        return complete("Saved donation response.");
    }

    Reply apply_core_settings(const Json& input)
    {
        const auto settings = input.find("settings");
        if (settings == input.end()) {
            return {false, "Send a core settings object."};
        }
        auto normalized = core_settings::normalize(*settings);
        if (!normalized.issue.empty()) {
            return {false, std::move(normalized.issue)};
        }
        try {
            database_->save_core_settings(normalized.settings);
        } catch (const std::runtime_error&) {
            return {false, "Could not save core settings in wave-db.json."};
        }
        saved_core_settings_ = normalized.settings;
        data_["core_settings"] = std::move(normalized.settings);
        data_.erase("core_settings_notice");
        return complete("Saved core settings for the next launch.");
    }

    Reply restore_backup(const Json& input)
    {
        const auto invalid = Reply{false, "Choose a valid Wave preview backup."};
        const auto backup = input.find("backup");
        if (backup == input.end() || !backup->is_object() || !backup->contains("format") ||
            (*backup)["format"] != preview_backup_format || !backup->contains("version") ||
            !(*backup)["version"].is_number_integer() || (*backup)["version"] != preview_backup_version) {
            return invalid;
        }
        const auto tls = backup->find("settings");
        const auto saved_configs = backup->find("configs");
        auto settings = data_["settings"];
        if (tls == backup->end() ||
            !std::ranges::all_of(preview_backup_tls_fields,
                                 [&](const char* field) { return tls->contains(field); }) ||
            !update_tls_settings(*tls, settings) || saved_configs == backup->end() ||
            !saved_configs->is_array() || saved_configs->size() > max_core_configs) {
            return invalid;
        }

        auto& current = data_["server"]["core"]["configs"];
        auto configs = Json::array();
        for (const auto& config : current) {
            if (config["active"] == true) {
                configs.push_back(config);
            }
        }
        if (configs.size() + saved_configs->size() > max_core_configs) {
            return invalid;
        }
        int next_id = next_config_id_;
        for (const auto& config : *saved_configs) {
            const auto raw_name = config.find("name");
            const auto* text = raw_name == config.end() ? nullptr : raw_name->get_ptr<const std::string*>();
            const auto name = text ? core_config_name(*text) : std::string_view{};
            const auto enabled = config.find("enabled");
            if (name.empty() || enabled == config.end() || !enabled->is_boolean() ||
                std::ranges::any_of(configs, [&](const Json& item) { return item["name"] == name; })) {
                return invalid;
            }
            const auto existing =
                std::ranges::find_if(current, [&](const Json& item) { return item["name"] == name; });
            if (existing == current.end() && next_id == std::numeric_limits<int>::max()) {
                return {false, "The config list is full."};
            }
            const int id = existing == current.end() ? next_id++ : (*existing)["id"].get<int>();
            configs.push_back({{"id", id}, {"name", name}, {"enabled", *enabled}, {"active", false}});
        }

        std::vector<std::string_view> created;
        std::string_view error;
        for (auto& config : configs) {
            if (config["active"] == true) {
                continue;
            }
            const auto& name = config["name"].get_ref<const std::string&>();
            bool new_file;
            if (!config_files_.create(name, true, new_file, make_empty_core_config(name))) {
                error = "Could not prepare the files for the restored configs.";
                break;
            }
            if (new_file) {
                created.push_back(name);
            }
            const auto file = config_files_.read(name);
            update_config_tabs(config, file.config);
        }
        if (error.empty() && !store_configs(configs)) {
            error = "Could not save restored configs in wave-db.json.";
        }
        if (!error.empty()) {
            for (const auto name : created) {
                config_files_.remove(name);
            }
            return {false, std::string(error)};
        }
        data_["settings"] = std::move(settings);
        current = std::move(configs);
        next_config_id_ = next_id;
        return complete("Restored sample TLS settings and the inactive config list.");
    }

    Reply change_core_state(std::string_view action)
    {
        if (core_update_) {
            return {false, "Wait for the WaterWall update to finish."};
        }
        if (action == "install_core") {
            if (catalog_result_.valid()) {
                return {false, "Wait for the core update check to finish."};
            }
            auto error = core_->install(waterwall_schematic());
            if (!error.empty()) {
                return {false, std::move(error)};
            }
            restart_error_.clear();
            return complete("Installing WaterWall.", "info");
        }
        if (action == "start_core") {
            return start_core();
        }
        const bool restart = action == "restart_core";
        if (restart) {
            if (core_->state() != "running") {
                return {false, "WaterWall must be running before it can be restarted."};
            }
            if (!core_->installed()) {
                return {false, "The WaterWall executable must be installed before it can be restarted."};
            }
        }
        if (auto error = core_->stop(); !error.empty()) {
            return {false, std::move(error)};
        }
        restart_pending_ = restart;
        return complete(restart ? "Restarting WaterWall." : "Stopping WaterWall.", "info");
    }

    Reply start_core()
    {
        if (core_->state() != "stopped") {
            return {false, "WaterWall must be installed and stopped before it can be started."};
        }
        if (data_.contains("core_settings_notice")) {
            return {false, "Review and save the core settings in Settings → Core before starting WaterWall."};
        }
        auto paths = Json::array();
        std::map<int, Json> live_configs;
        auto& configs = data_["server"]["core"]["configs"];
        for (const auto& config : configs) {
            if (config["enabled"] == true) {
                const auto& name = config["name"].get_ref<const std::string&>();
                if (!config_files_.available(name)) {
                    return {false, "Configuration file is missing or unsafe for " + name + '.'};
                }
                auto file = config_files_.read(name);
                if (const auto nodes = file.config.find("nodes");
                    nodes != file.config.end() && nodes->is_array()) {
                    for (const auto& node : *nodes) {
                        if (uses_auth_database(node)) {
                            const auto db = auth_database_name(node, name, database_->directory());
                            bool created;
                            if (!db.empty()) {
                                if (auto error = config_users_.prepare(name, db, created); !error.empty()) {
                                    return {false, name + ": " + std::move(error)};
                                }
                            }
                        }
                    }
                }
                paths.push_back(CoreConfigFiles::relative_path(name).generic_string());
                live_configs.emplace(config["id"].get<int>(), std::move(file.config));
            }
        }
        if (paths.empty()) {
            return {false, "Enable at least one config before starting WaterWall."};
        }
        auto settings = data_["core_settings"];
        settings[schematic_text(waterwall_schematic().at("core_settings").at("config_list_key"))] =
            std::move(paths);
        if (auto error = core_->start(settings.dump()); !error.empty()) {
            return {false, std::move(error)};
        }
        restart_error_.clear();
        core_logs_.start(settings);
        live_configs_ = std::move(live_configs);
        ++live_run_;
        for (auto& config : configs) {
            config["active"] = config["enabled"];
        }
        return complete("Starting WaterWall.", "info");
    }

    bool installing_core() const
    {
        return core_update_.has_value() || core_->state() == "installing";
    }

    std::string check_catalog(bool manual)
    {
        std::packaged_task<CoreCatalogResult(std::stop_token)> task(
            [directory = waterwall_bin_, selected = std::string(waterwall_version()),
             packages = waterwall_schematic().at("packages")](std::stop_token stop) {
                try {
                    auto error = fetch_waterwall_schematics(directory, selected, packages, stop);
                    return CoreCatalogResult{cached_waterwall_versions(directory), std::move(error)};
                } catch (const std::exception&) {
                    return CoreCatalogResult{{}, "Could not read the WaterWall schematic catalog."};
                }
            });
        next_catalog_check_ = std::chrono::steady_clock::now() + core_update_check_interval;
        auto result = task.get_future();
        try {
            catalog_worker_ = std::jthread(std::move(task));
        } catch (const std::system_error&) {
            return "Could not start the WaterWall update check.";
        }
        catalog_result_ = std::move(result);
        manual_catalog_check_ = manual;
        auto& status = data_["server"]["core"];
        status["checking_updates"] = true;
        status.erase("update_check_error");
        return {};
    }

    Reply check_core_updates(bool queue_during_install = false)
    {
        if (data_["server"]["checking_updates"] == true) {
            return {true, "Wave is already checking for updates."};
        }
        if (catalog_result_.valid()) {
            manual_catalog_check_ = true;
            return {true, "Wave is already checking for core updates."};
        }
        if (installing_core()) {
            if (!queue_during_install) {
                return {false, "Wait for WaterWall installation to finish before checking for updates."};
            }
            queued_catalog_check_ = true;
            auto& status = data_["server"]["core"];
            status["checking_updates"] = true;
            status.erase("update_check_error");
            return {true, "WaterWall update check queued until installation finishes.", true};
        }
        auto error = check_catalog(true);
        return error.empty() ? Reply{true, "Checking for core updates.", true}
                             : Reply{false, std::move(error)};
    }

    void update_catalog()
    {
        auto& status = data_["server"]["core"];
        if (catalog_result_.valid() && catalog_result_.wait_for(0s) == std::future_status::ready) {
            auto result = catalog_result_.get();
            status["checking_updates"] = false;
            if (!result.versions.empty()) {
                status["available_versions"] = std::move(result.versions);
                sync_core_available_version(status);
            }
            if (!result.error.empty()) {
                status["update_check_error"] = result.error;
                log("error", result.error);
            } else if (manual_catalog_check_) {
                log("success", "WaterWall update check completed.");
            }
        }
        if (!catalog_result_.valid() && !installing_core() &&
            (queued_catalog_check_ || std::chrono::steady_clock::now() >= next_catalog_check_)) {
            const bool manual = std::exchange(queued_catalog_check_, false);
            if (auto error = check_catalog(manual); !error.empty()) {
                status["checking_updates"] = false;
                status["update_check_error"] = error;
                log("error", error);
            }
        }
    }

    Reply check_panel_updates()
    {
        if (panel_update_result_.valid() || data_["server"]["checking_updates"] == true) {
            return {true, "Wave is already checking for panel updates."};
        }
        std::packaged_task<PanelUpdateCheck(std::stop_token)> task([](std::stop_token stop) {
            try {
                return check_panel_update(release_version, stop);
            } catch (const std::exception&) {
                return PanelUpdateCheck{.error = "Could not check for Wave panel updates."};
            }
        });
        auto result = task.get_future();
        try {
            panel_update_worker_ = std::jthread(std::move(task));
        } catch (const std::system_error&) {
            return {false, "Could not start the Wave panel update check."};
        }
        panel_update_result_ = std::move(result);
        auto& status = data_["server"]["panel"];
        status["checking_updates"] = true;
        status.erase("update_check_error");
        return {true, "Checking for panel updates.", true};
    }

    Reply check_updates()
    {
        auto& checking = data_["server"]["checking_updates"];
        if (checking == true) {
            return {true, "Wave is already checking for updates."};
        }
        const auto core = check_core_updates(true);
        auto panel = check_panel_updates();
        checking = core.ok || panel.ok;
        if (!core.ok) {
            data_["server"]["core"]["update_check_error"] = core.feedback;
            log("error", core.feedback);
        }
        return panel.ok ? Reply{true, "Checking for updates.", true} : std::move(panel);
    }

    void finish_panel_update_check()
    {
        if (!panel_update_result_.valid() || panel_update_result_.wait_for(0s) != std::future_status::ready) {
            return;
        }
        const auto result = panel_update_result_.get();
        auto& status = data_["server"]["panel"];
        status["checking_updates"] = false;
        status["update_check_generation"] = status["update_check_generation"].get<std::uint64_t>() + 1;
        if (!result.error.empty()) {
            status["update_check_error"] = result.error;
            log("error", result.error);
            return;
        }
        status["update_available"] = result.update_available;
        if (result.latest_version.empty()) {
            status.erase("latest_version");
            log("info", "Wave has no version tags on GitHub yet.");
            return;
        }
        status["latest_version"] = result.latest_version;
        if (result.update_available) {
            log("info", "Wave " + result.latest_version + " is available.");
        } else {
            log("info", "No newer Wave panel version is available.");
        }
    }

    Reply update_core_release(const Json& input)
    {
        if (core_in_use(core_->state()) || restart_pending_) {
            return {false, "Stop WaterWall before changing its version."};
        }
        if (installing_core() || catalog_result_.valid()) {
            return {false, "Wait for the current WaterWall operation to finish."};
        }
        const auto target = schematic_text(schematic_field(input, "version"));
        if (!valid_waterwall_version(target)) {
            return {false, "Choose an available WaterWall version."};
        }
        if (target == waterwall_version()) {
            return {false, "That WaterWall version is already active."};
        }
        auto& status = data_["server"]["core"];
        const auto& available = status["available_versions"];
        if (std::ranges::find(available, target) == available.end()) {
            return {false, "That WaterWall version is unavailable."};
        }
        status.erase("update_error");
        try {
            auto schematic = prepare_waterwall_release_schematic(waterwall_bin_, target);
            auto candidate = std::make_unique<WaterWall>(database_->directory(), target);
            if (candidate->installed()) {
                if (auto error = activate_release({std::move(schematic), std::move(candidate)});
                    !error.empty()) {
                    return {false, std::move(error)};
                }
                return {true, "Selected WaterWall " + std::string(target) + '.', true, {}, true};
            }
            if (auto error = candidate->install(waterwall_schematic(*schematic)); !error.empty()) {
                return {false, std::move(error)};
            }
            core_update_ = CoreUpdate{std::move(schematic), std::move(candidate)};
            status["update_target"] = target;
            return complete("Installing WaterWall " + std::string(target) + '.', "info");
        } catch (const std::exception& error) {
            return {false, error.what()};
        }
    }

    std::string activate_release(CoreUpdate next)
    {
        const auto target = schematic_text(waterwall_schematic(*next.schematic).at("version"));
        try {
            database_->save_waterwall_version(target);
        } catch (const std::runtime_error&) {
            return "Could not save the selected WaterWall version in wave-db.json.";
        }
        activate_waterwall_schematic(std::move(next.schematic));
        core_ = std::move(next.candidate);
        core_logs_.stop();
        restart_error_.clear();
        ++schematic_revision_;
        ++live_run_;
        reload_core_settings();
        auto& status = data_["server"]["core"];
        status["version"] = waterwall_version();
        sync_core_available_version(status);
        for (auto& config : status["configs"]) {
            const auto& name = config["name"].get_ref<const std::string&>();
            update_config_tabs(config, config_files_.read(name).config);
        }
        log("success", "Selected WaterWall " + std::string(waterwall_version()) + '.');
        next_catalog_check_ = {};
        sync_core();
        return {};
    }

    void update_release_installation()
    {
        if (!core_update_) {
            return;
        }
        core_update_->candidate->update();
        if (core_update_->candidate->state() == "installing") {
            return;
        }
        auto next = std::move(*core_update_);
        core_update_.reset();
        auto& status = data_["server"]["core"];
        status.erase("update_target");
        std::string error;
        if (next.candidate->installed()) {
            error = activate_release(std::move(next));
        } else {
            error = next.candidate->error();
            if (error.empty()) {
                error = "The target binary is missing or unsafe.";
            }
        }
        if (!error.empty()) {
            status["update_error"] = "Could not update WaterWall. " + error;
            log("error", status["update_error"].get_ref<const std::string&>());
        }
    }

    std::string prepare_auth_databases(std::string_view name, const Json& config,
                                       std::vector<std::string>& created)
    {
        for (const auto& node : config["nodes"]) {
            if (!uses_auth_database(node)) {
                continue;
            }
            const auto& db = node["panel-settings"]["auth-db"].get_ref<const std::string&>();
            bool new_file;
            auto error = config_users_.prepare(name, db, new_file);
            if (new_file) {
                created.push_back(db);
            }
            if (!error.empty()) {
                return error;
            }
        }
        return {};
    }

    void remove_created_auth_databases(std::string_view name, const std::vector<std::string>& created)
    {
        for (const auto& db : created) {
            config_users_.remove(name, db);
        }
    }

    Reply create_core_config(const Json& input)
    {
        const auto value = input.find("name");
        const auto* text = value == input.end() ? nullptr : value->get_ptr<const std::string*>();
        const auto name = text ? core_config_name(*text) : std::string_view{};
        if (name.empty()) {
            return {false, "Enter a valid config filename of up to 64 bytes."};
        }
        const auto add_auth_nodes = input.find("add_auth_nodes");
        if (add_auth_nodes != input.end() && !add_auth_nodes->is_boolean()) {
            return {false, "Choose whether to add API and authentication database nodes."};
        }

        auto& configs = data_["server"]["core"]["configs"];
        if (configs.size() >= max_core_configs || next_config_id_ == std::numeric_limits<int>::max()) {
            return {false, "The config list is full."};
        }
        if (std::ranges::any_of(configs, [&](const Json& config) { return config["name"] == name; })) {
            return {false, "A config already uses that name."};
        }
        auto changed = configs;
        changed.push_back({{"id", next_config_id_}, {"name", name}, {"enabled", false}, {"active", false}});
        Json config;
        if (add_auth_nodes != input.end() && *add_auth_nodes == true) {
            if (!schematic_field(waterwall_schematic().at("config"), "users_starter").is_object()) {
                return {false, "This WaterWall schematic does not define user starter nodes."};
            }
            std::vector<Json> existing;
            existing.reserve(configs.size() + live_configs_.size());
            for (const auto& row : configs) {
                auto file = config_files_.read(row["name"].get_ref<const std::string&>());
                if (file.error.empty()) {
                    existing.push_back(std::move(file.config));
                }
            }
            for (const auto& [id, running] : live_configs_) {
                existing.push_back(running);
            }
            const auto port = available_waterwall_api_port(existing);
            if (!port) {
                return {false, "Could not find an available loopback TCP port in the range 49152–65535."};
            }
            config = make_users_starter_config(name, *port);
            if (config.is_null()) {
                return {false, "Could not generate credentials for the starter nodes."};
            }
            if (auto error = validate_config_graph(config, {}); !error.empty()) {
                return {false, std::move(error)};
            }
            if (auto error = configure_auth_databases(config, {}, name, database_->directory());
                !error.empty()) {
                return {false, std::move(error)};
            }
            update_config_tabs(changed.back(), config);
        } else {
            config = make_empty_core_config(name);
        }
        bool created;
        if (!config_files_.create(name, false, created, config)) {
            return {false, "Could not create the config folder and file."};
        }
        std::vector<std::string> created_databases;
        if (auto error = prepare_auth_databases(name, config, created_databases); !error.empty()) {
            remove_created_auth_databases(name, created_databases);
            config_files_.remove(name);
            return {false, std::move(error)};
        }
        if (!store_configs(changed)) {
            remove_created_auth_databases(name, created_databases);
            config_files_.remove(name);
            return {false, "Could not save the new config in wave-db.json."};
        }
        configs = std::move(changed);
        ++next_config_id_;
        return complete("Created config " + std::string(name) + " (disabled).");
    }

    Reply change_core_config(std::string_view action, const Json& input)
    {
        const auto id = input.find("id");
        if (id == input.end() || !integer_value(*id, 1, std::numeric_limits<int>::max())) {
            return {false, "Choose a config."};
        }
        auto& configs = data_["server"]["core"]["configs"];
        auto changed = configs;
        const auto config =
            std::ranges::find_if(changed, [&](const Json& item) { return item["id"] == *id; });
        if (config == changed.end()) {
            return {false, "That config is unavailable."};
        }
        if ((*config)["active"] == true) {
            return {false, "The running core is using this config. Stop it before making changes."};
        }

        const auto name = (*config)["name"].get<std::string>();
        if (action == "remove_core_config") {
            changed.erase(config);
            if (!store_configs(changed)) {
                return {false, "Could not update configs in wave-db.json."};
            }
            configs = std::move(changed);
            if (!config_files_.remove(name)) {
                return complete("Unregistered config " + name + ". Its file could not be removed.",
                                "warning");
            }
            return complete("Removed config " + name + '.');
        }
        const auto enabled = input.find("enabled");
        if (enabled == input.end() || !enabled->is_boolean()) {
            return {false, "Choose whether to enable the config."};
        }
        if ((*config)["enabled"] == *enabled) {
            return {true, "The config already has that setting."};
        }
        (*config)["enabled"] = *enabled;
        if (!store_configs(changed)) {
            return {false, "Could not save config changes in wave-db.json."};
        }
        configs = std::move(changed);
        return complete(std::string(*enabled == true ? "Enabled" : "Disabled") + " config " + name + '.');
    }

    static bool update_config_tabs(Json& row, const Json& config)
    {
        const bool live = has_waterwall_users_api(config);
        bool changed = row.contains("live_users") != live;
        if (live) {
            row["live_users"] = true;
        } else {
            row.erase("live_users");
        }
        auto names = config_auth_nodes(config);
        if (names.empty()) {
            return row.erase("auth_nodes") != 0 || changed;
        }
        if (!row.contains("auth_nodes") || row["auth_nodes"] != names) {
            row["auth_nodes"] = std::move(names);
            changed = true;
        }
        return changed;
    }

    Reply get_live_config_users(const Json& input, std::unique_lock<std::mutex>& lock)
    {
        const auto id = input.find("config_id");
        const auto target = input.find("node");
        if (id == input.end() || !integer_value(*id, 1, std::numeric_limits<int>::max()) ||
            target == input.end() || !text_value(*target, 128, true)) {
            return {false, "Choose a config and user database node."};
        }
        std::string_view cursor;
        if (const auto supplied = input.find("cursor"); supplied != input.end()) {
            if (!text_value(*supplied, 64) ||
                !valid_waterwall_users_cursor(cursor = supplied->get_ref<const std::string&>())) {
                return {false, "Choose a valid WaterWall users page.", false, {{"reset_cursor", true}}};
            }
        }
        const auto unavailable = [] {
            return Reply{true,
                         {},
                         false,
                         {{"available", false}, {"users", Json::array()}, {"total", 0}, {"next_cursor", nullptr}}};
        };
        const auto config = live_configs_.find(id->get<int>());
        if (core_->state() != "running" || config == live_configs_.end() ||
            !has_waterwall_users_api(config->second)) {
            return unavailable();
        }
        const auto names = config_auth_nodes(config->second);
        if (std::ranges::find(names, *target) == names.end()) {
            return {false, "That user database node is unavailable."};
        }
        const auto& node = target->get_ref<const std::string&>();
        auto endpoint = waterwall_users_endpoint(config->second, node);
        if (!endpoint) {
            return {false, std::string(schematic_text(
                               waterwall_schematic().at("ui").at("users").at("live_setup_help")))};
        }
        std::size_t bindings = 0;
        for (const auto& [_, launch_config] : live_configs_) {
            bindings += waterwall_tcp_bindings(launch_config, *endpoint);
        }
        if (bindings != 1) {
            return {false, "Use a dedicated listener port for the WaterWall API."};
        }

        const auto run = live_run_;
        const auto pid = core_->running_pid();
        // Keep lifecycle actions and host sampling independent of the API's network deadline.
        lock.unlock();
        auto page = list_waterwall_users(pid, *endpoint, node, cursor);
        lock.lock();
        if (live_run_ != run || core_->state() != "running") {
            return unavailable();
        }
        return {page.error.empty(), std::move(page.error), false, std::move(page.result)};
    }

    Reply edit_core_config(std::string_view action, const Json& input)
    {
        const auto id = input.find(action.ends_with("_config") ? "id" : "config_id");
        if (id == input.end() || !integer_value(*id, 1, std::numeric_limits<int>::max())) {
            return {false, "Choose a config."};
        }
        auto& configs = data_["server"]["core"]["configs"];
        const auto row = std::ranges::find_if(configs, [&](const Json& entry) { return entry["id"] == *id; });
        if (row == configs.end()) {
            return {false, "That config is unavailable."};
        }
        const auto& name = (*row)["name"].get_ref<const std::string&>();
        const bool active = (*row)["active"] == true;
        const bool graph_action = action == "get_core_config" || action == "save_core_config";
        if (!graph_action && core_in_use(core_->state())) {
            return {false, "Stop WaterWall to use the offline users editor."};
        }
        auto document = config_files_.read(name);
        if (!document.error.empty()) {
            return {false, std::move(document.error)};
        }
        if (action == "get_core_config") {
            const bool changed = update_config_tabs(*row, document.config);
            return {true,
                    {},
                    changed,
                    {{"config", std::move(document.config)},
                     {"revision", std::move(document.revision)},
                     {"editable", !active}}};
        }
        if (action == "save_core_config") {
            if (active) {
                return {false, "Stop WaterWall before editing this config."};
            }
            return save_config(input, *row, name, document);
        }
        return edit_config_users(action, input, name, document.config);
    }

    Reply save_config(const Json& input, Json& row, std::string_view name, const CoreConfigDocument& document)
    {
        const auto revision = input.find("revision");
        if (revision == input.end() || *revision != document.revision) {
            return {false, "The config changed on disk. Reload it before saving again."};
        }
        auto config = input.value("config", Json{});
        if (auto error = validate_config_graph(config, document.config); !error.empty()) {
            return {false, std::move(error)};
        }
        if (auto error = configure_auth_databases(config, document.config, name, database_->directory());
            !error.empty()) {
            return {false, std::move(error)};
        }
        const auto contents = config.dump(2) + '\n';
        if (contents.size() > max_core_config_bytes) {
            return {false, "Wave can save configs up to 256 KiB."};
        }

        std::vector<std::string> created;
        if (auto error = prepare_auth_databases(name, config, created); !error.empty()) {
            remove_created_auth_databases(name, created);
            return {false, std::move(error)};
        }
        auto saved = config_files_.save(name, contents);
        if (!saved) {
            remove_created_auth_databases(name, created);
            return {false, "Could not save the config file."};
        }
        update_config_tabs(row, config);
        auto reply = complete("Saved config " + std::string(name) + '.');
        reply.result = {{"config", std::move(config)}, {"revision", std::move(*saved)}, {"editable", true}};
        return reply;
    }

    Reply edit_config_users(std::string_view action, const Json& input, std::string_view name,
                            const Json& config)
    {
        const auto target = input.find("node");
        const auto nodes = config.find("nodes");
        if (target == input.end() || !text_value(*target, 128, true) || nodes == config.end() ||
            !nodes->is_array()) {
            return {false, "Choose a user database node."};
        }
        const auto node = std::ranges::find_if(*nodes, [&](const Json& candidate) {
            return candidate.is_object() && candidate.value("name", Json{}) == *target &&
                   uses_auth_database(candidate);
        });
        if (node == nodes->end()) {
            return {false, "That user database node is unavailable."};
        }
        const auto db = auth_database_name(*node, name, database_->directory());
        if (db.empty()) {
            return {false, "Save the node with a database in its config folder before managing its users."};
        }
        auto result = config_users_.execute(action, name, db, input);
        if (result.success && workspace_mutation(action)) {
            auto reply = complete(std::move(result.message));
            reply.result = std::move(result.result);
            return reply;
        }
        return {result.success, std::move(result.message), false, std::move(result.result)};
    }

    Reply dispatch(std::string_view action, const Json& input)
    {
        if (action == "refresh") {
            return complete("Workspace refreshed.", "info");
        }
        if (action == "install_core" || action == "start_core" || action == "stop_core" ||
            action == "restart_core") {
            return change_core_state(action);
        }
        if (action == "check_core_updates") {
            return check_core_updates();
        }
        if (action == "check_panel_updates") {
            return check_panel_updates();
        }
        if (action == "check_updates") {
            return check_updates();
        }
        if (action == "update_core") {
            return update_core_release(input);
        }
        if (action == "create_core_config") {
            return create_core_config(input);
        }
        if (action == "set_core_config_enabled" || action == "remove_core_config") {
            return change_core_config(action, input);
        }
        if (action == "get_core_config" || action == "save_core_config" || action == "get_config_users" ||
            action == "get_config_user_password" || action == "create_config_user" ||
            action == "edit_config_user" || action == "delete_config_user") {
            return edit_core_config(action, input);
        }
        if (action == "clear_logs") {
            core_logs_.read();
            data_["logs"].clear();
            return {true, "Logs cleared.", true};
        }
        if (action == "apply_settings") {
            return apply_settings(input);
        }
        if (action == "apply_tls_settings") {
            return apply_tls_settings(input);
        }
        if (action == "apply_session_settings") {
            return apply_session_settings(input);
        }
        if (action == "apply_theme") {
            return apply_theme(input);
        }
        if (action == "apply_donation") {
            return apply_donation(input);
        }
        if (action == "apply_core_settings") {
            return apply_core_settings(input);
        }
        if (action == "restore_preview_backup") {
            return restore_backup(input);
        }
        if (action == "diagnostics") {
            auto feedback = "WaterWall is " + std::string(core_->state()) + '.';
            const auto& error = core_error();
            if (!error.empty()) {
                feedback += ' ' + error;
            }
            return complete(std::move(feedback), "info");
        }
        return {false, "That preview action is unavailable."};
    }

    std::mutex mutex_;
    std::condition_variable_any timer_;
    LinuxMetrics host_;
    std::shared_ptr<PanelDatabase> database_;
    CoreConfigFiles config_files_;
    CoreConfigUsers config_users_;
    std::unique_ptr<WaterWall> core_;
    WaterWallLogs core_logs_;
    std::filesystem::path waterwall_bin_;
    std::optional<CoreUpdate> core_update_;
    std::chrono::steady_clock::time_point next_catalog_check_{};
    std::future<CoreCatalogResult> catalog_result_;
    std::jthread catalog_worker_;
    bool manual_catalog_check_ = false;
    bool queued_catalog_check_ = false;
    bool restart_pending_ = false;
    std::string restart_error_;
    std::map<int, Json> live_configs_;
    std::uint64_t live_run_ = 0;
    Json data_ = make_data();
    Json saved_core_settings_;
    std::optional<Donation> donation_;
    int next_config_id_ = 1;
    std::uint64_t revision_ = 1;
    std::uint64_t schematic_revision_ = 1;
    std::future<PanelUpdateCheck> panel_update_result_;
    std::jthread panel_update_worker_;
    std::jthread sampler_;
};

} // namespace

void configure_preview_routes(httplib::Server& server, std::shared_ptr<AuthStore> store,
                              std::shared_ptr<PanelDatabase> database)
{
    const auto preview = std::make_shared<Preview>(std::move(database));
    server.WebSocket(panel_route("/ws/overview"),
                     [preview, store](const httplib::Request& req, httplib::ws::WebSocket& socket) {
                         if (!websocket_session(store, req)()) {
                             socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                             return;
                         }
                         if (socket.send(preview->overview())) {
                             socket.close();
                         }
                     });
    server.WebSocket(panel_route("/ws/workspace"), [preview, store = std::move(store)](const httplib::Request& req,
                                                                                   httplib::ws::WebSocket& socket) {
        preview->serve(socket, websocket_session(store, req));
    });
}

} // namespace wave
