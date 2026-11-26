#include "preview.h"

#include "auth.h"
#include "core_config_files.h"
#include "core_config_graph.h"
#include "core_config_name.h"
#include "core_config_users.h"
#include "core_settings.h"
#include "linux_metrics.h"
#include "panel_settings.h"
#include "preview_backup.h"
#include "waterwall.h"
#include "waterwall_logs.h"
#include "waterwall_users.h"
#include "wave_version.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr auto host_sample_interval = 500ms;
constexpr std::size_t max_request_bytes = 8192;
constexpr std::size_t max_user_request_bytes = 32 * 1024;
constexpr auto max_frame_bytes = max_core_config_bytes + 1024;
constexpr std::size_t max_core_configs = 64;

bool core_in_use(std::string_view state)
{
    return state == "starting" || state == "running" || state == "stopping";
}

Json make_core()
{
    return {{"capabilities", {{"lifecycle", true}, {"configs", true}}}, {"configs", Json::array()}};
}

Json make_server()
{
    return {
        {"version", version}, {"available_panel_versions", Json::array({version})}, {"core", make_core()}};
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

struct Reply {
    bool ok;
    std::string feedback;
    bool changed = false;
    Json result = nullptr;
    bool feedback_in_logs_only = false;
};

class Preview {
  public:
    explicit Preview(std::shared_ptr<PanelDatabase> database)
        : database_(std::move(database)), config_files_(database_->directory()),
          config_users_(database_->directory()), core_(database_->directory()),
          core_logs_(database_->directory())
    {
        load_core_settings();
        if (database_->claim_waterwall_install()) {
            core_.install_if_missing();
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
        if (!session() || !socket.send(changed_snapshot(sent_revision))) {
            return;
        }
        std::string message;
        while (const auto result = socket.read(message)) {
            if (!session()) {
                socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                return;
            }
            const auto frame = result == httplib::ws::Timeout
                                   ? changed_snapshot(sent_revision)
                                   : handle_message(result == httplib::ws::Text ? std::string_view(message)
                                                                                : std::string_view{},
                                                    sent_revision);
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
    void load_core_settings()
    {
        const auto& stored = database_->initial_contents();
        auto normalized = core_settings::normalize(stored.value("core_settings", Json::object()));
        if (!normalized.issue.empty()) {
            data_["core_settings_notice"] =
                "Review the settings shown here before saving. Saving replaces the previous core settings. " +
                normalized.issue;
        }
        data_["core_settings"] = std::move(normalized.settings);

        const auto saved_configs = stored.value("core_configs", Json::array());
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
        return {{"type", "snapshot"}, {"data", data_}};
    }

    std::string changed_snapshot(std::uint64_t& sent_revision)
    {
        std::lock_guard lock(mutex_);
        if (revision_ == sent_revision) {
            return {};
        }
        auto frame = snapshot().dump(-1, ' ', false, Json::error_handler_t::replace);
        sent_revision = revision_;
        return frame;
    }

    std::string handle_message(std::string_view message, std::uint64_t& sent_revision)
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
        auto reply = action.empty()                      ? Reply{false, "Send a JSON panel action."}
                     : too_large                         ? Reply{false, "That panel request is too large."}
                     : action == "get_live_config_users" ? get_live_config_users(request, lock)
                                                         : dispatch(action, request);
        sync_core();
        if (reply.changed) {
            ++revision_;
        }
        auto frame = snapshot();
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
        core_.update();
        for (const auto& entry : core_logs_.read()) {
            add_log(entry.source, entry.timestamp.substr(11, 8), entry.level, entry.message);
        }
        if (restart_pending_ && !core_in_use(core_.state())) {
            restart_pending_ = false;
            const auto reply = start_core();
            if (!reply.ok) {
                restart_error_ = "Could not restart WaterWall. " + reply.feedback;
                log("error", restart_error_);
            }
        }
        sync_core();
    }

    void sync_core()
    {
        auto& core = data_["server"]["core"];
        const auto state = core_.state();
        core["state"] = state;
        if (!core_in_use(state)) {
            core_logs_.stop();
            live_configs_.clear();
            for (auto& config : core["configs"]) {
                config["active"] = false;
            }
        }
        const auto& error = core_error();
        if (error.empty()) {
            core.erase("error");
        } else {
            core["error"] = error;
        }
    }

    const std::string& core_error() const
    {
        return restart_error_.empty() ? core_.error() : restart_error_;
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
            if (!config_files_.create(name, true, new_file)) {
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
        if (action == "install_core") {
            auto error = core_.install();
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
            if (core_.state() != "running") {
                return {false, "WaterWall must be running before it can be restarted."};
            }
            if (!core_.installed()) {
                return {false, "The WaterWall executable must be installed before it can be restarted."};
            }
        }
        if (auto error = core_.stop(); !error.empty()) {
            return {false, std::move(error)};
        }
        restart_pending_ = restart;
        return complete(restart ? "Restarting WaterWall." : "Stopping WaterWall.", "info");
    }

    Reply start_core()
    {
        if (core_.state() != "stopped") {
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
                        if (const auto type = node.find("type");
                            type != node.end() && *type == "AuthenticationServer") {
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
        settings["configs"] = std::move(paths);
        if (auto error = core_.start(settings.dump()); !error.empty()) {
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

    Reply update_panel(const Json& input)
    {
        const auto target = input.find("version");
        return {false, target != input.end() && *target == version ? "That Wave version is already active."
                                                                   : "That Wave version is unavailable."};
    }

    Reply create_core_config(const Json& input)
    {
        const auto value = input.find("name");
        const auto* text = value == input.end() ? nullptr : value->get_ptr<const std::string*>();
        const auto name = text ? core_config_name(*text) : std::string_view{};
        if (name.empty()) {
            return {false, "Enter a valid config filename of up to 64 bytes."};
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
        bool created;
        if (!config_files_.create(name, false, created)) {
            return {false, "Could not create the config folder and file."};
        }
        if (!store_configs(changed)) {
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
            return {false, "Choose a config and AuthenticationServer node."};
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
        if (core_.state() != "running" || config == live_configs_.end() ||
            !has_waterwall_users_api(config->second)) {
            return unavailable();
        }
        const auto names = config_auth_nodes(config->second);
        if (std::ranges::find(names, *target) == names.end()) {
            return {false, "That AuthenticationServer node is unavailable."};
        }
        const auto& node = target->get_ref<const std::string&>();
        auto endpoint = waterwall_users_endpoint(config->second, node);
        if (!endpoint) {
            return {false,
                    "Configure a local TcpListener and an APIServer client with list-users permission for this node."};
        }
        std::size_t bindings = 0;
        for (const auto& [_, launch_config] : live_configs_) {
            bindings += waterwall_tcp_bindings(launch_config, *endpoint);
        }
        if (bindings != 1) {
            return {false, "Use a dedicated TcpListener port for the WaterWall API."};
        }

        const auto run = live_run_;
        const auto pid = core_.running_pid();
        // Keep lifecycle actions and host sampling independent of the API's network deadline.
        lock.unlock();
        auto page = list_waterwall_users(pid, *endpoint, node, cursor);
        lock.lock();
        if (live_run_ != run || core_.state() != "running") {
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
        if (!graph_action && core_in_use(core_.state())) {
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
        const auto rollback = [&] {
            for (const auto& db : created) {
                config_users_.remove(name, db);
            }
        };
        for (const auto& node : config["nodes"]) {
            if (node["type"] != "AuthenticationServer") {
                continue;
            }
            const auto& db = node["panel-settings"]["auth-db"].get_ref<const std::string&>();
            bool new_file;
            auto error = config_users_.prepare(name, db, new_file);
            if (new_file) {
                created.push_back(db);
            }
            if (!error.empty()) {
                rollback();
                return {false, std::move(error)};
            }
        }
        auto saved = config_files_.save(name, contents);
        if (!saved) {
            rollback();
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
            return {false, "Choose an AuthenticationServer node."};
        }
        const auto node = std::ranges::find_if(*nodes, [&](const Json& candidate) {
            return candidate.is_object() && candidate.value("name", Json{}) == *target &&
                   candidate.value("type", Json{}) == "AuthenticationServer";
        });
        if (node == nodes->end()) {
            return {false, "That AuthenticationServer node is unavailable."};
        }
        const auto db = auth_database_name(*node, name, database_->directory());
        if (db.empty()) {
            return {false, "Save the node with a database in its config folder before managing its users."};
        }
        auto result = config_users_.execute(action, name, db, input);
        return {result.success, std::move(result.message), false, std::move(result.result)};
    }

    Reply dispatch(std::string_view action, const Json& input)
    {
        if (action == "refresh") {
            return {true, "Workspace refreshed."};
        }
        if (action == "install_core" || action == "start_core" || action == "stop_core" ||
            action == "restart_core") {
            return change_core_state(action);
        }
        if (action == "update_panel") {
            return update_panel(input);
        }
        if (action == "create_core_config") {
            return create_core_config(input);
        }
        if (action == "set_core_config_enabled" || action == "remove_core_config") {
            return change_core_config(action, input);
        }
        if (action == "get_core_config" || action == "save_core_config" || action == "get_config_users" ||
            action == "create_config_user" || action == "edit_config_user" ||
            action == "delete_config_user") {
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
        if (action == "apply_core_settings") {
            return apply_core_settings(input);
        }
        if (action == "restore_preview_backup") {
            return restore_backup(input);
        }
        if (action == "diagnostics") {
            auto feedback = "WaterWall is " + std::string(core_.state()) + '.';
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
    WaterWall core_;
    WaterWallLogs core_logs_;
    bool restart_pending_ = false;
    std::string restart_error_;
    std::map<int, Json> live_configs_;
    std::uint64_t live_run_ = 0;
    Json data_ = make_data();
    int next_config_id_ = 1;
    std::uint64_t revision_ = 1;
    std::jthread sampler_;
};

} // namespace

void configure_preview_routes(httplib::Server& server, std::shared_ptr<AuthStore> store,
                              std::shared_ptr<PanelDatabase> database)
{
    const auto preview = std::make_shared<Preview>(std::move(database));
    server.WebSocket("/ws/overview",
                     [preview, store](const httplib::Request& req, httplib::ws::WebSocket& socket) {
                         if (!websocket_session(store, req)()) {
                             socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                             return;
                         }
                         if (socket.send(preview->overview())) {
                             socket.close();
                         }
                     });
    server.WebSocket("/ws/preview", [preview, store = std::move(store)](const httplib::Request& req,
                                                                        httplib::ws::WebSocket& socket) {
        preview->serve(socket, websocket_session(store, req));
    });
}

} // namespace wave
