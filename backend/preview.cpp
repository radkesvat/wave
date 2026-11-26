#include "preview.h"

#include "core_config_name.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace wave {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr auto update_interval = 5s;
constexpr std::size_t max_request_bytes = 8192;
constexpr std::size_t max_users = 128;
constexpr std::size_t max_topology_nodes = 64;
constexpr std::size_t max_node_name_bytes = 64;
constexpr std::size_t max_core_configs = 64;
constexpr std::size_t max_logs = 100;
constexpr std::size_t max_console_lines = 80;
constexpr std::size_t max_console_history = 32;

Json user_options()
{
    return {{"protocol", "tcp"}, {"upload_limit", 0}, {"download_limit", 0},
            {"traffic_limit", 0}, {"ip_limit", 0}};
}

Json make_user(int id, std::string_view name)
{
    return {{"id", id}, {"name", name}, {"enabled", true}, {"upload_rate", 0.0},
            {"download_rate", 0.0}, {"upload_total", 0.0}, {"download_total", 0.0},
            {"remaining_minutes", -1}, {"notes", ""}, {"options", user_options()},
            {"configuration", "wave-preview://workspace/user/" + std::to_string(id)}};
}

Json make_users()
{
    constexpr std::array names{
        "Avery Brooks", "Bella Chen", "Cameron Diaz", "Dara Ellis", "Emory Flores",
        "Finley Grant", "Gray Hassan", "Hana Ito", "Indigo James", "Jules Kim", "Kai Lee",
        "Lena Malik", "Milo Novak", "Nadia Ortega", "Oakley Patel", "Parker Quinn",
        "Quinn Rao", "Riley Singh", "Sage Tan", "Taylor Ueda", "Uma Vega", "Val Woods",
        "Wren Xu", "Yael Young", "Zion Zhang", "Morgan Reed", "Noor Ahmad", "Elliot Ward",
    };
    auto users = Json::array();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const int id = static_cast<int>(i) + 1;
        auto user = make_user(id, names[i]);
        user["enabled"] = i % 7 != 4;
        user["upload_total"] = (i * 37 % 730) / 10.0 + 0.8;
        user["download_total"] = (i * 127 % 2900) / 10.0 + 2.4;
        if (i % 4 != 0) {
            user["remaining_minutes"] = i % 9 == 3 ? 0 : 30 + i * 799 % 44640;
        }
        if (user["enabled"] == true && user["remaining_minutes"] != 0 && i % 6 != 5) {
            user["upload_rate"] = (i * 7 % 30 + 1) / 10.0;
            user["download_rate"] = (i * 13 % 57 + 2) / 10.0;
        }
        if (i % 3 == 0) {
            user["options"]["protocol"] = "tcp_websocket";
            user["options"]["ip_limit"] = 3;
        }
        if (i % 4 == 1) {
            user["options"]["traffic_limit"] = 300;
        }
        if (i % 3 == 2) {
            user["notes"] = "Preview account for the West office.";
        }
        users.push_back(std::move(user));
    }
    return users;
}

Json make_core()
{
    return {{"state", "running"}, {"active_version", "1.0.0-preview"},
            {"available_versions", {"1.0.0-preview", "1.1.0-preview", "1.2.0-preview"}},
            {"capabilities", {{"lifecycle", true}, {"versions", true}, {"configs", true}}},
            {"configs", Json::array({
                {{"id", 1}, {"name", "Default"}, {"enabled", true}, {"active", true}},
                {{"id", 2}, {"name", "Standby"}, {"enabled", true}, {"active", false}},
            })}};
}

Json make_server()
{
    const Json cpu = {21, 24, 23, 30, 28, 37, 31, 44, 42, 35, 33, 46, 37, 30, 34, 39, 35, 32, 28};
    const Json memory = {2.3, 2.5, 2.7, 2.6, 2.8, 2.9, 2.7, 3.0, 3.2, 3.4, 3.1, 3.2, 3.3, 3.5,
                         3.4, 3.6, 3.3, 3.3, 3.2};
    const Json upload = {3.8, 4.1, 5.6, 4.9, 7.2, 8.4, 6.8, 5.2, 7.0, 7.8, 9.9, 8.5, 8.8, 9.1,
                         7.4, 6.1, 6.4, 7.5, 8.2};
    const Json download = {11.8, 15.6, 13.5, 14.4, 17.1, 14.8, 22.6, 18.4, 21.9, 16.7, 20.6, 24.3,
                           18.1, 17.8, 19.9, 20.4, 21.8, 19.5, 18.3};
    return {{"name", "Wave Preview"}, {"version", "0.1.0-preview"}, {"platform", "Linux x86_64"},
            {"region", "Frankfurt, DE"}, {"cpu_cores", 8}, {"uptime_minutes", 6203},
            {"memory_total_gib", 8.0}, {"memory_used_gib", memory.back()}, {"cpu_percent", cpu.back()},
            {"upload_rate", upload.back()}, {"download_rate", download.back()},
            {"upload_total", 845.2}, {"download_total", 3342.7}, {"cpu_history", cpu},
            {"memory_history", memory}, {"upload_history", upload}, {"download_history", download},
            {"core", make_core()}};
}

Json make_settings()
{
    return {{"name", "Wave Preview"}, {"admin_username", "demo"}, {"domain", "wave.example.invalid"},
            {"port", 2030}, {"websocket_path", "/ws/preview"}, {"site_template", "Wave Standard"},
            {"certificate_path", "preview/server.crt"}, {"private_key_path", "preview/server.key"},
            {"multiplex", true}};
}

Json make_topology()
{
    auto nodes = Json::array();
    const auto add_node = [&](std::string_view id, std::string_view label, std::string_view type,
                              std::string_view address, std::string_view description, double x,
                              double y, std::string_view map) {
        nodes.push_back({{"id", id}, {"label", label}, {"type", type}, {"address", address},
                         {"description", description}, {"x", x}, {"y", y}, {"map", map}});
    };
    add_node("office", "Office clients", "Client", "192.0.2.10", "Example office devices", 0.14, 0.22, "inside");
    add_node("lab", "QA clients", "Client", "192.0.2.11", "Sample lab devices", 0.14, 0.67, "inside");
    add_node("gateway", "Wave gateway", "Router", "192.0.2.20", "Demonstration ingress", 0.44, 0.42, "both");
    add_node("cache", "Local cache", "Node", "192.0.2.21", "Sample cache for internal clients", 0.77, 0.17, "inside");
    add_node("edge", "Regional relay", "Router", "198.51.100.20", "Sample relay for remote traffic", 0.72, 0.56, "both");
    add_node("cloud", "Preview cloud", "Node", "203.0.113.30", "Example external destination", 0.88, 0.27, "outside");
    add_node("branch", "Remote branch", "Node", "203.0.113.40", "Example failover destination", 0.88, 0.82, "outside");

    auto links = Json::array();
    const auto add_link = [&](std::string_view id, std::string_view from, std::string_view to,
                              std::string_view label, std::string_view type,
                              std::string_view description, std::string_view map) {
        links.push_back({{"id", id}, {"from", from}, {"to", to}, {"label", label}, {"type", type},
                         {"description", description}, {"map", map}});
    };
    add_link("office-gateway", "office", "gateway", "Office LAN", "LAN", "Office client path", "inside");
    add_link("lab-gateway", "lab", "gateway", "Lab LAN", "LAN", "QA client path", "inside");
    add_link("gateway-cache", "gateway", "cache", "Local cache", "LAN", "Example cache access", "inside");
    add_link("gateway-edge", "gateway", "edge", "Regional uplink", "Uplink", "Shared uplink for both maps", "both");
    add_link("edge-cloud", "edge", "cloud", "Primary path", "WAN", "Sample external route", "outside");
    add_link("edge-branch", "edge", "branch", "Failover path", "WAN", "Sample failover route", "outside");

    return {{"nodes", std::move(nodes)}, {"links", std::move(links)}, {"routes", Json::array({
        {{"id", "office-cache"}, {"name", "Office to cache"}, {"path", {"office", "gateway", "cache"}},
         {"description", "Example internal route"}},
        {{"id", "office-cloud"}, {"name", "Office to cloud"}, {"path", {"office", "gateway", "edge", "cloud"}},
         {"description", "Example primary external route"}},
        {{"id", "office-branch"}, {"name", "Office to branch"}, {"path", {"office", "gateway", "edge", "branch"}},
         {"description", "Example failover routing path"}},
    })}};
}

Json make_bot()
{
    Json notification_rules = Json::object();
    for (const char* rule : {"login_failed", "login_succeeded", "settings_changed", "high_cpu",
                             "high_memory", "server_reboot", "panel_started", "user_expired",
                             "traffic_limit", "user_created", "user_disabled", "user_enabled",
                             "user_deleted", "user_support"}) {
        notification_rules[rule] = {{"enabled", true}, {"sound", false}};
    }
    Json permissions = Json::object();
    for (const char* permission : {"status", "traffic", "ip_limit", "speed_limit", "traffic_limit",
                                   "expiration", "notes"}) {
        permissions[permission] = true;
    }
    return {{"connected", false}, {"name", "Wave Preview Bot"}, {"admin_link", "wave-preview://bot/admin"},
            {"notifications", true}, {"allow_status", true}, {"allow_config", true},
            {"overrides", {{"domain", ""}, {"sni", ""}, {"ws_host", ""}, {"port", 0}}},
            {"notification_rules", std::move(notification_rules)}, {"permissions", std::move(permissions)},
            {"last_message", ""}};
}

Json make_console()
{
    return {{"history", Json::array({"status"})}, {"running", true}, {"output", Json::array({
        "Wave Bash preview. Type help for available demo commands.",
        "$ status", "Preview process ready. WaterWall is disconnected.",
    })}};
}

Json make_data()
{
    return {{"users", make_users()}, {"users_live", true}, {"logs", Json::array({
                {{"ts", "09:41:02"}, {"level", "info"}, {"message", "Wave preview started."}},
                {{"ts", "09:41:03"}, {"level", "success"}, {"message", "Loaded sample users and traffic history."}},
                {{"ts", "09:41:04"}, {"level", "warning"}, {"message", "WaterWall is disconnected."}},
                {{"ts", "09:41:06"}, {"level", "info"}, {"message", "Loaded the sample topology and bot settings."}},
                {{"ts", "09:41:10"}, {"level", "success"}, {"message", "Preview workspace is ready."}},
            })},
            {"server", make_server()}, {"settings", make_settings()}, {"topology", make_topology()},
            {"bot", make_bot()}, {"console", make_console()}};
}

bool text_value(const Json& value, std::size_t limit, bool required = false)
{
    const auto* text = value.get_ptr<const std::string*>();
    return text && text->size() <= limit && text->find('\0') == std::string::npos &&
           (!required || text->find_first_not_of(" \t\r\n") != std::string::npos);
}

std::pair<double, double> sample_node_position(const Json& nodes)
{
    if (nodes.empty()) {
        return {0.5, 0.5};
    }

    // Place the new node in the grid slot farthest from its nearest neighbor.
    std::pair<double, double> position;
    double best_distance = -1;
    for (int row = 1; row <= 9; ++row) {
        for (int column = 1; column <= 9; ++column) {
            const double x = column / 10.0;
            const double y = row / 10.0;
            double distance = 2;
            for (const auto& node : nodes) {
                const double dx = x - node["x"].get<double>();
                const double dy = y - node["y"].get<double>();
                distance = std::min(distance, dx * dx + dy * dy);
            }
            if (distance > best_distance) {
                best_distance = distance;
                position = {x, y};
            }
        }
    }
    return position;
}

bool integer_value(const Json& value, int minimum, int maximum)
{
    if (!value.is_number_integer()) {
        return false;
    }
    const double number = value.get<double>();
    return minimum <= number && number <= maximum;
}

template<typename Valid>
bool update_field(const Json& input, Json& result, const char* key, Valid valid)
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
    return update_field(input, result, key, [&](const Json& value) {
        return text_value(value, limit, required);
    });
}

bool update_integer(const Json& input, Json& result, const char* key, int minimum, int maximum)
{
    return update_field(input, result, key, [&](const Json& value) {
        return integer_value(value, minimum, maximum);
    });
}

bool update_bool(const Json& input, Json& result, const char* key)
{
    return update_field(input, result, key, [](const Json& value) { return value.is_boolean(); });
}

bool update_user(const Json& input, Json& result)
{
    if (!update_text(input, result, "name", 64, true) || !update_text(input, result, "notes", 768) ||
        !update_integer(input, result, "remaining_minutes", -1, 5256000) ||
        !update_bool(input, result, "enabled")) {
        return false;
    }

    const auto options = input.find("options");
    if (options == input.end()) {
        return true;
    }
    auto& edited = result["options"];
    return options->is_object() && update_field(*options, edited, "protocol", [](const Json& value) {
        return value == "tcp" || value == "tcp_websocket";
    }) && update_integer(*options, edited, "upload_limit", 0, 1000000) &&
        update_integer(*options, edited, "download_limit", 0, 1000000) &&
        update_integer(*options, edited, "traffic_limit", 0, 1000000) &&
        update_integer(*options, edited, "ip_limit", 0, 1000000);
}

bool update_settings(const Json& input, Json& result)
{
    return input.is_object() && update_text(input, result, "name", 80, true) &&
        update_text(input, result, "admin_username", 64, true) &&
        update_text(input, result, "domain", 160, true) &&
        update_integer(input, result, "port", 1, 65535) &&
        update_field(input, result, "websocket_path", [](const Json& value) {
            return text_value(value, 160, true) && value.get_ref<const std::string&>().starts_with('/');
        }) && update_text(input, result, "site_template", 80, true) &&
        update_text(input, result, "certificate_path", 160) && update_text(input, result, "private_key_path", 160) &&
        update_bool(input, result, "multiplex");
}

bool update_bot_settings(const Json& input, Json& result)
{
    if (!input.is_object() || !update_bool(input, result, "notifications") ||
        !update_bool(input, result, "allow_status") || !update_bool(input, result, "allow_config")) {
        return false;
    }
    if (const auto overrides = input.find("overrides"); overrides != input.end()) {
        auto& changes = result["overrides"];
        if (!overrides->is_object() || !update_text(*overrides, changes, "domain", 160) ||
            !update_text(*overrides, changes, "sni", 160) || !update_text(*overrides, changes, "ws_host", 160) ||
            !update_integer(*overrides, changes, "port", 0, 65535)) {
            return false;
        }
    }
    if (const auto permissions = input.find("permissions"); permissions != input.end()) {
        if (!permissions->is_object()) {
            return false;
        }
        auto& changes = result["permissions"];
        for (auto item = changes.begin(); item != changes.end(); ++item) {
            if (!update_bool(*permissions, changes, item.key().c_str())) {
                return false;
            }
        }
    }
    if (const auto rules = input.find("notification_rules"); rules != input.end()) {
        if (!rules->is_object()) {
            return false;
        }
        auto& changes = result["notification_rules"];
        for (auto item = changes.begin(); item != changes.end(); ++item) {
            const auto rule = rules->find(item.key());
            if (rule != rules->end() && (!rule->is_object() || !update_bool(*rule, *item, "enabled") ||
                                        !update_bool(*rule, *item, "sound"))) {
                return false;
            }
        }
    }
    return true;
}

void append_bounded(Json& array, Json value, std::size_t limit)
{
    if (array.size() == limit) {
        array.erase(array.begin());
    }
    array.push_back(std::move(value));
}

struct Reply {
    bool ok;
    std::string feedback;
    bool changed = false;
};

class Preview {
public:
    Preview()
    {
        sync_server();
    }

    void serve(httplib::ws::WebSocket& socket)
    {
        // Only this handler writes to its socket. A short read timeout lets it
        // publish shared state changes while the current browser is idle.
        socket.set_read_timeout(400ms);
        std::uint64_t sent_revision = 0;
        if (!socket.send(changed_snapshot(sent_revision))) {
            return;
        }
        std::string message;
        while (const auto result = socket.read(message)) {
            const auto frame = result == httplib::ws::Timeout
                ? changed_snapshot(sent_revision)
                : handle_message(result == httplib::ws::Text ? std::string_view(message) : std::string_view{},
                                 sent_revision);
            if (!frame.empty() && !socket.send(frame)) {
                return;
            }
        }
    }

private:
    Json snapshot() const
    {
        return {{"type", "snapshot"}, {"data", data_}};
    }

    std::string changed_snapshot(std::uint64_t& sent_revision)
    {
        std::lock_guard lock(mutex_);
        tick();
        if (revision_ == sent_revision) {
            return {};
        }
        auto frame = snapshot().dump();
        sent_revision = revision_;
        return frame;
    }

    std::string handle_message(std::string_view message, std::uint64_t& sent_revision)
    {
        const auto request = message.size() > max_request_bytes || message.find('\0') != std::string_view::npos
            ? Json{}
            : Json::parse(message, nullptr, false);
        const auto action_value = request.find("action");
        const std::string_view action = action_value != request.end() && text_value(*action_value, 48, true)
            ? action_value->get_ref<const std::string&>()
            : std::string_view{};

        std::lock_guard lock(mutex_);
        tick();
        const auto reply = action.empty()
            ? Reply{false, "Send a JSON preview action."}
            : dispatch(action, request);
        if (reply.changed) {
            sync_server();
            ++revision_;
        }
        auto frame = snapshot();
        frame["action"] = action;
        frame["ok"] = reply.ok;
        frame["feedback"] = reply.feedback;
        if (const auto id = request.find("request_id"); id != request.end() &&
            (id->is_number_integer() || text_value(*id, 80))) {
            frame["request_id"] = *id;
        }
        sent_revision = revision_;
        return frame.dump();
    }

    void log(std::string_view level, const std::string& message)
    {
        // Use a repeatable preview clock so action order can be read without
        // depending on the preview host's wall clock or timezone.
        const unsigned timestamp = (9 * 3600 + 41 * 60 + 10 + ++log_sequence_) % (24 * 3600);
        std::ostringstream time;
        time << std::setfill('0') << std::setw(2) << timestamp / 3600 << ':'
             << std::setw(2) << timestamp / 60 % 60 << ':' << std::setw(2) << timestamp % 60;
        append_bounded(data_["logs"], {{"ts", time.str()}, {"level", level}, {"message", message}}, max_logs);
    }

    Reply complete(std::string feedback, std::string_view level = "success")
    {
        log(level, feedback);
        return {true, std::move(feedback), true};
    }

    void sync_server()
    {
        auto& server = data_["server"];
        unsigned enabled = 0;
        unsigned active = 0;
        double upload_rate = 0.0;
        double download_rate = 0.0;
        double upload_total = 0.0;
        double download_total = 0.0;
        for (const auto& user : data_["users"]) {
            enabled += user["enabled"].get<bool>();
            const auto up = user["upload_rate"].get<double>();
            const auto down = user["download_rate"].get<double>();
            active += up > 0 || down > 0;
            upload_rate += up;
            download_rate += down;
            upload_total += user["upload_total"].get<double>();
            download_total += user["download_total"].get<double>();
        }
        server["users_total"] = data_["users"].size();
        server["users_enabled"] = enabled;
        server["users_active"] = active;
        server["users_online"] = active;
        server["upload_total"] = upload_total;
        server["download_total"] = download_total;
        server["upload_rate"] = upload_rate;
        server["download_rate"] = download_rate;
        server["upload_history"].back() = upload_rate;
        server["download_history"].back() = download_rate;
        server["name"] = data_["settings"]["name"];
    }

    void update_rates(Json& user)
    {
        const auto phase = user["id"].get<unsigned>() + tick_count_;
        const bool active = user["enabled"] == true && user["remaining_minutes"] != 0 && phase % 6 != 0;
        user["upload_rate"] = active ? (phase * 7 % 30 + 1) / 10.0 : 0.0;
        user["download_rate"] = active ? (phase * 13 % 57 + 2) / 10.0 : 0.0;
    }

    void tick()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_tick_) {
            return;
        }
        next_tick_ = now + update_interval;
        ++tick_count_;
        auto& server = data_["server"];
        server["uptime_minutes"] = 6203 + tick_count_ / 12;
        server["cpu_percent"] = 22 + tick_count_ * 7 % 33;
        server["memory_used_gib"] = 2.8 + (tick_count_ * 3 % 11) / 10.0;
        append_bounded(server["cpu_history"], server["cpu_percent"], 19);
        append_bounded(server["memory_history"], server["memory_used_gib"], 19);
        append_bounded(server["upload_history"], server["upload_rate"], 19);
        append_bounded(server["download_history"], server["download_rate"], 19);
        if (data_["users_live"] == true) {
            for (auto& user : data_["users"]) {
                if (tick_count_ % 12 == 0 && user["remaining_minutes"] > 0) {
                    user["remaining_minutes"] = user["remaining_minutes"].get<int>() - 1;
                }
                update_rates(user);
                user["upload_total"] = user["upload_total"].get<double>() +
                    user["upload_rate"].get<double>() * 5.0 / 1024.0;
                user["download_total"] = user["download_total"].get<double>() +
                    user["download_rate"].get<double>() * 5.0 / 1024.0;
            }
        }
        sync_server();
        ++revision_;
    }

    Json::iterator find_user(const Json& input)
    {
        auto& users = data_["users"];
        const auto id = input.find("id");
        if (id == input.end() || !integer_value(*id, 1, std::numeric_limits<int>::max())) {
            return users.end();
        }
        return std::find_if(users.begin(), users.end(), [&](const Json& user) {
            return user["id"] == *id;
        });
    }

    bool duplicate_user_name(const Json& candidate) const
    {
        const auto& users = data_["users"];
        return std::any_of(users.begin(), users.end(), [&](const Json& user) {
            return user["id"] != candidate["id"] && user["name"] == candidate["name"];
        });
    }

    Reply create_user(const Json& input)
    {
        auto& users = data_["users"];
        if (users.size() == max_users || next_user_id_ == std::numeric_limits<int>::max()) {
            return {false, "The sample user list is full."};
        }
        auto user = make_user(next_user_id_, "");
        if (!input.contains("name") || !update_user(input, user)) {
            return {false, "Enter a user name and valid sample limits."};
        }
        if (duplicate_user_name(user)) {
            return {false, "A sample user already uses that name."};
        }
        const auto name = user["name"].get<std::string>();
        users.push_back(std::move(user));
        ++next_user_id_;
        return complete("Created sample user " + name + '.');
    }

    Reply change_user(std::string_view action, const Json& input)
    {
        auto user = find_user(input);
        if (user == data_["users"].end()) {
            return {false, "That sample user is unavailable."};
        }
        const auto name = (*user)["name"].get<std::string>();
        if (action == "delete_user") {
            data_["users"].erase(user);
            return complete("Deleted sample user " + name + '.');
        }
        if (action == "reset_user_traffic") {
            (*user)["upload_total"] = 0.0;
            (*user)["download_total"] = 0.0;
            return complete("Reset sample traffic for " + name + '.');
        }
        if (action == "set_user_enabled") {
            const auto enabled = input.find("enabled");
            if (enabled == input.end() || !enabled->is_boolean()) {
                return {false, "Choose an enabled state."};
            }
            (*user)["enabled"] = *enabled;
            if (*enabled == false) {
                (*user)["upload_rate"] = 0.0;
                (*user)["download_rate"] = 0.0;
            }
            return complete(std::string(*enabled == true ? "Enabled" : "Disabled") + " sample user " + name + '.');
        }

        auto edited = *user;
        if (!update_user(input, edited)) {
            return {false, "Enter a user name and valid sample limits."};
        }
        if (duplicate_user_name(edited)) {
            return {false, "A sample user already uses that name."};
        }
        if (edited["enabled"] == false || edited["remaining_minutes"] == 0) {
            edited["upload_rate"] = 0.0;
            edited["download_rate"] = 0.0;
        }
        auto message = "Updated sample user " + edited["name"].get<std::string>() + '.';
        *user = std::move(edited);
        return complete(std::move(message));
    }

    Reply add_node(const Json& input)
    {
        const auto value = input.find("name");
        if (value == input.end() || !text_value(*value, max_node_name_bytes, true)) {
            return {false, "Enter a sample node name of up to 64 bytes."};
        }
        auto name = value->get<std::string>();
        if (std::ranges::any_of(name, [](unsigned char c) { return c < ' ' || c == 0x7f; })) {
            return {false, "Enter a sample node name without control characters."};
        }
        const auto start = name.find_first_not_of(' ');
        name = name.substr(start, name.find_last_not_of(' ') - start + 1);

        auto& nodes = data_["topology"]["nodes"];
        if (nodes.size() >= max_topology_nodes || next_node_id_ == std::numeric_limits<std::uint64_t>::max()) {
            return {false, "The sample node list is full."};
        }
        if (std::ranges::any_of(nodes, [&](const Json& node) { return node["label"] == name; })) {
            return {false, "A sample node already uses that name."};
        }
        const auto [x, y] = sample_node_position(nodes);
        const auto id = "sample-node-" + std::to_string(next_node_id_);
        nodes.push_back({{"id", id}, {"label", name}, {"type", "Node"}, {"address", ""},
                         {"description", ""}, {"x", x}, {"y", y}, {"map", "both"}});
        ++next_node_id_;
        return complete("Added sample node " + name + '.');
    }

    Reply delete_node(const Json& input)
    {
        const auto id = input.find("id");
        if (id == input.end() || !text_value(*id, 64, true)) {
            return {false, "Select a sample node to delete."};
        }
        auto& topology = data_["topology"];
        auto& nodes = topology["nodes"];
        const auto node = std::ranges::find_if(nodes, [&](const Json& item) { return item["id"] == *id; });
        if (node == nodes.end()) {
            return {false, "That sample node is unavailable."};
        }

        const auto name = (*node)["label"].get<std::string>();
        std::erase_if(topology["links"].get_ref<Json::array_t&>(), [&](const Json& link) {
            return link["from"] == *id || link["to"] == *id;
        });
        std::erase_if(topology["routes"].get_ref<Json::array_t&>(), [&](const Json& route) {
            const auto& path = route["path"];
            return std::ranges::find(path, *id) != path.end();
        });
        nodes.erase(node);
        return complete("Deleted sample node " + name + '.');
    }

    Reply user_bot_action(const Json& input)
    {
        const auto kind = input.find("kind");
        const auto message = input.find("message");
        if (kind == input.end() || !text_value(*kind, 32, true) ||
            (message != input.end() && !text_value(*message, 512))) {
            return {false, "Enter a valid demo bot action."};
        }
        std::string target = "all sample users";
        if (input.contains("id")) {
            const auto user = find_user(input);
            if (user == data_["users"].end()) {
                return {false, "That sample user is unavailable."};
            }
            target = (*user)["name"].get<std::string>();
        } else if (*kind != "broadcast" && *kind != "configuration") {
            return {false, "Choose a sample user for this bot action."};
        }
        if ((*kind == "message" || *kind == "broadcast") &&
            (message == input.end() || !text_value(*message, 512, true))) {
            return {false, "Enter a preview message."};
        }
        const auto title = kind->get_ref<const std::string&>();
        if (*kind != "broadcast" && *kind != "configuration" && *kind != "message" && *kind != "invite") {
            return {false, "Choose a supported demo bot action."};
        }
        return complete("Simulated bot " + title + " for " + target + '.');
    }

    Reply apply_settings(const Json& input)
    {
        const auto values = input.find("settings");
        auto settings = data_["settings"];
        if (values == input.end() || !update_settings(*values, settings)) {
            return {false, "Enter valid preview settings. The WebSocket path must start with / and the port must be 1 to 65535."};
        }
        if (const auto password = values->find("password"); password != values->end()) {
            if (!text_value(*password, 96, true)) {
                return {false, "Enter a valid demo password."};
            }
            demo_password_ = password->get<std::string>();
        }
        data_["settings"] = std::move(settings);
        return complete("Applied sample panel settings.");
    }

    Reply apply_bot_settings(const Json& input)
    {
        const auto values = input.find("settings");
        auto bot = data_["bot"];
        if (values == input.end() || !update_bot_settings(*values, bot)) {
            return {false, "Enter valid sample bot settings."};
        }
        data_["bot"] = std::move(bot);
        return complete("Saved sample bot settings.");
    }

    Reply bot_action(const Json& input)
    {
        if (data_["bot"]["connected"] != true) {
            return {false, "Connect the preview bot before sending a test action."};
        }
        const auto kind = input.find("kind");
        const auto message = input.find("message");
        const auto audience = input.find("audience");
        if (kind == input.end() || !text_value(*kind, 40, true) ||
            (message != input.end() && !text_value(*message, 512)) ||
            (audience != input.end() && !text_value(*audience, 80, true))) {
            return {false, "Enter a valid test action and preview message."};
        }
        const std::string preview_text = message != input.end() && text_value(*message, 512, true)
            ? message->get<std::string>()
            : "Wave preview bot is connected. WaterWall is disconnected.";
        data_["bot"]["last_message"] = preview_text;
        return complete("Simulated bot " + kind->get<std::string>() + " for " +
                        (audience == input.end() ? "preview users" : audience->get<std::string>()) + '.');
    }

    Reply console_command(const Json& input)
    {
        auto& console = data_["console"];
        if (console["running"] != true) {
            return {false, "Start the demo terminal before entering a command."};
        }
        const auto value = input.find("input");
        if (value == input.end() || !text_value(*value, 512, true)) {
            return {false, "Enter a demo command."};
        }
        const auto& command = value->get_ref<const std::string&>();
        std::string output = "Recorded as preview input. Type help for available demo commands.";
        if (command == "help") {
            output = "Demo commands: help, status, uptime, users, whoami, pwd.";
        } else if (command == "status") {
            output = "Preview process ready. WaterWall is disconnected.";
        } else if (command == "uptime") {
            output = "Sample uptime: " + std::to_string(data_["server"]["uptime_minutes"].get<unsigned>()) + " minutes.";
        } else if (command == "users") {
            output = std::to_string(data_["users"].size()) + " sample users.";
        } else if (command == "whoami") {
            output = "wave-preview";
        } else if (command == "pwd") {
            output = "/preview/workspace";
        }

        append_bounded(console["history"], command, max_console_history);
        append_bounded(console["output"], "$ " + command, max_console_lines);
        append_bounded(console["output"], std::move(output), max_console_lines);
        return complete("Processed demo console input.", "info");
    }

    Reply sign_in(const Json& input)
    {
        const auto username = input.find("username");
        const auto password = input.find("password");
        if (username == input.end() || !text_value(*username, 64, true) ||
            password == input.end() || !text_value(*password, 96, true) ||
            *username != data_["settings"]["admin_username"] || *password != demo_password_) {
            log("warning", "Demo sign-in was unsuccessful.");
            return {false, "Use the demo sign-in details shown on the form.", true};
        }
        return complete("Signed in to the preview.");
    }

    Reply setup(const Json& input)
    {
        const auto name = input.find("name");
        const auto username = input.find("username");
        const auto password = input.find("password");
        if (name == input.end() || !text_value(*name, 80, true) ||
            username == input.end() || !text_value(*username, 64, true) ||
            password == input.end() || !text_value(*password, 96, true)) {
            return {false, "Enter a panel name and valid demo credentials."};
        }
        auto& settings = data_["settings"];
        settings["name"] = *name;
        settings["admin_username"] = *username;
        demo_password_ = password->get<std::string>();
        return complete("Completed first-run preview setup.");
    }

    Reply change_core_state(std::string_view action)
    {
        auto& state = data_["server"]["core"]["state"];
        if (action == "start_core" && state == "stopped") {
            state = "running";
            return complete("Started the preview core.");
        }
        if (action == "stop_core" && state == "running") {
            state = "stopped";
            return complete("Stopped the preview core.");
        }
        return {false, "That action is unavailable in the current preview core state."};
    }

    Reply update_core(const Json& input)
    {
        auto& core = data_["server"]["core"];
        const auto version = input.find("version");
        const auto& available = core["available_versions"];
        if (version == input.end() || std::ranges::find(available, *version) == available.end()) {
            return {false, "Choose an available preview core version."};
        }
        if (*version == core["active_version"]) {
            return {false, "That preview core version is already active."};
        }
        core["active_version"] = *version;
        return complete("Updated the preview core to " + version->get<std::string>() + '.');
    }

    Reply create_core_config(const Json& input)
    {
        const auto value = input.find("name");
        const auto* text = value == input.end() ? nullptr : value->get_ptr<const std::string*>();
        const auto name = text ? core_config_name(*text) : std::string_view{};
        if (name.empty()) {
            return {false, "Enter a config name of up to 64 bytes without control characters."};
        }

        auto& configs = data_["server"]["core"]["configs"];
        if (configs.size() >= max_core_configs || next_config_id_ == std::numeric_limits<int>::max()) {
            return {false, "The preview config list is full."};
        }
        if (std::ranges::any_of(configs, [&](const Json& config) { return config["name"] == name; })) {
            return {false, "A preview config already uses that name."};
        }
        configs.push_back({{"id", next_config_id_}, {"name", name}, {"enabled", false}, {"active", false}});
        ++next_config_id_;
        return complete("Created preview config " + std::string(name) + " (disabled).");
    }

    Reply change_core_config(std::string_view action, const Json& input)
    {
        const auto id = input.find("id");
        if (id == input.end() || !integer_value(*id, 1, std::numeric_limits<int>::max())) {
            return {false, "Choose a preview config."};
        }
        auto& configs = data_["server"]["core"]["configs"];
        const auto config = std::ranges::find_if(configs, [&](const Json& item) { return item["id"] == *id; });
        if (config == configs.end()) {
            return {false, "That preview config is unavailable."};
        }
        if ((*config)["active"] == true) {
            return {false, "The active preview config is protected."};
        }

        const auto name = (*config)["name"].get<std::string>();
        if (action == "remove_core_config") {
            configs.erase(config);
            return complete("Removed preview config " + name + '.');
        }
        const auto enabled = input.find("enabled");
        if (enabled == input.end() || !enabled->is_boolean()) {
            return {false, "Choose whether to enable the preview config."};
        }
        if ((*config)["enabled"] == *enabled) {
            return {true, "The preview config already has that setting."};
        }
        (*config)["enabled"] = *enabled;
        return complete(std::string(*enabled == true ? "Enabled" : "Disabled") + " preview config " + name + '.');
    }

    Reply dispatch(std::string_view action, const Json& input)
    {
        if (action == "refresh") {
            return {true, "Sample workspace refreshed."};
        }
        if (action == "start_core" || action == "stop_core") {
            return change_core_state(action);
        }
        if (action == "update_core") {
            return update_core(input);
        }
        if (action == "create_core_config") {
            return create_core_config(input);
        }
        if (action == "set_core_config_enabled" || action == "remove_core_config") {
            return change_core_config(action, input);
        }
        if (action == "add_node") {
            return add_node(input);
        }
        if (action == "delete_node") {
            return delete_node(input);
        }
        if (action == "create_user") {
            return create_user(input);
        }
        if (action == "edit_user" || action == "set_user_enabled" || action == "delete_user" ||
            action == "reset_user_traffic") {
            return change_user(action, input);
        }
        if (action == "set_users_live") {
            const auto live = input.find("live");
            if (live == input.end() || !live->is_boolean()) {
                return {false, "Choose a sample update mode."};
            }
            data_["users_live"] = *live;
            return complete(*live == true ? "Resumed live sample user traffic." : "Paused sample user traffic.", "info");
        }
        if (action == "user_bot_action") {
            return user_bot_action(input);
        }
        if (action == "clear_logs") {
            data_["logs"].clear();
            return {true, "Sample logs cleared.", true};
        }
        if (action == "apply_settings") {
            return apply_settings(input);
        }
        if (action == "backup_settings") {
            return complete("Prepared sample panel backup.", "info");
        }
        if (action == "console_command") {
            return console_command(input);
        }
        if (action == "clear_console") {
            data_["console"]["output"].clear();
            return complete("Cleared demo console output.", "info");
        }
        if (action == "start_console" || action == "stop_console") {
            const bool running = action == "start_console";
            data_["console"]["running"] = running;
            return complete(running ? "Started demo terminal." : "Stopped demo terminal.", "info");
        }
        if (action == "bot_connect") {
            const auto token = input.find("token");
            if (token == input.end() || !text_value(*token, 160, true)) {
                return {false, "Enter a demo bot token."};
            }
            data_["bot"]["connected"] = true;
            return complete("Connected the preview bot.");
        }
        if (action == "bot_disconnect") {
            data_["bot"]["connected"] = false;
            return complete("Disconnected the preview bot.", "info");
        }
        if (action == "apply_bot_settings") {
            return apply_bot_settings(input);
        }
        if (action == "bot_action") {
            return bot_action(input);
        }
        if (action == "sign_in") {
            return sign_in(input);
        }
        if (action == "sign_out") {
            return complete("Signed out of the preview.", "info");
        }
        if (action == "setup") {
            return setup(input);
        }
        if (action == "diagnostics") {
            return complete("Preview diagnostics completed. WaterWall is disconnected.", "info");
        }
        return {false, "That preview action is unavailable."};
    }

    std::mutex mutex_;
    Json data_ = make_data();
    int next_user_id_ = static_cast<int>(data_["users"].size()) + 1;
    std::uint64_t next_node_id_ = 1;
    int next_config_id_ = 3;
    std::uint64_t revision_ = 1;
    std::uint64_t tick_count_ = 0;
    unsigned log_sequence_ = 0;
    std::chrono::steady_clock::time_point next_tick_ = std::chrono::steady_clock::now() + update_interval;
    std::string demo_password_ = "wave-demo";
};

} // namespace

void configure_preview_route(httplib::Server& server)
{
    server.WebSocket("/ws/preview", [preview = std::make_shared<Preview>()](const httplib::Request&,
                                                                            httplib::ws::WebSocket& socket) {
        preview->serve(socket);
    });
}

} // namespace wave
