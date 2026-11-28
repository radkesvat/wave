#include "core_config_graph.h"

#include "core_config_json.h"
#include "core_config_users.h"
#include "schematic_rules.h"
#include "waterwall_node_links.h"
#include "waterwall_users.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

const Json& field(const Json& object, const char* key)
{
    static const Json empty;
    const auto found = object.find(key);
    return found == object.end() ? empty : *found;
}

std::string_view text(const Json& value)
{
    const auto* string = value.get_ptr<const std::string*>();
    return string ? std::string_view(*string) : std::string_view{};
}

bool valid_name(const Json& value)
{
    return valid_core_node_name(text(value));
}

bool valid_version(const Json& value)
{
    return value.is_number_integer() && value >= 0 && value <= std::numeric_limits<int>::max();
}

bool contains_nul(const Json& value)
{
    if (value.is_object()) {
        for (const auto& [key, field] : value.items()) {
            if (key.find('\0') != key.npos || contains_nul(field)) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        return std::ranges::any_of(value, contains_nul);
    }
    return text(value).find('\0') != std::string_view::npos;
}

template <std::size_t size> std::string random_hex()
{
    std::array<unsigned char, size> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) {
        return {};
    }
    std::string result;
    result.reserve(2 * size);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0xf]);
    }
    return result;
}

std::string new_db_name()
{
    const auto suffix = random_hex<16>();
    return suffix.empty() ? std::string{} : "auth-" + suffix + ".json";
}

} // namespace

Json make_empty_core_config(std::string_view name)
{
    return {{"name", name}, {"config-version", 1}, {"core-minimum-version", 0}, {"nodes", Json::array()}};
}

Json make_users_starter_config(std::string_view name, std::uint16_t port, bool allow_live_user_management)
{
    const auto& schematic = waterwall_schematic();
    const auto& starter = schematic_field(schematic_field(schematic, "config"), "users_starter");
    const auto management_slot = users_starter_management_slot(schematic);
    if (!starter.is_object() || (allow_live_user_management && management_slot.empty())) {
        return nullptr;
    }
    const auto secret = random_hex<32>();
    const auto token = random_hex<32>();
    if (secret.empty() || token.empty()) {
        return nullptr;
    }

    auto config = make_empty_core_config(name);
    config.update(starter.at("template"));
    constexpr char report_interval[] = "push-interval-ms";
    const auto& client_fields = field(waterwall_node("AuthenticationClient"), "fields");
    const bool supports_reports = std::ranges::any_of(client_fields, [&](const Json& descriptor) {
        return field(descriptor, "key") == report_interval;
    });
    if (supports_reports) {
        for (auto& node : config.at("nodes")) {
            if (field(node, "type") == "AuthenticationClient" && field(node, "settings").is_object()) {
                node.at("settings").emplace(report_interval, 500);
            }
        }
    }

    // Bind generated values as JSON nodes so config names and ports keep their types.
    const auto& slots = starter.at("slots");
    for (const auto& [slot, value] : std::array<std::pair<const char*, Json>, 4>{
             {{"name", name}, {"secret", secret}, {"token", token}, {"port", port}}}) {
        for (const auto& pointer : slots.at(slot)) {
            config.at(Json::json_pointer(pointer.get<std::string>())) = value;
        }
    }
    if (allow_live_user_management) {
        auto operations = Json::array();
        for (const auto operation : waterwall_users_operations) {
            operations.push_back(waterwall_users_operation_name(operation));
        }
        config.at(Json::json_pointer(std::string(management_slot))) = std::move(operations);
    }
    config["name"] = name;
    return config;
}

bool uses_auth_database(const Json& node)
{
    const auto& definition = waterwall_node(text(field(node, "type")));
    return schematic_field(definition, "user_database") == true;
}

Json config_auth_nodes(const Json& config)
{
    auto names = Json::array();
    std::set<std::string_view> seen;
    const auto& nodes = field(config, "nodes");
    if (nodes.is_array()) {
        for (const auto& node : nodes) {
            const auto& name = field(node, "name");
            if (uses_auth_database(node) && valid_name(name) && seen.insert(text(name)).second) {
                names.push_back(name);
            }
        }
    }
    return names;
}

std::string auth_database_name(const Json& node, std::string_view config_name,
                               const fs::path& executable_directory)
{
    return auth_database_name(node, config_name, executable_directory, waterwall_node(text(field(node, "type"))));
}

std::string auth_database_name(const Json& node, std::string_view config_name,
                               const fs::path& executable_directory, const Json& definition)
{
    const auto key = schematic_text(schematic_field(definition, "database_key"));
    const auto configured = text(schematic_field(field(node, "settings"), key));
    if (configured.empty() || configured.find('\0') != std::string_view::npos) {
        return {};
    }
    auto path = fs::path(configured).lexically_normal();
    if (path.is_relative()) {
        path = executable_directory / path;
    }
    const auto filename = path.filename().string();
    return CoreConfigUsers::valid_filename(filename) &&
                   path == executable_directory / CoreConfigUsers::relative_path(config_name, filename)
               ? filename
               : std::string{};
}

std::string validate_config_graph(const Json& config, const Json& previous, const WaterWallSchematic* schematic)
{
    const auto& schema = schematic ? waterwall_schematic(*schematic) : waterwall_schematic();
    const auto definition = [&](std::string_view type) -> const Json& {
        return schematic_field(schematic_field(schema, "nodes"), type);
    };
    const auto& nodes = field(config, "nodes");
    if (!config.is_object() || !valid_name(field(config, "name")) || !nodes.is_array()) {
        return "The config must have a name and a nodes array.";
    }
    if (contains_nul(config)) {
        return "Config strings cannot contain a NUL character.";
    }
    for (const auto& version_key :
         schematic_field(schematic_field(schema, "config"), "version_keys")) {
        const auto key = schematic_text(version_key);
        if (config.contains(key) && !valid_version(config[key])) {
            return std::string(key) + " must be a nonnegative integer.";
        }
    }

    std::map<std::string_view, const Json*> named;
    for (const auto& node : nodes) {
        const auto& name = field(node, "name");
        const auto& type = field(node, "type");
        if (!valid_name(name) || !valid_name(type)) {
            return "Each node needs a valid name and type of up to 128 bytes.";
        }
        if (!named.emplace(text(name), &node).second) {
            return "Duplicate node name: " + name.get<std::string>();
        }
        if (!definition(text(type)).is_object() &&
            !std::ranges::any_of(field(previous, "nodes"), [&](const Json& existing) {
                return field(existing, "name") == name && field(existing, "type") == type;
            })) {
            return "WaterWall " + std::string(text(field(schema, "version"))) + " does not include node type " +
                   type.get<std::string>() + '.';
        }
        if ((node.contains("settings") && !node["settings"].is_object()) ||
            (node.contains("version") && !valid_version(node["version"]))) {
            return "Invalid settings or version on node " + name.get<std::string>() + '.';
        }
        if (const auto panel = node.find("panel-settings"); panel != node.end()) {
            if (!panel->is_object()) {
                return "Node panel-settings must be an object.";
            }
            for (const char* axis : {"x", "y"}) {
                if (const auto coordinate = panel->find(axis);
                    coordinate != panel->end() &&
                    (!coordinate->is_number() || !std::isfinite(coordinate->get<double>()))) {
                    return "Node positions must contain finite coordinates.";
                }
            }
        }
    }

    std::map<std::string_view, std::string_view> next_nodes;
    for (const auto& [name, node] : named) {
        const auto type = text((*node)["type"]);
        const auto& settings = field(*node, "settings");
        const auto find_target = [&](const Json& value) -> const Json* {
            const auto target = named.find(text(value));
            return target != named.end() && target->first != name ? target->second : nullptr;
        };
        if (const auto next = node->find("next"); next != node->end()) {
            const auto* target = find_target(*next);
            if (!node_allows_next(definition(type), settings) || !target ||
                !node_allows_previous(definition(text(field(*target, "type"))))) {
                return "Invalid next node on " + std::string(name) + '.';
            }
            next_nodes.emplace(name, text(*next));
        }
        if (auto issue =
                schematic_rules::validate(schematic_field(definition(type), "graph_rules"), settings);
            !issue.empty()) {
            return std::string(name) + ": " + issue;
        }
        std::string error;
        const auto links = schematic ? node_link_fields(*schematic, type) : node_link_fields(type);
        visit_node_links(*node, links, [&](const Json& container, const NodeLinkField& link, const char* key) {
            if (!error.empty() || !node_link_active(container, link)) {
                return;
            }
            const auto* target = find_target(field(container, key));
            if (!target) {
                error = "Invalid " + std::string(key) + " on " + std::string(name) +
                        ". Choose a different existing node in this config.";
                return;
            }
            if (!node_link_target_type_matches(link, text(field(*target, "type")), field(*target, "settings"))) {
                error = "Incompatible target for " + std::string(key) + " on " + std::string(name) + '.';
            }
        });
        if (!error.empty()) {
            return error;
        }
    }
    std::set<std::string_view> visited;
    for (const auto& [name, unused] : next_nodes) {
        std::set<std::string_view> chain;
        auto current = name;
        while (!visited.contains(current)) {
            if (!chain.insert(current).second) {
                return "Next connections contain a cycle through " + std::string(current) + '.';
            }
            const auto next = next_nodes.find(current);
            if (next == next_nodes.end()) {
                break;
            }
            current = next->second;
        }
        visited.merge(chain);
    }
    return {};
}

std::string configure_auth_databases(Json& config, const OfflineUsersBindings& previous, std::string_view config_name,
                                     const fs::path& executable_directory)
{
    std::set<std::string_view> known;
    for (const auto& [name, binding] : previous) {
        if (!binding.database.empty()) {
            known.insert(binding.database);
        }
    }
    std::set<std::string> assigned;
    for (auto& node : config["nodes"]) {
        if (!uses_auth_database(node)) {
            continue;
        }
        const auto name = text(node["name"]);
        const auto& definition = waterwall_node(text(node["type"]));
        const auto database_key = schematic_text(definition.at("database_key"));
        auto& metadata = node["panel-settings"];
        auto& settings = node["settings"];
        const auto stored_name = text(field(metadata, "auth-db"));
        const auto requested = auth_database_name(node, config_name, executable_directory);
        std::string filename;
        if (known.contains(stored_name)) {
            filename = stored_name;
        } else if (known.contains(requested)) {
            filename = requested;
        } else if (const auto original = previous.find(name); original != previous.end()) {
            filename = original->second.database;
        }
        if (filename.empty() && settings.contains(database_key)) {
            return "The existing database for " + std::string(name) +
                   " must be inside its config folder and referenced by the saved config.";
        }
        if (filename.empty()) {
            filename = new_db_name();
            if (filename.empty()) {
                return "Could not allocate a user database name.";
            }
        }
        if (!assigned.insert(filename).second) {
            return "Each user database node needs its own database file.";
        }
        metadata["auth-db"] = filename;
        settings[database_key] = CoreConfigUsers::relative_path(config_name, filename).generic_string();
        for (const auto& [key, value] : schematic_field(definition, "database_defaults").items()) {
            if (!settings.contains(key)) {
                settings[key] = value;
            }
        }
    }
    return {};
}

} // namespace wave
