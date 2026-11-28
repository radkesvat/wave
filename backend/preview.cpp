#include "preview.h"

#include "auth.h"
#include "auth_store.h"
#include "core_config_files.h"
#include "core_config_graph.h"
#include "core_config_json.h"
#include "core_config_name.h"
#include "core_config_users.h"
#include "core_settings.h"
#include "linux_metrics.h"
#include "panel_base_path.h"
#include "panel_settings.h"
#include "panel_update.h"
#include "preview_backup.h"
#include "session_settings.h"
#include "users_roster_stream.h"
#include "waterwall.h"
#include "waterwall_logs.h"
#include "waterwall_schematic.h"
#include "waterwall_schematic_catalog.h"
#include "waterwall_schematic_file.h"
#include "waterwall_users.h"
#include "wave_version.h"
#include "workspace_backup.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <compare>
#include <cstdint>
#include <ctime>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using UtcTime = std::chrono::sys_seconds;
using namespace std::chrono_literals;

constexpr auto host_sample_interval = 500ms;
constexpr auto users_roster_poll_interval = 500ms;
constexpr auto core_update_check_interval = 24h;
constexpr auto donation_reminder_interval = std::chrono::days{30};
constexpr std::size_t max_request_bytes = 8192;
constexpr std::size_t max_user_request_bytes = 32 * 1024;
constexpr std::size_t max_live_user_request_bytes = 32 * 1024 + 2048;

class WorkspaceBackupBusy : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class WorkspaceBackupSessionEnded : public std::runtime_error {
  public:
    WorkspaceBackupSessionEnded() : std::runtime_error("Sign in to restore a workspace backup.") {}
};

std::string new_restore_token()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) {
        throw std::runtime_error("Could not create a workspace restore token.");
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        token += digits[byte >> 4];
        token += digits[byte & 0xf];
    }
    return token;
}

bool core_in_use(std::string_view state)
{
    return state == "starting" || state == "running" || state == "stopping";
}

std::optional<WaterWallUsersOperation> live_users_operation(std::string_view action)
{
    using Op = WaterWallUsersOperation;
    constexpr std::pair<std::string_view, Op> actions[] = {
        {"get_live_config_user_status", Op::status},
        {"get_live_config_users", Op::list},
        {"get_live_config_user", Op::get},
        {"create_live_config_user", Op::create},
        {"edit_live_config_user", Op::update},
        {"change_live_config_user_password", Op::password},
    };
    const auto found = std::ranges::find(actions, action, &std::pair<std::string_view, Op>::first);
    return found == std::end(actions) ? std::nullopt : std::optional(found->second);
}

bool live_users_write(WaterWallUsersOperation operation)
{
    using Op = WaterWallUsersOperation;
    return operation == Op::create || operation == Op::update || operation == Op::password;
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
        "reset_config_user_traffic",
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
        "get_config_user",
        "get_config_user_password",
        "create_config_user",
        "edit_config_user",
        "delete_config_user",
        "reset_config_user_traffic",
    };
    return live_users_operation(action).has_value() || std::ranges::find(actions, action) != std::end(actions);
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
    constexpr std::string_view answers[] = {"no", "1", "3", "5", "10", "15", "30", "50_plus",
                                           "below_5", "more_than_5"};
    return std::ranges::find(answers, answer) != std::end(answers);
}

UtcTime utc_now()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

std::string format_utc_timestamp(UtcTime time)
{
    const auto seconds = std::chrono::system_clock::to_time_t(time);
    std::tm date{};
    char timestamp[21];
    if (!gmtime_r(&seconds, &date) ||
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &date) != 20) {
        return {};
    }
    return timestamp;
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
    if (!time) {
        return std::nullopt;
    }
    return Donation{*answer, *timestamp, *time};
}

Json donation_settings(const std::optional<Donation>& donation)
{
    if (!donation) {
        return {{"reminder_due", true}};
    }
    const auto reminder_at = donation->time + donation_reminder_interval;
    const bool donated = donation->answer != "no";
    const bool due = donated && utc_now() >= reminder_at;
    Json settings = {{"answer", donation->answer}, {"answered_at", donation->answered_at}, {"reminder_due", due}};
    if (donated && !due) {
        settings["next_reminder_at"] = format_utc_timestamp(reminder_at);
    }
    return settings;
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

bool panel_restore_needs_restart(const Json& panel, const PanelSettings& active)
{
    const auto& listen = panel.at("listen");
    return listen.at("host") != active.listen_host || listen.at("port") != active.listen_port ||
           panel_tls_settings(panel.at("tls"), active.tls).at("restart_required") == true;
}

struct Reply {
    bool ok;
    std::string feedback;
    bool changed = false;
    Json result = nullptr;
    bool feedback_in_logs_only = false;
};

struct WorkspaceReply {
    std::string payload;
    std::optional<PanelListenerRestart> listener_restart;
};

struct RestoreReply {
    Json data = {{"ok", true}};
    std::optional<PanelListenerRestart> listener_restart;
};

struct CoreCatalogResult {
    std::vector<std::string> versions;
    std::string error;
};

struct CoreUpdate {
    std::shared_ptr<const WaterWallSchematic> schematic;
    std::unique_ptr<WaterWall> candidate;
};

struct PendingWorkspaceBackup {
    WorkspaceBackup backup;
    WorkspaceRestoreFiles existing;
    std::string session;
    std::string token;
    std::chrono::steady_clock::time_point expires;
};

struct LiveUsersAccess {
    std::map<WaterWallUsersOperation, std::vector<WaterWallUsersEndpoint>> endpoints;
    std::string reason;
    std::uint64_t database_generation = 0;
};

using RosterTime = std::chrono::steady_clock::time_point;

struct UsersRosterScope {
    int config;
    std::string node;
    bool live;
    std::string run;
    std::string generation;
    std::uint64_t schematic;

    auto operator<=>(const UsersRosterScope&) const = default;
};

struct UsersRosterTarget {
    UsersRosterScope scope;
    std::string config_name;
    std::string database;
    LiveUsersRosterReader reader;
    std::shared_ptr<const UsersRosterVersion> version;
    std::shared_ptr<const UsersRosterDelta> delta;
    UsersRosterClock clock;
    RosterTime sampled_at{};
    RosterTime due = RosterTime::min();
    std::uint64_t last_sequence = 0;
    std::uint64_t state_revision = 1;
    std::uint64_t loss_revision = 0;
    std::string_view loss_cause = "identity";
    std::string loss_message;
    std::uint64_t saved_write = 0;
    std::uint64_t saved_started_validation = 0;
    std::uint64_t saved_finished_validation = 0;
    std::uint64_t saved_validation = 0;
    std::uint64_t saved_loss_validation = 0;
    std::size_t viewers = 0;
    unsigned failures = 0;
    std::string_view state = "loading";
    std::string message;
    bool force = true;
};

struct UsersRosterSubscription {
    std::string id;
    std::shared_ptr<UsersRosterTarget> target;
    std::shared_ptr<const UsersRosterVersion> delivered;
    std::optional<UsersRosterTransfer> transfer;
    std::uint64_t state_revision = 0;
    std::uint64_t loss_revision = 0;
    std::uint64_t required_saved_validation = 0;
    std::uint64_t attempt = 0;
    std::string notice;
};

struct UsersRosterFrame {
    std::string payload;
    bool completes_transfer = false;
};

struct UsersRosterPoll {
    std::shared_ptr<UsersRosterVersion> version;
    std::optional<UsersRosterDelta> delta;
    UsersRosterClock clock;
    std::string error;
    std::optional<UsersRosterLoss> loss;
};

class Preview {
  public:
    Preview(std::shared_ptr<PanelDatabase> database, PreparePanelRestart prepare_restart, bool log_panel_events)
        : database_(std::move(database)), prepare_restart_(std::move(prepare_restart)), config_files_(database_->directory()),
          config_users_(database_->directory()),
          core_(std::make_unique<WaterWall>(database_->directory(), waterwall_version())),
          core_logs_(database_->directory()),
          waterwall_bin_(waterwall_bin_directory(database_->directory())), log_panel_events_(log_panel_events)
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
        for (auto& worker : roster_workers_) {
            worker = std::jthread([this](std::stop_token stop) { poll_users_rosters(stop); });
        }
        sampler_ = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(mutex_);
            auto sampled_at = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                timer_.wait_until(lock, stop, sampled_at + host_sample_interval, [] { return false; });
                if (stop.stop_requested() || shutting_down_) {
                    break;
                }
                sampled_at = std::chrono::steady_clock::now();
                expire_workspace_backup();
                host_.sample(data_["server"]);
                update_core();
                finish_panel_update_check();
                if (!panel_update_result_.valid() && !catalog_result_.valid() && !queued_catalog_check_) {
                    data_["server"]["checking_updates"] = false;
                }
                ++revision_;
            }
            // Linux ties WaterWall's parent-death signal to the thread that started it.
            core_->shutdown();
        });
    }

    void request_shutdown()
    {
        {
            std::lock_guard lock(mutex_);
            shutting_down_ = true;
        }
        sampler_.request_stop();
    }

    std::string download_workspace_backup()
    {
        BackupEntries files;
        {
            std::lock_guard lock(mutex_);
            require_workspace_backup_idle();
            auto panel = database_->capture_panel_backup();
            if (!read_donation(panel.at("donation"))) {
                panel["donation"] = nullptr;
            }
            files = capture_workspace_backup(*database_, std::move(panel), saved_core_settings_,
                                             saved_config_registry(data_["server"]["core"]["configs"]));
        }
        return write_backup_archive(files);
    }

    Json stage_workspace_backup(std::string_view archive, std::string session)
    {
        auto backup = prepare_workspace_backup(*database_, read_backup_archive(archive));
        const auto version = schematic_text(backup.manifest.at("waterwall_version"));
        const auto& configs = backup.manifest.at("configs");
        Json restored_panel;
        if (const auto panel = backup.manifest.find("panel_settings"); panel != backup.manifest.end()) {
            const auto& donation = panel->at("donation");
            if (!donation.is_null() && !read_donation(donation)) {
                throw std::runtime_error("The backup contains an invalid donation response or UTC timestamp.");
            }
            if (const auto error = database_->panel_backup_tls_error(*panel); !error.empty()) {
                backup.missing_dependencies.push_back(
                    "The current listener will stay active if Wave cannot load the restored TLS settings. " + error);
            }
            restored_panel = {{"base_path", panel->at(panel_base_path_key)},
                              {"administrator", panel->at("admin").front().at("username")},
                              {"restart_required", panel_restore_needs_restart(*panel, database_->settings())}};
        }
        auto token = new_restore_token();

        std::lock_guard lock(mutex_);
        require_workspace_backup_idle();
        auto existing = existing_workspace_restore_files(database_->directory(), backup);
        auto replaced = Json::array();
        std::set<std::string> replaced_names;
        auto removed = Json::array();
        for (const auto& row : data_["server"]["core"]["configs"]) {
            const auto& name = row.at("name");
            if (std::ranges::any_of(configs, [&](const Json& config) { return config.at("name") == name; })) {
                replaced_names.insert(name.get<std::string>());
            } else {
                removed.push_back(name);
            }
        }
        for (const auto& [path, stamp] : existing) {
            replaced_names.insert(std::filesystem::path(path).parent_path().filename().string());
        }
        for (const auto& name : replaced_names) {
            replaced.push_back(name);
        }
        auto dependencies = std::move(backup.missing_dependencies);
        if (!WaterWall(database_->directory(), version).installed()) {
            dependencies.push_back("Install WaterWall " + std::string(version) + " before starting the core.");
        }
        Json result = {{"ok", true}, {"token", token}, {"waterwall_version", version},
                       {"current_waterwall_version", waterwall_version()}, {"configs", configs},
                       {"replaced_configs", std::move(replaced)}, {"removed_configs", std::move(removed)},
                       {"missing_dependencies", std::move(dependencies)},
                       {"compatibility_warnings", std::move(backup.compatibility_warnings)}};
        if (!restored_panel.is_null()) {
            result["wave_version"] = backup.manifest.at("wave_version");
            result["panel_settings"] = std::move(restored_panel);
        }
        pending_backup_ = {std::move(backup), std::move(existing), std::move(session), std::move(token),
                           std::chrono::steady_clock::now() + 5min};
        return result;
    }

    RestoreReply restore_workspace_backup(std::string_view token, std::string_view session, AuthStore& store)
    {
        std::lock_guard lock(mutex_);
        if (listener_restart_pending_) {
            throw WorkspaceBackupBusy("Wait for the panel listener to restart before restoring a backup.");
        }
        expire_workspace_backup();
        if (!pending_backup_ || pending_backup_->token != token || pending_backup_->session != session) {
            throw std::runtime_error("Choose and validate the workspace backup again before restoring it.");
        }
        require_workspace_backup_idle();
        if (catalog_result_.valid()) {
            throw WorkspaceBackupBusy("Wait for the WaterWall release check to finish before restoring.");
        }

        auto pending = std::move(*pending_backup_);
        pending_backup_.reset();
        auto& backup = pending.backup;
        if (existing_workspace_restore_files(database_->directory(), backup) != pending.existing) {
            throw std::runtime_error("Workspace files changed. Validate the backup again before restoring it.");
        }
        const auto version = schematic_text(backup.manifest.at("waterwall_version"));
        const auto schematic = prepare_waterwall_release_schematic(waterwall_bin_, version);
        if (waterwall_schematic(*schematic) != waterwall_schematic(*backup.schematic)) {
            throw std::runtime_error("The WaterWall schematic changed. Validate the backup again before restoring it.");
        }
        auto candidate = std::make_unique<WaterWall>(database_->directory(), version);
        const auto panel = backup.manifest.find("panel_settings");
        RestoreReply reply;
        std::string listener_error;
        const auto active = database_->settings();
        if (panel != backup.manifest.end() && panel_restore_needs_restart(*panel, active)) {
            try {
                auto settings = database_->panel_backup_listener_settings(*panel);
                auto listener = database_->make_listener(settings);
                reply.listener_restart.emplace(prepare_restart_(std::move(listener), std::move(settings)));
            } catch (const std::runtime_error& error) {
                listener_error = error.what();
            }
        }
        try {
            if (panel == backup.manifest.end()) {
                apply_workspace_backup(*database_, backup, pending.token);
            } else {
                const auto& restored_path = panel->at(panel_base_path_key).get_ref<const std::string&>();
                reply.data["base_path"] = restored_path;
                if (const auto& restart = reply.listener_restart) {
                    auto destination = Json::object();
                    if (const auto& tls = restart->settings.tls) {
                        destination["protocol"] = "https:";
                        destination["hostname"] = tls->domain;
                    } else if (active.tls) {
                        destination["protocol"] = "http:";
                    }
                    if (!restart->bind_after_stop || restart->settings.tls || active.tls) {
                        destination["port"] = restart->port;
                    }
                    reply.data["restart"] = std::move(destination);
                    database_->begin_listener_restart();
                }
                if (!store.restore_admin(panel->at("admin"), session, restored_path, [&](const Json& auth_state) {
                        apply_workspace_backup(*database_, backup, pending.token, auth_state);
                    })) {
                    throw WorkspaceBackupSessionEnded();
                }
            }
        } catch (...) {
            if (reply.listener_restart) {
                database_->finish_listener_restart(std::nullopt);
            }
            std::error_code error;
            workspace_recovery_required_ = std::filesystem::exists(database_->directory() / ".wave-restore", error) ||
                                           static_cast<bool>(error);
            throw;
        }

        if (panel != backup.manifest.end()) {
            auto& settings = data_["settings"];
            settings["session_expiry_days"] = panel->at("session_expiry_days");
            settings["theme"] = panel->at("theme");
            settings["tls"] = panel_tls_settings(panel->at("tls"), active.tls);
            donation_ = read_donation(panel->at("donation"));
            if (!listener_error.empty()) {
                log("error", "Could not activate the restored panel listener. Wave kept the previous listener. " +
                                 listener_error);
            }
        }
        saved_core_settings_ = std::move(backup.manifest.at("core_settings"));
        data_["server"]["core"]["available_versions"] = cached_waterwall_versions(waterwall_bin_);
        publish_core_release({std::move(backup.schematic), std::move(candidate)});
        for (const auto& [scope, target] : roster_targets_) {
            ++target->saved_write;
            invalidate_users_roster(*target, "The saved workspace was restored.");
        }
        offline_users_bindings_.clear();
        offline_users_config_errors_.clear();
        next_config_id_ = 1;
        data_["server"]["core"]["configs"].clear();
        load_core_configs(backup.manifest.at("configs"));
        workspace_restored_ = true;
        sync_core();
        log("success", "Restored the WaterWall workspace backup.");
        listener_restart_pending_ = reply.listener_restart.has_value();
        ++revision_;
        return reply;
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

    std::string_view workspace_id() const { return users_session_id_; }

    std::string http_snapshot(std::string_view known_workspace, std::uint64_t sent_schematic,
                              const std::function<bool()>& session)
    {
        std::lock_guard lock(mutex_);
        if (!session()) {
            return {};
        }
        auto frame = snapshot(false);
        if (known_workspace != workspace_id()) {
            sent_schematic = 0;
        }
        attach_schematic(frame, sent_schematic);
        return frame.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    WorkspaceReply http_action(std::string_view message, const std::function<bool()>& session)
    {
        std::uint64_t sent_revision = 0;
        std::uint64_t sent_schematic = 0;
        return handle_message(message, sent_revision, sent_schematic, nullptr, session);
    }

    void finish_listener_restart(bool applied)
    {
        std::lock_guard lock(mutex_);
        listener_restart_pending_ = false;
        if (applied) {
            data_["settings"]["tls"]["restart_required"] = false;
            log("info", database_->settings().tls ? "Panel HTTPS is active." : "Panel HTTP is active.");
        } else {
            log("error", "Could not restart the panel listener. Wave resumed the previous listener with the saved settings pending.");
        }
        ++revision_;
    }

    void serve(httplib::ws::WebSocket& socket, const std::function<bool()>& session,
               const RestartPanel& restart_panel)
    {
        // Only this handler writes to its socket. A short read timeout lets it
        // publish shared state changes while the current browser is idle.
        socket.set_read_timeout(100ms);
        UsersRosterSubscription roster;
        const auto release = [this](UsersRosterSubscription* sub) {
            std::lock_guard lock(mutex_);
            release_users_roster(*sub);
        };
        const std::unique_ptr<UsersRosterSubscription, decltype(release)> lease(&roster, release);
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
            auto frame = result == httplib::ws::Timeout
                             ? WorkspaceReply{changed_snapshot(sent_revision, sent_schematic), {}}
                             : handle_message(result == httplib::ws::Text ? std::string_view(message)
                                                                         : std::string_view{},
                                              sent_revision, sent_schematic, &roster, session);
            if (frame.listener_restart) {
                // The change is saved even if the client has already disconnected.
                if (session()) {
                    socket.send(frame.payload);
                }
                restart_panel(std::move(*frame.listener_restart));
                return;
            }
            if (!frame.payload.empty()) {
                if (!session()) {
                    socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                    return;
                }
                if (!socket.send(frame.payload)) {
                    return;
                }
            }
            if (roster.id.empty()) {
                continue;
            }
            auto update = next_users_roster_frame(roster);
            if (!update.payload.empty()) {
                if (!session()) {
                    socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                    return;
                }
                if (!socket.send(update.payload)) {
                    return;
                }
                if (update.completes_transfer) {
                    roster.delivered = roster.transfer->version();
                    roster.transfer.reset();
                }
            }
        }
    }

  private:
    void require_workspace_backup_idle() const
    {
        if (workspace_recovery_required_) {
            throw std::runtime_error("Restart Wave to finish workspace restore recovery.");
        }
        if (core_in_use(core_->state()) || restarting_or_installing()) {
            throw WorkspaceBackupBusy("Stop WaterWall and wait for any installation to finish first.");
        }
    }

    bool restarting_or_installing() const
    {
        return restart_pending_ || core_update_.has_value() || core_->state() == "installing";
    }

    void expire_workspace_backup()
    {
        if (pending_backup_ && pending_backup_->expires <= std::chrono::steady_clock::now()) {
            pending_backup_.reset();
        }
    }

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
        if (const auto error = core_config_registry_error(saved_configs); !error.empty()) {
            throw std::runtime_error("core_configs in wave-db.json " + std::string(error));
        }
        auto& configs = data_["server"]["core"]["configs"];
        for (const auto& row : saved_configs) {
            const auto& name = row.at("name").get_ref<const std::string&>();
            const auto id = row.at("id").get<int>();
            configs.push_back({{"id", id}, {"name", name}, {"enabled", row.at("enabled")}, {"active", false}});
            update_read_config_tabs(configs.back(), config_files_.read(name));
            next_config_id_ = std::max(next_config_id_, id + 1);
        }
    }

    static Json saved_config_registry(const Json& configs)
    {
        auto stored = configs;
        for (auto& config : stored) {
            config.erase("active");
            config.erase("auth_nodes");
            config.erase("live_users");
        }
        return stored;
    }

    bool store_configs(const Json& configs)
    {
        try {
            database_->save_core_configs(saved_config_registry(configs));
        } catch (const std::runtime_error&) {
            return false;
        }
        return true;
    }

    Json snapshot(bool users_roster) const
    {
        Json snapshot = {{"type", "snapshot"}, {"data", data_}};
        snapshot["users_roster"] = users_roster;
        if (!users_roster) {
            snapshot["workspace_id"] = workspace_id();
        }
        snapshot["data"]["settings"]["donation"] = donation_settings(donation_);
        auto& core = snapshot["data"]["server"]["core"];
        const auto state = core["state"].get_ref<const std::string&>();
        for (auto& config : core["configs"]) {
            if (const auto nodes = config.find("auth_nodes"); nodes != config.end()) {
                auto access = Json::object();
                const auto id = config["id"].get<int>();
                for (const auto& node : *nodes) {
                    const auto& name = node.get_ref<const std::string&>();
                    access[name] = users_access(id, name, state);
                }
                config["users_access"] = std::move(access);
            }
        }
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
        auto frame = snapshot(true);
        attach_schematic(frame, sent_schematic);
        sent_revision = revision_;
        return frame.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    const Json* roster_config(int config) const
    {
        const auto& configs = data_["server"]["core"]["configs"];
        const auto found = std::ranges::find_if(configs, [&](const Json& row) { return row["id"] == config; });
        return found == configs.end() ? nullptr : &*found;
    }

    std::string roster_database_generation(std::uint64_t generation) const
    {
        return generation == 0 ? std::string{} : users_session_id_ + '/' + std::to_string(generation);
    }

    std::optional<UsersRosterScope> roster_scope(int config, const std::string& node, bool live) const
    {
        const auto state = core_->state();
        if (live) {
            const auto access = live_users_access_.find({config, node});
            if (state != "running" || !live_configs_.contains(config) || access == live_users_access_.end() ||
                !access->second.endpoints.contains(WaterWallUsersOperation::list)) {
                return std::nullopt;
            }
            return UsersRosterScope{config, node, true, users_run_id(config, node),
                                    roster_database_generation(access->second.database_generation), schematic_revision_};
        }
        const auto* binding = offline_users_binding(config, node);
        if (core_in_use(state) || !binding || binding->database.empty() || !roster_config(config)) {
            return std::nullopt;
        }
        return UsersRosterScope{config, node, false, offline_users_run_id(config, node, *binding),
                                roster_database_generation(binding->generation), schematic_revision_};
    }

    void wake_roster_workers()
    {
        ++roster_schedule_;
        roster_timer_.notify_all();
    }

    bool roster_is_current(const UsersRosterTarget& target) const
    {
        return target.viewers != 0 &&
               roster_scope(target.scope.config, target.scope.node, target.scope.live) == target.scope;
    }

    bool roster_can_read(const UsersRosterTarget& target) const
    {
        return roster_is_current(target) &&
               (target.scope.live || !offline_users_config_errors_.contains(target.scope.config));
    }

    static void clear_users_roster_delivery(UsersRosterSubscription& sub)
    {
        sub.delivered.reset();
        sub.transfer.reset();
    }

    void leave_users_roster(UsersRosterSubscription& sub)
    {
        if (!sub.target) {
            return;
        }
        if (--sub.target->viewers == 0) {
            roster_targets_.erase(sub.target->scope);
            wake_roster_workers();
        }
        sub.target.reset();
        clear_users_roster_delivery(sub);
    }

    void release_users_roster(UsersRosterSubscription& sub)
    {
        leave_users_roster(sub);
        sub = {};
    }

    void lose_users_roster(UsersRosterSubscription& sub, std::string message)
    {
        leave_users_roster(sub);
        sub.notice = std::move(message);
    }

    bool refresh_roster_binding(int config)
    {
        auto& configs = data_["server"]["core"]["configs"];
        const auto found = std::ranges::find_if(configs, [&](const Json& row) { return row["id"] == config; });
        if (found == configs.end() || core_in_use(core_->state())) {
            return false;
        }
        const auto document = config_files_.read((*found)["name"].get_ref<const std::string&>());
        if (update_read_config_tabs(*found, document)) {
            ++revision_;
        }
        return !offline_users_config_errors_.contains(config);
    }

    std::uint64_t schedule_saved_roster_validation(UsersRosterTarget& target)
    {
        target.due = RosterTime::min();
        // A read already in progress reserved its snapshot before this entry.
        return target.saved_started_validation + 1;
    }

    void handle_users_roster(std::string_view action, const Json& input, UsersRosterSubscription& sub)
    {
        const auto& raw_subscription = schematic_field(input, "subscription");
        if (!text_value(raw_subscription, 64, true)) {
            return;
        }
        const auto& subscription = raw_subscription.get_ref<const std::string&>();
        if (action == "users_roster_unsubscribe") {
            if (sub.id == subscription) {
                release_users_roster(sub);
            }
            return;
        }
        if (action == "users_roster_refresh") {
            if (!sub.target || sub.id != subscription) {
                return;
            }
            auto& target = *sub.target;
            if (schematic_field(input, "resync") == true) {
                clear_users_roster_delivery(sub);
                sub.state_revision = 0;
            } else if (target.scope.live) {
                set_roster_state(target, "loading");
                target.force = true;
                target.due = RosterTime::min();
                wake_roster_workers();
            } else if (refresh_roster_binding(target.scope.config) && roster_is_current(target)) {
                set_roster_state(target, "loading");
                schedule_saved_roster_validation(target);
                wake_roster_workers();
            }
            return;
        }

        const auto& config = schematic_field(input, "config_id");
        const auto& node = schematic_field(input, "node");
        const auto& source = schematic_field(input, "source");
        const auto& revision = schematic_field(input, "schematic_revision");
        const auto& run = schematic_field(input, "run_id");
        const auto run_id = schematic_text(run);
        const auto& generation = schematic_field(input, "database_generation");
        const bool live = source == "live";
        std::optional<UsersRosterScope> scope;
        if (integer_value(config, 1, std::numeric_limits<int>::max()) && text_value(node, 128, true) &&
            (live || source == "offline") && text_value(run, 256, true) &&
            (generation.is_null() || text_value(generation, 128)) &&
            (revision == schematic_revision_ ||
             (revision.is_null() && schematic_revision_ == 1 && waterwall_version() == bundled_waterwall_version))) {
            const auto id = config.get<int>();
            const auto& node_name = node.get_ref<const std::string&>();
            scope = roster_scope(id, node_name, live);
            if (sub.id == subscription && sub.target && scope && *scope == sub.target->scope &&
                scope->run == run_id && scope->generation == schematic_text(generation)) {
                return;
            }
            if (!live) {
                refresh_roster_binding(id);
                scope = roster_scope(id, node_name, false);
            }
        }
        if (!scope || scope->run != run_id || scope->generation != schematic_text(generation) ||
            (sub.id == subscription && sub.target && sub.target->scope != *scope)) {
            release_users_roster(sub);
            sub.id = subscription;
            lose_users_roster(sub, "The user list is unavailable for this config, node or run.");
            return;
        }
        release_users_roster(sub);
        auto& target = roster_targets_[*scope];
        if (!target) {
            target = std::make_shared<UsersRosterTarget>();
            target->scope = *scope;
            if (!live) {
                target->config_name = roster_config(scope->config)->at("name").get<std::string>();
                target->database = offline_users_binding(scope->config, scope->node)->database;
            }
        }
        ++target->viewers;
        sub.id = subscription;
        sub.target = target;
        if (!live) {
            if (const auto error = offline_users_config_errors_.find(scope->config);
                error != offline_users_config_errors_.end()) {
                if (target->state != "lost") {
                    invalidate_users_roster(*target, error->second, "config_unverified");
                }
            } else {
                if (target->state == "lost") {
                    set_roster_state(*target, "loading");
                }
                sub.required_saved_validation = schedule_saved_roster_validation(*target);
            }
        } else if (target->state == "lost") {
            set_roster_state(*target, "loading");
            target->force = true;
            target->due = RosterTime::min();
        }
        sub.loss_revision = target->loss_revision;
        wake_roster_workers();
    }

    static void set_roster_state(UsersRosterTarget& target, std::string_view state, std::string message = {})
    {
        if (target.state != state || target.message != message) {
            target.state = state;
            target.message = std::move(message);
            ++target.state_revision;
        }
    }

    static UsersRosterClock current_roster_clock(const UsersRosterTarget& target)
    {
        auto clock = target.clock;
        if (clock.wall_time_ms == 0) {
            return clock;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - target.sampled_at).count();
        const auto advance = [elapsed](std::uint64_t time) {
            const auto duration = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
            return time + std::min(duration, std::numeric_limits<std::uint64_t>::max() - time);
        };
        clock.wall_time_ms = advance(clock.wall_time_ms);
        if (clock.accounting_time_ms) {
            clock.accounting_time_ms = advance(*clock.accounting_time_ms);
        }
        return clock;
    }

    UsersRosterFrame next_users_roster_frame(UsersRosterSubscription& sub)
    {
        std::unique_lock lock(mutex_);
        if (sub.target && !roster_is_current(*sub.target)) {
            lose_users_roster(sub, "The user database scope changed. Reopen the Users tab.");
        }
        const auto state_frame = [&](std::string_view state, std::string_view message, UsersRosterClock clock,
                                     std::string_view cause = {}) {
            Json frame{{"type", "users_roster"}, {"subscription", sub.id}, {"event", "state"},
                       {"state", state}, {"message", message}};
            if (!cause.empty()) {
                frame["cause"] = cause;
            }
            add_users_roster_clock(frame, clock);
            return UsersRosterFrame{frame.dump(-1, ' ', false, Json::error_handler_t::replace)};
        };
        if (!sub.notice.empty()) {
            auto frame = state_frame("lost", sub.notice, {}, "identity");
            sub = {};
            return frame;
        }
        if (!sub.target) {
            return {};
        }
        const auto& target = *sub.target;
        if (target.loss_revision != sub.loss_revision) {
            clear_users_roster_delivery(sub);
            sub.loss_revision = target.loss_revision;
            sub.state_revision = target.state == "lost" ? target.state_revision : 0;
            if (target.saved_loss_validation == 0 || sub.required_saved_validation <= target.saved_loss_validation) {
                return state_frame("lost", target.loss_message, {}, target.loss_cause);
            }
        }
        if (target.state == "lost") {
            clear_users_roster_delivery(sub);
        }
        const bool awaiting_validation = sub.required_saved_validation > target.saved_validation;
        const auto can_deliver = [&] {
            return roster_is_current(target) && target.loss_revision == sub.loss_revision && target.state != "lost";
        };
        if (!awaiting_validation && !sub.transfer && target.version && target.version != sub.delivered) {
            auto destination = target.version;
            std::shared_ptr<const UsersRosterDelta> delta;
            if (sub.delivered) {
                if (target.delta && target.delta->base_sequence == sub.delivered->sequence) {
                    delta = target.delta;
                } else {
                    // A slow socket only retains its delivered version and the version it is sending.
                    lock.unlock();
                    delta = std::make_shared<const UsersRosterDelta>(compare_users_rosters(*sub.delivered, *destination));
                    lock.lock();
                    if (!can_deliver()) {
                        clear_users_roster_delivery(sub);
                        return {};
                    }
                }
            }
            sub.transfer.emplace(std::move(destination), std::move(delta), ++sub.attempt);
        }
        if (!sub.transfer && sub.state_revision != target.state_revision) {
            if (awaiting_validation && sub.required_saved_validation > target.saved_finished_validation &&
                target.state != "lost" && target.state != "loading") {
                return {};
            }
            sub.state_revision = target.state_revision;
            const auto state = awaiting_validation && target.state == "stale" ? "error" : target.state;
            const auto cause = state == "lost" ? target.loss_cause : std::string_view{};
            return state_frame(state, target.message, current_roster_clock(target), cause);
        }
        if (!sub.transfer) {
            return {};
        }
        if (sub.transfer->will_end()) {
            const auto stale = target.state == "stale" ? std::optional<std::string_view>(target.message) : std::nullopt;
            auto frame = sub.transfer->next(sub.id, current_roster_clock(target), stale);
            if (stale) {
                sub.state_revision = target.state_revision;
            }
            return {std::move(frame), true};
        }
        lock.unlock();
        auto frame = sub.transfer->next(sub.id);
        lock.lock();
        if (!can_deliver()) {
            clear_users_roster_delivery(sub);
            return {};
        }
        return {std::move(frame)};
    }

    UsersRosterPoll read_users_roster(UsersRosterTarget& target, bool force,
                                     const std::shared_ptr<const UsersRosterVersion>& previous)
    {
        using Op = WaterWallUsersOperation;
        const auto current = [&] {
            std::lock_guard lock(mutex_);
            return roster_can_read(target);
        };
        UsersRosterPoll result;
        auto reader = target.reader;
        auto version = std::make_shared<UsersRosterVersion>();
        const auto append = [&](const Json& row) {
            if (!version->insert(row)) {
                result.error = "The user list exceeds Wave's roster size limit.";
                return false;
            }
            return true;
        };
        if (target.scope.live) {
            bool has_status = false;
            {
                std::lock_guard lock(mutex_);
                if (!roster_can_read(target)) {
                    return {.loss = UsersRosterLoss::identity};
                }
                has_status = live_users_access_.at({target.scope.config, target.scope.node}).endpoints.contains(Op::status);
            }
            const auto request = [&](const WaterWallUsersRequest& input) -> WaterWallUsersReply {
                std::vector<WaterWallUsersEndpoint> endpoints;
                pid_t pid;
                {
                    std::lock_guard lock(mutex_);
                    if (!roster_can_read(target)) {
                        return {{{"code", "run_changed"}}, "The live user database scope changed.",
                                WaterWallUsersOutcome::not_sent};
                    }
                    const auto& operations = live_users_access_.at({target.scope.config, target.scope.node}).endpoints;
                    endpoints = operations.at(input.operation);
                    pid = core_->running_pid();
                }
                return request_waterwall_users(pid, endpoints, target.scope.node, input, false, current);
            };
            auto read = reader.read(request, has_status, force, current);
            result.clock = read.clock;
            result.error = std::move(read.error);
            result.loss = read.loss;
            if (!read.rows) {
                return result;
            }
            for (const auto& entry : *read.rows) {
                if (!append(entry.second)) {
                    return result;
                }
            }
        } else {
            const auto read = config_users_.load_roster(target.config_name, target.database);
            if (!read.success) {
                return {.error = read.message,
                        .loss = read.database_unavailable ? std::optional(UsersRosterLoss::identity) : std::nullopt};
            }
            const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            result.clock.wall_time_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, wall));
            for (const auto& row : read.result) {
                if (!append(row)) {
                    return result;
                }
            }
        }
        if (previous) {
            result.delta = compare_and_share_users_rosters(*previous, *version);
        }
        target.reader = std::move(reader);
        result.version = std::move(version);
        return result;
    }

    static void publish_users_roster(UsersRosterTarget& target, std::shared_ptr<UsersRosterVersion> version,
                                     std::optional<UsersRosterDelta> delta)
    {
        if (!version || (delta && delta->changed.empty() && delta->removed.empty())) {
            return;
        }
        version->sequence = ++target.last_sequence;
        target.version = std::move(version);
        target.delta = delta ? std::make_shared<const UsersRosterDelta>(std::move(*delta)) : nullptr;
    }

    static void invalidate_users_roster(UsersRosterTarget& target, const std::string& message,
                                        std::string_view cause = "identity", std::uint64_t saved_validation = 0)
    {
        target.version.reset();
        target.delta.reset();
        target.clock = {};
        target.due = RosterTime::max();
        target.saved_validation = 0;
        target.saved_finished_validation = 0;
        target.saved_loss_validation = saved_validation;
        ++target.loss_revision;
        target.loss_cause = cause;
        target.loss_message = message;
        set_roster_state(target, "lost", message);
    }

    void invalidate_saved_rosters(std::string_view config, std::string_view database, const std::string& message,
                                   const UsersRosterTarget* read_target = nullptr, std::uint64_t validation = 0)
    {
        for (const auto& [scope, target] : roster_targets_) {
            if (!scope.live && target->config_name == config && target->database == database) {
                ++target->saved_write;
                const bool follow_up = target.get() == read_target && target->due != RosterTime::max();
                invalidate_users_roster(*target, message, "identity", follow_up ? validation : 0);
                if (follow_up) {
                    // An entry queued after this read still needs its own validation.
                    set_roster_state(*target, "loading");
                    target->due = RosterTime::min();
                }
            }
        }
    }

    void poll_users_rosters(std::stop_token stop)
    {
        using Clock = std::chrono::steady_clock;
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            std::shared_ptr<UsersRosterTarget> target;
            auto due = RosterTime::max();
            for (const auto& [scope, candidate] : roster_targets_) {
                if (candidate->due < due && !rosters_in_flight_.contains(scope) && roster_can_read(*candidate)) {
                    target = candidate;
                    due = candidate->due;
                }
            }
            const auto started = Clock::now();
            if (!target || due > started) {
                const auto expected = roster_schedule_;
                roster_timer_.wait_until(lock, stop, due, [&] { return roster_schedule_ != expected; });
                continue;
            }

            rosters_in_flight_.insert(target->scope);
            // Keep the scope busy even if its last viewer leaves during the read.
            target->due = RosterTime::max();
            const auto force = std::exchange(target->force, false);
            const auto saved_write = target->saved_write;
            const auto saved_validation = target->scope.live ? 0 : ++target->saved_started_validation;
            auto previous = target->version;
            lock.unlock();
            UsersRosterPoll result;
            try {
                result = read_users_roster(*target, force, previous);
            } catch (const std::exception&) {
                result.error = "Wave could not load the user list. Retry the list.";
            }
            previous.reset();
            const auto finished = Clock::now();
            lock.lock();
            rosters_in_flight_.erase(target->scope);
            wake_roster_workers();
            if (!roster_can_read(*target) || saved_write != target->saved_write) {
                continue;
            }
            if (result.loss) {
                if (target->scope.live) {
                    target->reader = {};
                    const auto cause = *result.loss == UsersRosterLoss::permission ? "permission" : "identity";
                    invalidate_users_roster(*target, result.error, cause);
                } else {
                    invalidate_saved_rosters(target->config_name, target->database, result.error, target.get(),
                                             saved_validation);
                }
                continue;
            }
            target->saved_finished_validation = saved_validation;
            if (!result.error.empty()) {
                set_roster_state(*target, target->version ? "stale" : "error", std::move(result.error));
                if (target->scope.live && target->due == RosterTime::max()) {
                    target->failures = std::min(target->failures + 1, 4u);
                    target->due = finished + users_roster_poll_interval * (1u << target->failures);
                }
                continue;
            }
            target->failures = 0;
            if (result.clock.wall_time_ms) {
                const auto previous_clock = current_roster_clock(*target);
                if (target->scope.live && !result.clock.accounting_time_ms) {
                    result.clock.accounting_time_ms = previous_clock.accounting_time_ms;
                }
                if (previous_clock.wall_time_ms != 0 &&
                    std::max(previous_clock.wall_time_ms, result.clock.wall_time_ms) -
                            std::min(previous_clock.wall_time_ms, result.clock.wall_time_ms) > 1000) {
                    ++target->state_revision;
                }
                target->clock = result.clock;
                target->sampled_at = finished;
            }
            publish_users_roster(*target, std::move(result.version), std::move(result.delta));
            target->saved_validation = saved_validation;
            set_roster_state(*target, "ready");
            if (target->scope.live && target->due == RosterTime::max()) {
                const auto tick = started + users_roster_poll_interval;
                target->due = tick > finished ? tick : finished + users_roster_poll_interval;
            }
        }
    }

    void mark_live_roster_due(int config, const std::string& node)
    {
        if (const auto scope = roster_scope(config, node, true)) {
            const auto found = roster_targets_.find(*scope);
            if (found != roster_targets_.end() && found->second->state != "lost") {
                found->second->due = RosterTime::min();
                wake_roster_workers();
            }
        }
    }

    void publish_saved_roster_change(std::string_view name, std::string_view database, const Json& change)
    {
        const auto& id = change.at("user_id").get_ref<const std::string&>();
        const auto& user = change.at("user");
        for (const auto& [scope, target] : roster_targets_) {
            if (scope.live || target->config_name != name || target->database != database ||
                !roster_can_read(*target)) {
                continue;
            }
            // A file read started before the atomic write cannot replace the newer roster.
            ++target->saved_write;
            if (!target->version || rosters_in_flight_.contains(scope)) {
                target->due = RosterTime::min();
                wake_roster_workers();
            }
            if (!target->version) {
                continue;
            }
            auto version = std::make_shared<UsersRosterVersion>(*target->version);
            UsersRosterDelta delta{target->version->sequence};
            if (user.is_null()) {
                if (version->rows.contains(id)) {
                    version->erase(id);
                    delta.removed.push_back(id);
                }
            } else if (!version->insert(user)) {
                set_roster_state(*target, "stale", "The user list exceeds Wave's roster size limit.");
                continue;
            } else {
                const auto old = target->version->rows.find(id);
                if (old == target->version->rows.end() || *old->second != *version->rows.at(id)) {
                    delta.changed.push_back(id);
                }
            }
            publish_users_roster(*target, std::move(version), std::move(delta));
        }
    }

    WorkspaceReply handle_message(std::string_view message, std::uint64_t& sent_revision,
                                  std::uint64_t& sent_schematic, UsersRosterSubscription* roster,
                                  const std::function<bool()>& session)
    {
        const bool invalid_frame =
            message.size() > max_workspace_action_bytes || message.find('\0') != std::string_view::npos;
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
        const auto live_operation = live_users_operation(action);
        const bool live_write = live_operation && live_users_write(*live_operation);
        const bool too_large =
            bytes > max_request_bytes &&
            !(action == "save_core_config" ||
              (action == "apply_core_settings" && bytes <= core_settings::max_bytes + 256) ||
              (action == "restore_preview_backup" && bytes <= max_preview_restore_request_bytes) ||
              ((action == "create_config_user" || action == "edit_config_user") &&
               bytes <= max_user_request_bytes) ||
              (live_write && bytes <= max_live_user_request_bytes));

        std::unique_lock lock(mutex_);
        if (!session()) {
            return {};
        }
        const bool roster_action = action == "users_roster_subscribe" || action == "users_roster_unsubscribe" ||
                                   action == "users_roster_refresh";
        if (roster && roster_action && !too_large) {
            handle_users_roster(action, request, *roster);
            return {};
        }
        const auto revision = request.find("schematic_revision");
        if (!roster && revision != request.end() && revision->is_number_unsigned()) {
            sent_schematic = revision->get<std::uint64_t>();
        }
        const bool allow_missing_revision =
            schematic_revision_ == 1 && waterwall_version() == bundled_waterwall_version;
        const bool edits_config_registry = action == "set_core_config_enabled" || action == "remove_core_config";
        const bool stale = (uses_core_schematic(action) || (workspace_restored_ && edits_config_registry)) &&
                           (revision == request.end()
                                ? !allow_missing_revision
                                : !revision->is_number_unsigned() || *revision != schematic_revision_);
        const bool changes_workspace = workspace_mutation(action) &&
                                       (uses_core_schematic(action) || edits_config_registry ||
                                        action == "restore_preview_backup");
        std::optional<PanelListenerRestart> listener_restart;
        auto reply = action.empty() ? Reply{false, "Send a JSON panel action."}
                     : too_large    ? Reply{false, "That panel request is too large."}
                     : roster_action ? Reply{false, "The account roster requires a WebSocket connection."}
                     : workspace_recovery_required_ && (changes_workspace || live_write)
                         ? Reply{false, "Restart Wave to finish workspace restore recovery."}
                     : stale        ? Reply{false, "WaterWall changed version. Reload the form and retry."}
                     : live_operation ? live_config_users(*live_operation, request, lock)
                                      : dispatch(action, request, listener_restart);
        if (reply.ok && changes_workspace) {
            pending_backup_.reset();
        }
        if (live_write && reply.result.is_null()) {
            reply.result = {{"outcome", "not_sent"}};
        }
        if (!reply.ok && !reply.feedback.empty() && (live_write || workspace_mutation(action))) {
            log("error", reply.feedback);
            reply.changed = true;
            reply.feedback_in_logs_only = !live_write;
        }
        sync_core();
        if (reply.changed) {
            ++revision_;
        }
        auto frame = snapshot(roster != nullptr);
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
        return {frame.dump(-1, ' ', false, Json::error_handler_t::replace), std::move(listener_restart)};
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
        if (log_panel_events_) {
            auto line = message;
            std::ranges::replace_if(line, [](unsigned char ch) { return ch < ' ' || ch == 0x7f; }, ' ');
            std::osyncstream(std::cerr) << '[' << level << "] " << line << '\n';
        }
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
            live_users_access_.clear();
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

    Reply apply_tls_settings(const Json& input, std::optional<PanelListenerRestart>& activation)
    {
        if (listener_restart_pending_) {
            return {false, "The panel listener is already restarting."};
        }
        const auto values = input.find("settings");
        if (values == input.end() || !values->is_object()) {
            return {false, "Choose whether to enable panel HTTPS."};
        }
        const auto enabled = values->find("enabled");
        if (enabled == values->end() || !enabled->is_boolean()) {
            return {false, "Choose whether to enable panel HTTPS."};
        }
        const auto activate = input.find("activate");
        if (activate != input.end() && !activate->is_boolean()) {
            return {false, "Choose whether to activate HTTPS after saving."};
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

        auto active = database_->settings();
        const bool schedule = selected && !active.tls && activate != input.end() && *activate == true;
        std::unique_ptr<httplib::Server> listener;
        try {
            const auto saved = database_->save_tls_settings(selected, schedule ? &listener : nullptr);
            data_["settings"]["tls"] = panel_tls_settings(saved, active.tls);
            if (schedule) {
                active.tls = std::move(selected);
                activation.emplace(prepare_restart_(std::move(listener), std::move(active)));
            }
        } catch (const std::runtime_error& error) {
            if (schedule) {
                database_->finish_listener_restart(std::nullopt);
            }
            return {false, error.what()};
        }
        if (schedule) {
            const auto url = "https://" + tls_authority(activation->settings.tls->domain, activation->port) +
                database_->panel_base_path() + '/';
            listener_restart_pending_ = true;
            auto reply = complete("Saved panel TLS settings. Restarting the panel on HTTPS.");
            reply.result = {{"https_url", std::move(url)}};
            return reply;
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
        auto timestamp = format_utc_timestamp(now);
        if (timestamp.empty()) {
            return {false, "Could not read the UTC date for the donation response."};
        }
        try {
            database_->save_donation(*answer, timestamp);
        } catch (const std::runtime_error&) {
            return {false, "Could not save the donation response in wave-db.json."};
        }
        donation_ = Donation{*answer, std::move(timestamp), now};
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
            !saved_configs->is_array() || saved_configs->size() > max_workspace_configs) {
            return invalid;
        }

        auto& current = data_["server"]["core"]["configs"];
        auto configs = Json::array();
        for (const auto& config : current) {
            if (config["active"] == true) {
                configs.push_back(config);
            }
        }
        if (configs.size() + saved_configs->size() > max_workspace_configs) {
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
            if (existing == current.end()) {
                configs.push_back({{"id", next_id++}, {"name", name}, {"enabled", *enabled}, {"active", false}});
            } else {
                configs.push_back(*existing);
                configs.back()["enabled"] = *enabled;
            }
        }

        std::vector<std::string_view> created;
        std::map<int, CoreConfigDocument> restored_graphs;
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
            restored_graphs.emplace(config["id"].get<int>(), config_files_.read(name));
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
        for (auto& config : configs) {
            if (const auto graph = restored_graphs.find(config["id"].get<int>()); graph != restored_graphs.end()) {
                update_read_config_tabs(config, graph->second);
            }
        }
        const auto removed = [&](const auto& entry) {
            return std::ranges::none_of(configs, [&](const Json& config) { return config["id"] == entry.first; });
        };
        std::erase_if(offline_users_bindings_, removed);
        std::erase_if(offline_users_config_errors_, removed);
        current = std::move(configs);
        next_config_id_ = next_id;
        return complete("Restored sample TLS settings and the inactive config list.");
    }

    Reply change_core_state(std::string_view action)
    {
        if (shutting_down_) {
            return {false, "Wave is shutting down."};
        }
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
        for (auto& config : configs) {
            if (config["enabled"] == true) {
                const auto& name = config["name"].get_ref<const std::string&>();
                if (!config_files_.available(name)) {
                    if (unverify_saved_binding(config["id"].get<int>(), "The config file is missing or unsafe.")) {
                        ++revision_;
                    }
                    return {false, "Configuration file is missing or unsafe for " + name + '.'};
                }
                auto file = config_files_.read(name);
                if (update_read_config_tabs(config, file)) {
                    ++revision_;
                }
                const auto id = config.at("id").get<int>();
                if (const auto bindings = offline_users_bindings_.find(id);
                    !offline_users_config_errors_.contains(id) && bindings != offline_users_bindings_.end()) {
                    for (const auto& [node, binding] : bindings->second) {
                        if (!binding.database.empty()) {
                            bool created;
                            if (auto error = config_users_.prepare(name, binding.database, created); !error.empty()) {
                                return {false, name + ": " + std::move(error)};
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
        resolve_live_users_access();
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
        if (std::ranges::find(available, target, schematic_text) == available.end()) {
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
        pending_backup_.reset();
        publish_core_release(std::move(next));
        auto& status = data_["server"]["core"];
        for (auto& config : status["configs"]) {
            const auto& name = config["name"].get_ref<const std::string&>();
            update_read_config_tabs(config, config_files_.read(name));
        }
        log("success", "Selected WaterWall " + std::string(waterwall_version()) + '.');
        sync_core();
        return {};
    }

    void publish_core_release(CoreUpdate next)
    {
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
        next_catalog_check_ = {};
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
        const auto management = input.find("allow_live_user_management");
        if (management != input.end() && !management->is_boolean()) {
            return {false, "Choose whether to allow live user management."};
        }
        const bool add_starter = add_auth_nodes != input.end() && *add_auth_nodes == true;
        const bool allow_management = management == input.end() ? add_starter : management->get<bool>();
        if (allow_management && (!add_starter || users_starter_management_slot(waterwall_schematic()).empty())) {
            return {false, "Live user management is unavailable for this starter."};
        }

        auto& configs = data_["server"]["core"]["configs"];
        if (configs.size() >= max_workspace_configs || next_config_id_ == std::numeric_limits<int>::max()) {
            return {false, "The config list is full."};
        }
        if (std::ranges::any_of(configs, [&](const Json& config) { return config["name"] == name; })) {
            return {false, "A config already uses that name."};
        }
        Json config;
        if (add_starter) {
            if (!schematic_field(waterwall_schematic().at("config"), "users_starter").is_object()) {
                return {false, "This WaterWall schematic does not define user starter nodes."};
            }
            std::vector<Json> existing;
            existing.reserve(configs.size() + live_configs_.size());
            for (auto& row : configs) {
                auto file = config_files_.read(row["name"].get_ref<const std::string&>());
                if (update_read_config_tabs(row, file)) {
                    ++revision_;
                }
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
            config = make_users_starter_config(name, *port, allow_management);
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
        } else {
            config = make_empty_core_config(name);
        }
        auto changed = configs;
        changed.push_back({{"id", next_config_id_}, {"name", name}, {"enabled", false}, {"active", false}});
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
        update_config_tabs(changed.back(), config);
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
            offline_users_bindings_.erase(id->get<int>());
            offline_users_config_errors_.erase(id->get<int>());
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

    bool unverify_saved_binding(int config, const std::string& error)
    {
        if (const auto found = offline_users_config_errors_.find(config);
            found != offline_users_config_errors_.end() && found->second == error) {
            return false;
        }
        offline_users_config_errors_.insert_or_assign(config, error);
        for (const auto& [scope, target] : roster_targets_) {
            if (!scope.live && scope.config == config) {
                ++target->saved_write;
                invalidate_users_roster(*target, error, "config_unverified");
            }
        }
        return true;
    }

    bool update_read_config_tabs(Json& row, const CoreConfigDocument& document)
    {
        if (!document.error.empty()) {
            return unverify_saved_binding(row["id"].get<int>(), document.error);
        }
        return update_config_tabs(row, document.config);
    }

    bool update_config_tabs(Json& row, const Json& config)
    {
        const int id = row["id"].get<int>();
        const auto& nodes = schematic_field(config, "nodes");
        if (!nodes.is_array()) {
            return unverify_saved_binding(id, "The config must contain a nodes array.");
        }
        const auto& config_name = row["name"].get_ref<const std::string&>();
        OfflineUsersBindings bindings;
        std::set<std::string_view> seen;
        auto names = Json::array();
        for (const auto& node : nodes) {
            const auto name = schematic_text(schematic_field(node, "name"));
            const auto type = schematic_text(schematic_field(node, "type"));
            if (!valid_core_node_name(name) || !valid_core_node_name(type)) {
                return unverify_saved_binding(id, "Each node must have a valid name and type.");
            }
            if (!seen.insert(name).second) {
                return unverify_saved_binding(id, "Duplicate node name: " + std::string(name));
            }
            const auto& definition = waterwall_node(type);
            if (schematic_field(definition, "user_database") == true) {
                const auto& settings = schematic_field(node, "settings");
                if (node.contains("settings") && !settings.is_object()) {
                    return unverify_saved_binding(id,
                        "The settings for " + std::string(name) + " must be an object.");
                }
                const auto key = schematic_text(schematic_field(definition, "database_key"));
                if (const auto path = settings.find(key); path != settings.end() && schematic_text(*path).empty()) {
                    return unverify_saved_binding(id,
                        "The database path for " + std::string(name) + " must be a nonempty string.");
                }
                names.push_back(name);
                auto database = auth_database_name(node, config_name, database_->directory());
                const auto* previous = offline_users_binding(id, name);
                const auto generation = previous && previous->database == database ? previous->generation
                                                                                   : ++offline_users_generation_;
                bindings.emplace(name, OfflineUsersBinding{std::move(database), generation});
            }
        }

        const bool live = has_waterwall_users_api(config);
        bool changed = offline_users_config_errors_.erase(id) != 0 || row.contains("live_users") != live;
        if (live) {
            row["live_users"] = true;
        } else {
            row.erase("live_users");
        }
        if (bindings.empty()) {
            changed |= offline_users_bindings_.erase(id) != 0;
        } else if (const auto previous = offline_users_bindings_.find(id);
                   previous == offline_users_bindings_.end() || previous->second != bindings) {
            offline_users_bindings_.insert_or_assign(id, std::move(bindings));
            changed = true;
        }
        if (names.empty()) {
            return row.erase("auth_nodes") != 0 || changed;
        }
        if (!row.contains("auth_nodes") || row["auth_nodes"] != names) {
            row["auth_nodes"] = std::move(names);
            changed = true;
        }
        return changed;
    }

    const OfflineUsersBinding* offline_users_binding(int config, std::string_view node) const
    {
        const auto found = offline_users_bindings_.find(config);
        if (found == offline_users_bindings_.end()) {
            return nullptr;
        }
        const auto binding = found->second.find(node);
        return binding == found->second.end() ? nullptr : &binding->second;
    }

    std::string users_run_id(int config, std::string_view node) const
    {
        return users_session_id_ + '/' + std::to_string(live_run_) + '/' + std::to_string(config) + '/' +
               std::string(node);
    }

    std::string offline_users_run_id(int config, std::string_view node, const OfflineUsersBinding& binding) const
    {
        return users_run_id(config, node) + '/' + std::to_string(binding.generation);
    }

    void resolve_live_users_access()
    {
        live_users_access_.clear();
        for (const auto& [id, config] : live_configs_) {
            const auto nodes = config_auth_nodes(config);
            if (nodes.empty()) {
                continue;
            }
            const auto listeners = waterwall_users_listeners(live_configs_, id);
            const auto logical_name = schematic_text(schematic_field(config, "name"));
            for (const auto& value : nodes) {
                const auto& node = value.get_ref<const std::string&>();
                auto& access = live_users_access_[{id, node}];
                if (const auto* binding = offline_users_binding(id, node)) {
                    access.database_generation = binding->generation;
                }
                bool ambiguous_port = false;
                for (const auto operation : waterwall_users_operations) {
                    auto candidates =
                        waterwall_users_endpoints(listeners, logical_name, node, operation, ambiguous_port);
                    if (!candidates.empty()) {
                        access.endpoints.emplace(operation, std::move(candidates));
                    }
                }
                if (access.endpoints.empty()) {
                    if (ambiguous_port) {
                        access.reason = "Use a dedicated listener port for the WaterWall API.";
                    } else if (!listeners.empty()) {
                        access.reason = "Grant the local APIServer client an operation for this config and node, "
                                        "then restart WaterWall.";
                    } else {
                        access.reason = "Configure a loopback TcpListener connected to an APIServer, "
                                        "then restart WaterWall.";
                    }
                }
            }
        }
    }

    Json users_access(int config, const std::string& node, std::string_view state) const
    {
        Json operations = Json::array();
        std::string_view source = "blocked";
        std::string reason;
        std::uint64_t database_generation = 0;
        if (state == "starting" || state == "stopping") {
            source = "transition";
            reason = "WaterWall is " + std::string(state) + ". User actions will resume when it finishes.";
        } else if (state != "running") {
            source = "offline";
        } else if (const auto access = live_users_access_.find({config, node});
                   access != live_users_access_.end()) {
            reason = access->second.reason;
            if (!access->second.endpoints.empty()) {
                source = "live";
                database_generation = access->second.database_generation;
                for (const auto& [operation, _] : access->second.endpoints) {
                    operations.push_back(waterwall_users_operation_name(operation));
                }
            }
        } else {
            reason = "This config is inactive. Stop WaterWall to use the saved database.";
        }
        auto run_id = users_run_id(config, node);
        Json access{{"source", source}, {"reason", std::move(reason)}, {"operations", std::move(operations)}};
        if (source == "offline") {
            if (const auto error = offline_users_config_errors_.find(config);
                error != offline_users_config_errors_.end()) {
                access["binding_error"] = error->second;
            }
            if (const auto* binding = offline_users_binding(config, node)) {
                run_id = offline_users_run_id(config, node, *binding);
                database_generation = binding->generation;
            }
        }
        if (database_generation != 0) {
            access["database_generation"] = users_session_id_ + '/' + std::to_string(database_generation);
        }
        access["run_id"] = std::move(run_id);
        return access;
    }

    Reply live_config_users(WaterWallUsersOperation operation, const Json& input,
                            std::unique_lock<std::mutex>& lock)
    {
        using Op = WaterWallUsersOperation;
        const auto id = input.find("config_id");
        const auto target = input.find("node");
        if (id == input.end() || !integer_value(*id, 1, std::numeric_limits<int>::max()) ||
            target == input.end() || !text_value(*target, 128, true)) {
            return {false, "Choose a config and user database node."};
        }
        WaterWallUsersRequest request{.operation = operation};
        if (operation == Op::list) {
            const auto cursor = input.find("cursor");
            if (cursor != input.end() &&
                (!text_value(*cursor, 64) ||
                 !valid_waterwall_users_cursor(request.cursor = cursor->get_ref<const std::string&>()))) {
                return {false, "Choose a valid WaterWall users page.", false, {{"reset_cursor", true}}};
            }
        }
        const auto config = id->get<int>();
        const auto& node = target->get_ref<const std::string&>();
        const bool legacy_list = operation == Op::list && !input.contains("source") && !input.contains("run_id");
        const auto legacy_unavailable = [] {
            return Reply{true,
                         {},
                         false,
                         {{"available", false}, {"users", Json::array()}, {"total", 0}, {"next_cursor", nullptr}}};
        };
        if (!legacy_list && (schematic_field(input, "source") != "live" ||
                             schematic_field(input, "run_id") != users_run_id(config, node))) {
            return {false, "The user tab's source changed. Review the current account before retrying."};
        }
        const auto launched = live_configs_.find(config);
        const auto state = core_->state();
        if (state != "running" || launched == live_configs_.end() ||
            (legacy_list && !has_waterwall_users_api(launched->second))) {
            if (legacy_list) {
                return legacy_unavailable();
            }
            auto reason = users_access(config, node, state).value("reason", std::string{});
            return {false, reason.empty() ? "The live user database is unavailable while WaterWall is stopped."
                                          : std::move(reason)};
        }
        const auto access = live_users_access_.find({config, node});
        if (access == live_users_access_.end()) {
            return {false, "That user database node is unavailable."};
        }
        const auto found = access->second.endpoints.find(operation);
        if (found == access->second.endpoints.end()) {
            const auto& reason = access->second.reason;
            if (legacy_list) {
                const auto& help = schematic_field(waterwall_schematic().at("ui").at("users"), "live_setup_help");
                return {false, reason.empty() ? std::string(schematic_text(help)) : reason};
            }
            return {false, reason.empty() ? "This APIServer does not grant that operation for this node." : reason,
                    false, {{"code", "permission_denied"}, {"outcome", "not_sent"}}};
        }
        const auto read_text = [&](std::string_view key, std::string& out) {
            const auto value = input.find(key);
            if (value == input.end()) {
                return true;
            }
            if (!value->is_string()) {
                return false;
            }
            out = value->get_ref<const std::string&>();
            return true;
        };
        if ((operation == Op::get || live_users_write(operation)) && !read_text("user_id", request.id)) {
            return {false, "Choose a valid account ID."};
        }
        if (operation == Op::create || operation == Op::update) {
            const auto fields = input.find("user");
            if (fields == input.end() || !fields->is_object()) {
                return {false, "Supply account fields for that operation."};
            }
            request.fields = *fields;
        }
        if (operation == Op::create) {
            if (const auto embedded = request.fields.find("id"); embedded != request.fields.end()) {
                if (!embedded->is_string() ||
                    (input.contains("user_id") && request.id != schematic_text(*embedded))) {
                    return {false, "Supply one valid account ID. The two ID fields must agree when both are present."};
                }
                request.id = embedded->get_ref<const std::string&>();
                request.fields.erase(embedded);
            }
            const auto password = request.fields.find("password");
            if (password == request.fields.end() || !password->is_string()) {
                return {false, "Enter a password for the AuthenticationServer user."};
            }
            request.password = std::move(password->get_ref<std::string&>());
            request.fields.erase(password);
        }
        if (operation == Op::update || operation == Op::password) {
            if (!read_text("if_match", request.if_match) || !valid_waterwall_user_edit_token(request.if_match)) {
                return {false, "Read the current account before submitting this change."};
            }
        }
        if (operation == Op::password && !read_text("password", request.password)) {
            return {false, "Enter a new password for the AuthenticationServer user."};
        }
        if ((operation == Op::get || live_users_write(operation)) && !valid_waterwall_user_id(request.id)) {
            return {false, "Choose a nonzero account ID in the unsigned 64-bit range."};
        }

        const auto endpoints = found->second;
        const auto run = live_run_;
        const auto pid = core_->running_pid();
        const auto same_run = [this, run, pid] {
            return live_run_ == run && core_->state() == "running" && core_->running_pid() == pid;
        };
        // Keep lifecycle actions and host sampling independent of the API's network deadline.
        lock.unlock();
        auto reply = request_waterwall_users(pid, endpoints, node, request, legacy_list, [&] {
            std::lock_guard guard(mutex_);
            return same_run();
        });
        lock.lock();
        using Outcome = WaterWallUsersOutcome;
        const bool write = live_users_write(operation);
        if (!same_run()) {
            if (legacy_list) {
                return legacy_unavailable();
            }
            if (!write) {
                return {false, "WaterWall changed run. Reload this user tab."};
            }
            if (reply.outcome != Outcome::not_sent) {
                reply = {{}, "WaterWall changed run before Wave could confirm the change. The outcome is unknown.",
                         Outcome::unknown};
            }
        }
        if (write) {
            std::string_view outcome;
            switch (reply.outcome) {
            case Outcome::applied: outcome = "applied"; break;
            case Outcome::conflict: outcome = "conflict"; break;
            case Outcome::unknown: outcome = "unknown"; break;
            case Outcome::rejected: outcome = "rejected"; break;
            case Outcome::not_sent: outcome = "not_sent"; break;
            }
            reply.result["outcome"] = outcome;
            reply.result["user_id"] = request.id;
            if (reply.outcome == Outcome::applied) {
                mark_live_roster_due(config, node);
                auto applied = complete("Change applied to the running WaterWall core.");
                applied.result = std::move(reply.result);
                return applied;
            }
        }
        return {reply.outcome == Outcome::applied, std::move(reply.error), false, std::move(reply.result)};
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
        const bool changed = update_read_config_tabs(*row, document);
        if (!document.error.empty()) {
            return {false, std::move(document.error), changed};
        }
        if (action == "get_core_config") {
            return {true,
                    {},
                    changed,
                    {{"config", std::move(document.config)},
                     {"revision", std::move(document.revision)},
                     {"editable", !active}}};
        }
        if (action == "save_core_config") {
            if (active) {
                return {false, "Stop WaterWall before editing this config.", changed};
            }
            auto reply = save_config(input, *row, name, document);
            reply.changed |= changed;
            return reply;
        }
        if (const auto error = offline_users_config_errors_.find(id->get<int>());
            error != offline_users_config_errors_.end()) {
            return {false, error->second, changed};
        }
        auto reply = edit_config_users(action, input, id->get<int>(), name);
        reply.changed |= changed;
        return reply;
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
        static const OfflineUsersBindings empty_bindings;
        // A repair may be based on a readable document with malformed nodes.
        const auto previous = offline_users_bindings_.find(row["id"].get<int>());
        const auto& bindings = previous == offline_users_bindings_.end() ? empty_bindings : previous->second;
        if (auto error = configure_auth_databases(config, bindings, name, database_->directory());
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

    Reply edit_config_users(std::string_view action, const Json& input, int config, std::string_view name)
    {
        const auto target = input.find("node");
        if (target == input.end() || !text_value(*target, 128, true)) {
            return {false, "Choose a user database node."};
        }
        const auto& node = target->get_ref<const std::string&>();
        const auto* binding = offline_users_binding(config, node);
        if ((action == "get_config_user" || action == "reset_config_user_traffic" ||
             schematic_field(input, "roster") == true || input.contains("source") || input.contains("run_id")) &&
            (!binding || schematic_field(input, "source") != "offline" ||
             schematic_field(input, "run_id") != offline_users_run_id(config, node, *binding))) {
            return {false, "The user database scope changed. Reopen the account before trying again."};
        }
        if (!binding) {
            return {false, "That user database node is unavailable."};
        }
        if (binding->database.empty()) {
            return {false, "Save the node with a database in its config folder before managing its users."};
        }
        const bool watch_saved = workspace_mutation(action) && std::ranges::any_of(roster_targets_, [&](const auto& entry) {
            const auto& [scope, target] = entry;
            return !scope.live && target->config_name == name && target->database == binding->database;
        });
        auto result = config_users_.execute(action, name, binding->database, input, watch_saved);
        if (result.database_unavailable) {
            invalidate_saved_rosters(name, binding->database, result.message);
        }
        if (result.success && workspace_mutation(action)) {
            if (result.roster_change) {
                publish_saved_roster_change(name, binding->database, *result.roster_change);
            }
            auto reply = complete(std::move(result.message));
            reply.result = std::move(result.result);
            return reply;
        }
        return {result.success, std::move(result.message), false, std::move(result.result)};
    }

    Reply dispatch(std::string_view action, const Json& input, std::optional<PanelListenerRestart>& listener_restart)
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
            action == "get_config_user" || action == "get_config_user_password" || action == "create_config_user" ||
            action == "edit_config_user" || action == "delete_config_user" || action == "reset_config_user_traffic") {
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
            return apply_tls_settings(input, listener_restart);
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
    std::condition_variable_any roster_timer_;
    LinuxMetrics host_;
    std::shared_ptr<PanelDatabase> database_;
    PreparePanelRestart prepare_restart_;
    CoreConfigFiles config_files_;
    CoreConfigUsers config_users_;
    std::unique_ptr<WaterWall> core_;
    WaterWallLogs core_logs_;
    std::filesystem::path waterwall_bin_;
    bool log_panel_events_;
    std::optional<CoreUpdate> core_update_;
    std::optional<PendingWorkspaceBackup> pending_backup_;
    bool workspace_recovery_required_ = false;
    bool workspace_restored_ = false;
    std::chrono::steady_clock::time_point next_catalog_check_{};
    std::future<CoreCatalogResult> catalog_result_;
    std::jthread catalog_worker_;
    bool manual_catalog_check_ = false;
    bool queued_catalog_check_ = false;
    bool restart_pending_ = false;
    bool listener_restart_pending_ = false;
    bool shutting_down_ = false;
    std::string restart_error_;
    std::map<int, Json> live_configs_;
    std::map<std::pair<int, std::string>, LiveUsersAccess> live_users_access_;
    std::map<int, OfflineUsersBindings> offline_users_bindings_;
    std::map<int, std::string> offline_users_config_errors_;
    std::uint64_t offline_users_generation_ = 0;
    const std::string users_session_id_ = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t live_run_ = 0;
    Json data_ = make_data();
    Json saved_core_settings_;
    std::optional<Donation> donation_;
    int next_config_id_ = 1;
    std::uint64_t revision_ = 1;
    std::uint64_t schematic_revision_ = 1;
    std::uint64_t roster_schedule_ = 0;
    std::map<UsersRosterScope, std::shared_ptr<UsersRosterTarget>> roster_targets_;
    std::set<UsersRosterScope> rosters_in_flight_;
    std::future<PanelUpdateCheck> panel_update_result_;
    std::jthread panel_update_worker_;
    std::jthread sampler_;
    std::array<std::jthread, 4> roster_workers_;
};

void backup_json(httplib::Response& response, const Json& result)
{
    response.set_content(result.dump(), "application/json");
}

void backup_error(httplib::Response& response, int status, std::string_view error)
{
    response.status = status;
    backup_json(response, {{"ok", false}, {"error", error}});
}

template <typename Action>
void backup_request(const std::shared_ptr<AuthStore>& store, const httplib::Request& request,
                    httplib::Response& response, Action&& action)
{
    auto session = authenticated_session_id(store, request);
    if (session.empty()) {
        backup_error(response, 401, "Sign in to use workspace backups.");
        return;
    }
    try {
        action(std::move(session));
    } catch (const std::runtime_error& error) {
        backup_error(response, 400, error.what());
    } catch (const std::exception&) {
        backup_error(response, 400, "Could not process the workspace backup.");
    }
}

void configure_backup_routes(httplib::Server& server, const std::shared_ptr<AuthStore>& store,
                             const std::shared_ptr<Preview>& preview, const RestartPanel& restart_panel)
{
    server.Get(panel_route(backup_download_route), [preview, store](const httplib::Request& req, httplib::Response& res) {
        backup_request(store, req, res, [&](const std::string& session) {
            auto archive = preview->download_workspace_backup();
            if (authenticated_session_id(store, req) != session) {
                backup_error(res, 401, "Sign in to use workspace backups.");
                return;
            }
            res.set_header("Content-Disposition", "attachment; filename=\"wave-backup.zip\"");
            res.set_content(std::move(archive), "application/zip");
        });
    });
    auto uploads = std::make_shared<std::mutex>();
    server.Post(panel_route(backup_prepare_route),
                [preview, store, uploads](const httplib::Request& req, httplib::Response& res,
                                          const httplib::ContentReader& read) {
                    std::unique_lock lock(*uploads, std::try_to_lock);
                    if (!lock) {
                        backup_error(res, 409, "Wait for the current workspace backup upload to finish.");
                        return;
                    }
                    std::string archive;
                    bool too_large = false;
                    if (!read([&](const char* data, std::size_t size) {
                            too_large = size > max_backup_archive_bytes - archive.size();
                            if (too_large) {
                                return false;
                            }
                            archive.append(data, size);
                            return true;
                        })) {
                        const auto status = too_large || res.status == 413 ? 413 : 400;
                        backup_error(res, status, status == 413 ? "The workspace backup exceeds 128 MiB."
                                                                 : "Could not read the workspace backup upload.");
                        return;
                    }
                    backup_request(store, req, res, [&](std::string session) {
                        backup_json(res, preview->stage_workspace_backup(archive, std::move(session)));
                    });
                });
    post_panel_json(server, panel_route(backup_apply_route),
                    [preview, store, restart_panel](const httplib::Request& req, httplib::Response& res,
                                                    std::string_view body) {
                        backup_request(store, req, res, [&](const std::string& session) {
                            std::string error;
                            const auto input = parse_plain_core_config_json(body, "Invalid restore request.", error);
                            const auto token = schematic_text(schematic_field(input, "token"));
                            if (!error.empty() || token.size() != 32) {
                                throw std::runtime_error("Choose a validated backup before restoring it.");
                            }
                            try {
                                auto reply = preview->restore_workspace_backup(token, session, *store);
                                backup_json(res, reply.data);
                                if (reply.listener_restart) {
                                    res.set_header("Connection", "close");
                                    restart_panel(std::move(*reply.listener_restart));
                                }
                            } catch (const WorkspaceBackupSessionEnded& error) {
                                backup_error(res, 401, error.what());
                            } catch (const WorkspaceBackupBusy& error) {
                                res.status = 409;
                                backup_json(res, {{"ok", false}, {"error", error.what()}, {"retryable", true}});
                            }
                        });
                    });
}

std::function<bool()> workspace_http_session(const std::shared_ptr<AuthStore>& store,
                                            const std::shared_ptr<PanelDatabase>& database,
                                            const httplib::Request& request)
{
    const auto expected = request.get_header_value("X-Wave-Session");
    if (request.get_header_value_count("X-Wave-Session") != 1 || expected.empty() ||
        expected != authenticated_session_id(store, request)) {
        return [] { return false; };
    }
    return websocket_session(store, database, request);
}

std::optional<std::uint64_t> workspace_schematic_revision(const httplib::Request& request)
{
    const auto count = request.get_param_value_count("schematic_revision");
    std::uint64_t revision = 0;
    if (count == 0) {
        return revision;
    }
    const auto text = request.get_param_value("schematic_revision");
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, revision);
    return count == 1 && parsed.ec == std::errc{} && parsed.ptr == end ? std::optional(revision) : std::nullopt;
}

void send_workspace_http(httplib::Response& response, std::string payload, const std::function<bool()>& session)
{
    if (payload.empty() || !session()) {
        response.status = 401;
        return;
    }
    response.set_content(std::move(payload), "application/json");
}

void configure_workspace_http_routes(httplib::Server& server, const std::shared_ptr<Preview>& preview,
                                     const std::shared_ptr<AuthStore>& store,
                                     const std::shared_ptr<PanelDatabase>& database,
                                     const RestartPanel& restart_panel)
{
    const auto route = panel_route(workspace_http_route);
    server.Get(route, [preview, store, database](const httplib::Request& req, httplib::Response& res) {
        const auto session = workspace_http_session(store, database, req);
        if (!session()) {
            res.status = 401;
            return;
        }
        const auto revision = workspace_schematic_revision(req);
        if (!revision || req.get_param_value_count("workspace_id") > 1) {
            res.status = 400;
            return;
        }
        send_workspace_http(res, preview->http_snapshot(req.get_param_value("workspace_id"), *revision, session),
                            session);
    });
    post_panel_json(server, route,
                    [preview, store, database, restart_panel](const httplib::Request& req, httplib::Response& res,
                                                             std::string_view body) {
                        const auto session = workspace_http_session(store, database, req);
                        if (!session()) {
                            res.status = 401;
                            return;
                        }
                        if (req.get_header_value_count("X-Wave-Workspace") != 1 ||
                            req.get_header_value("X-Wave-Workspace") != preview->workspace_id()) {
                            res.status = 409;
                            return;
                        }
                        auto reply = preview->http_action(body, session);
                        send_workspace_http(res, std::move(reply.payload), session);
                        if (reply.listener_restart) {
                            restart_panel(std::move(*reply.listener_restart));
                        }
                    },
                    max_workspace_action_bytes);
}

} // namespace

PreviewRoutes make_preview_routes(std::shared_ptr<AuthStore> store, std::shared_ptr<PanelDatabase> database,
                                  PreparePanelRestart prepare_restart, RestartPanel restart_panel,
                                  bool log_panel_events)
{
    const auto preview = std::make_shared<Preview>(database, std::move(prepare_restart), log_panel_events);
    return {
        [preview, store = std::move(store), database = std::move(database), restart_panel = std::move(restart_panel)](
            httplib::Server& server) {
            configure_backup_routes(server, store, preview, restart_panel);
            configure_workspace_http_routes(server, preview, store, database, restart_panel);
            server.WebSocket(panel_route("/ws/overview"),
                             [preview, store, database](const httplib::Request& req, httplib::ws::WebSocket& socket) {
                                 if (!websocket_session(store, database, req)()) {
                                     socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                                     return;
                                 }
                                 if (socket.send(preview->overview())) {
                                     socket.close();
                                 }
                             });
            server.WebSocket(panel_route("/ws/workspace"),
                             [preview, store, database, restart_panel](const httplib::Request& req,
                                                                     httplib::ws::WebSocket& socket) {
                                 preview->serve(socket, websocket_session(store, database, req), restart_panel);
                             });
        },
        [preview](bool applied) { preview->finish_listener_restart(applied); },
        [preview] { preview->request_shutdown(); },
    };
}

} // namespace wave
