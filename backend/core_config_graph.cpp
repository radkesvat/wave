#include "core_config_graph.h"

#include "core_config_json.h"
#include "core_config_users.h"
#include "waterwall_node_links.h"
#include "waterwall_nodes.h"

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

std::string new_db_name()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) {
        return {};
    }
    std::string result = "auth-";
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0xf]);
    }
    return result + ".json";
}

} // namespace

Json config_auth_nodes(const Json& config)
{
    auto names = Json::array();
    std::set<std::string_view> seen;
    const auto& nodes = field(config, "nodes");
    if (nodes.is_array()) {
        for (const auto& node : nodes) {
            const auto& name = field(node, "name");
            if (field(node, "type") == "AuthenticationServer" && valid_name(name) &&
                seen.insert(text(name)).second) {
                names.push_back(name);
            }
        }
    }
    return names;
}

std::string auth_database_name(const Json& node, std::string_view config_name,
                               const fs::path& executable_directory)
{
    const auto configured = text(field(field(node, "settings"), "db-path"));
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

std::string validate_config_graph(const Json& config, const Json& previous)
{
    const auto& nodes = field(config, "nodes");
    if (!config.is_object() || !valid_name(field(config, "name")) || !nodes.is_array()) {
        return "The config must have a name and a nodes array.";
    }
    for (const char* key : {"config-version", "core-minimum-version"}) {
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
        if (!std::ranges::binary_search(waterwall_node_types, text(type)) &&
            !std::ranges::any_of(field(previous, "nodes"), [&](const Json& existing) {
                return field(existing, "name") == name && field(existing, "type") == type;
            })) {
            return "WaterWall 1.6.0 does not include node type " + type.get<std::string>() + '.';
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
            if (type == "AuthenticationServer" || !allows_node_next(type, settings) || !find_target(*next)) {
                return "Invalid next node on " + std::string(name) + '.';
            }
            next_nodes.emplace(name, text(*next));
        }
        if (type == "Router" || type == "SniffRouter") {
            const auto routes = settings.find(type == "SniffRouter" ? "routes" : "rules");
            if (routes != settings.end() && (!routes->is_array() || routes->empty())) {
                return "Invalid route target on " + std::string(name) + '.';
            }
        }
        std::string error;
        visit_node_links(*node, [&](const Json& container, const NodeLinkField& link, const char* key) {
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
        if (type == "PacketSplitStream" && settings.contains("up") &&
            field(settings, "up") == field(settings, "down")) {
            return "PacketSplitStream up and down must point to different nodes on " + std::string(name) + '.';
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

std::string configure_auth_databases(Json& config, const Json& previous, std::string_view config_name,
                                     const fs::path& executable_directory)
{
    std::map<std::string_view, std::string> existing;
    std::set<std::string> known;
    for (const auto& node : field(previous, "nodes")) {
        if (field(node, "type") == "AuthenticationServer") {
            auto filename = auth_database_name(node, config_name, executable_directory);
            if (!filename.empty()) {
                known.insert(filename);
            }
            existing.try_emplace(text(field(node, "name")), std::move(filename));
        }
    }
    std::set<std::string> assigned;
    for (auto& node : config["nodes"]) {
        if (node["type"] != "AuthenticationServer") {
            continue;
        }
        const auto name = text(node["name"]);
        auto& metadata = node["panel-settings"];
        auto& settings = node["settings"];
        const auto stored_name = text(field(metadata, "auth-db"));
        const auto requested = auth_database_name(node, config_name, executable_directory);
        std::string filename;
        if (!stored_name.empty() && known.contains(std::string(stored_name))) {
            filename = stored_name;
        } else if (known.contains(requested)) {
            filename = requested;
        } else if (const auto original = existing.find(name); original != existing.end()) {
            filename = original->second;
        }
        if (filename.empty() && settings.contains("db-path")) {
            return "The existing database for " + std::string(name) +
                   " must be inside its config folder and referenced by the saved config.";
        }
        if (filename.empty()) {
            filename = new_db_name();
            if (filename.empty()) {
                return "Could not allocate an AuthenticationServer database name.";
            }
        }
        if (!assigned.insert(filename).second) {
            return "Each AuthenticationServer needs its own database file.";
        }
        metadata["auth-db"] = filename;
        settings["db-path"] = CoreConfigUsers::relative_path(config_name, filename).generic_string();
        if (!settings.contains("file-save-rate-ms")) {
            settings["file-save-rate-ms"] = 10000;
        }
    }
    return {};
}

} // namespace wave
