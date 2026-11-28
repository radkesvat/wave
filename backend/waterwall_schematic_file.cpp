#include "waterwall_schematic_file.h"

#include "bundled_waterwall_schematic.h"
#include "core_config_users.h"
#include "core_settings.h"
#include "private_temp_directory.h"
#include "waterwall_schematic.h"
#include "waterwall_users.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::string_view legacy_binary_version = "1.6.0";

bool trusted_path(const fs::path& path, mode_t kind, mode_t extra_forbidden_modes = 0)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && (info.st_mode & S_IFMT) == kind &&
           (info.st_uid == ::geteuid() || info.st_uid == 0) &&
           !(info.st_mode & (S_IWGRP | S_IWOTH | extra_forbidden_modes));
}

bool prepare_directory(const fs::path& path)
{
    return (::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) && trusted_path(path, S_IFDIR);
}

std::string schematic_filename(std::string_view version)
{
    return "waterwall-" + std::string(version) + ".json";
}

std::string_view version_in_schematic_filename(std::string_view name)
{
    constexpr std::string_view prefix = "waterwall-";
    constexpr std::string_view suffix = ".json";
    if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
        return {};
    }
    name.remove_prefix(prefix.size());
    name.remove_suffix(suffix.size());
    return valid_waterwall_version(name) ? name : std::string_view{};
}

bool missing(const fs::path& path)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) != 0 && errno == ENOENT;
}

template <typename WriteCandidate>
void seed_schematic(const fs::path& target, WriteCandidate write_candidate)
{
    if (!missing(target) || !prepare_directory(target.parent_path())) {
        return;
    }
    const PrivateTempDirectory staging(target.parent_path(), ".waterwall-migration-");
    if (staging.path().empty()) {
        return;
    }
    const auto candidate = staging.path() / target.filename();
    if (!write_candidate(candidate) || !trusted_path(candidate, S_IFREG)) {
        return;
    }
    try {
        prepare_waterwall_schematic_file(candidate);
    } catch (const std::runtime_error&) {
        return;
    }
    // A release file published in the meantime takes priority.
    std::error_code error;
    fs::create_hard_link(candidate, target, error);
}

bool write_bundled_schematic(const fs::path& candidate)
{
    std::ofstream output(candidate, std::ios::binary);
    output.write(bundled_waterwall_schematic_json, sizeof(bundled_waterwall_schematic_json) - 1);
    output.close();
    return output && ::chmod(candidate.c_str(), 0600) == 0;
}

void migrate_legacy_schematics(const fs::path& legacy, const fs::path& bin_directory)
{
    if (!trusted_path(legacy, S_IFDIR)) {
        return;
    }
    std::error_code error;
    const fs::directory_iterator end;
    for (fs::directory_iterator entry(legacy, error); !error && entry != end; entry.increment(error)) {
        const auto& source = entry->path();
        const auto name = source.filename().string();
        const auto version = version_in_schematic_filename(name);
        std::error_code file_error;
        if (!version.empty() && trusted_path(source, S_IFREG) &&
            fs::file_size(source, file_error) <= max_waterwall_schematic_bytes && !file_error) {
            seed_schematic(waterwall_schematic_path(bin_directory, version), [&](const fs::path& candidate) {
                return fs::copy_file(source, candidate, file_error);
            });
        }
    }
}

void migrate_legacy_binary(const fs::path& bin_directory)
{
    const auto source = bin_directory / "Waterwall";
    if (!trusted_path(source, S_IFREG, S_ISUID | S_ISGID) || ::access(source.c_str(), X_OK) != 0) {
        return;
    }
    const auto directory = waterwall_release_directory(bin_directory, legacy_binary_version);
    const auto target = directory / source.filename();
    const auto warn = [&](std::string_view reason) {
        std::cerr << "Warning: Could not migrate legacy WaterWall executable " << source << " to " << target
                  << ". " << reason << " Wave will continue with the selected release.\n";
    };
    if (!prepare_directory(directory)) {
        warn("Cannot create or use the release directory.");
        return;
    }
    std::error_code error;
    const auto destination = fs::symlink_status(target, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        warn("Cannot inspect the migration destination. " + error.message() + '.');
        return;
    }
    if (fs::exists(destination)) {
        return;
    }
    const PrivateTempDirectory staging(directory, ".waterwall-migration-");
    const auto staging_error = std::error_code(errno, std::generic_category());
    if (staging.path().empty()) {
        warn("Cannot create a private staging directory. " + staging_error.message() + '.');
        return;
    }
    const auto candidate = staging.path() / source.filename();
    if (!fs::copy_file(source, candidate, error)) {
        warn("Cannot copy the legacy executable into staging. " + error.message() + '.');
        return;
    }
    // The service owns the copy even when its access to the source came from group or other bits.
    fs::permissions(candidate, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::add, error);
    if (error) {
        warn("Cannot make the staged executable readable and executable by its owner. " + error.message() + '.');
        return;
    }
    fs::create_hard_link(candidate, target, error);
    if (error && error != std::errc::file_exists) {
        warn("Cannot publish the executable from staging. " + error.message() + '.');
    }
}

bool archive_name(std::string_view name)
{
    return !name.empty() && name.ends_with(".zip") && std::ranges::all_of(name, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '-' || ch == '_' || ch == '.';
    });
}

bool valid_hash(std::string_view value)
{
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

void require(bool valid, const char* issue)
{
    if (!valid) {
        throw std::runtime_error(issue);
    }
}

void validate_log_path(const Json& core, const Json& value)
{
    const auto path = schematic_text(value);
    const auto split = path.rfind('/');
    require(path.starts_with('/') && split > 0 && split + 1 < path.size(), "Invalid logger settings path.");
    const auto& field = core_settings::field_at(core, Json::json_pointer(std::string(path)));
    require(schematic_field(field, "kind") == "text" && schematic_field(field, "default").is_string(),
            "A logger path must identify a core text setting with a default.");
}

bool is_generated_management_grant(const Json& schematic, const Json& starter, const Json& live,
                                   const Json::json_pointer& grant, const std::set<std::string>& slots)
{
    const auto operations = grant / "operations";
    const auto allow = grant.parent_pointer();
    const auto client = allow.parent_pointer();
    const auto clients = client.parent_pointer();
    const auto settings = clients.parent_pointer();
    const auto api = settings.parent_pointer();
    const auto& config = starter.at("template");
    if (api.parent_pointer().to_string() != "/nodes" || allow.back() != "allow" ||
        clients.back() != "clients" || settings.back() != "settings" || !config.contains(operations) ||
        slots.contains(operations.to_string()) ||
        !config.at(allow).is_array() || !config.at(clients).is_array() ||
        schematic_field(config.at(api), "type") != live.at("api_node")) {
        return false;
    }
    const auto& names = config.at(operations);
    if (!names.is_array() || names.empty() || !std::ranges::all_of(names, [](const Json& operation) {
            return std::ranges::find(waterwall_users_operations, schematic_text(operation), waterwall_users_operation_name)
                   != waterwall_users_operations.end();
        })) {
        return false;
    }
    const auto& tokens = starter.at("slots").at("token");
    if (std::ranges::find(tokens, Json((client / "token").to_string())) == tokens.end()) {
        return false;
    }
    const auto& target = schematic_field(config.at(grant), "node");
    return !schematic_text(target).empty() && std::ranges::any_of(config.at("nodes"), [&](const Json& node) {
        const auto& type = schematic_field(schematic.at("nodes"), schematic_text(node.at("type")));
        return schematic_field(node, "name") == target && schematic_field(type, "user_database") == true;
    });
}

void validate_users(Json& schematic)
{
    const auto& config = schematic.at("config");
    const auto& nodes = schematic.at("nodes");
    const auto& starter = schematic_field(config, "users_starter");
    const auto& live = schematic_field(config, "live_users");
    const auto& users = schematic_field(schematic.at("ui"), "users");
    const auto& protocol = schematic.at("runtime").at("users_protocol");
    const bool has_database = std::ranges::any_of(
        nodes, [](const Json& node) { return schematic_field(node, "user_database") == true; });
    if (protocol == "none") {
        require(starter.is_null() && live.is_null() && !has_database,
                "User database nodes and integrations require a supported user protocol.");
        return;
    }
    if (has_database) {
        if (auto error = CoreConfigUsers::validate_schematic_editor(users); !error.empty()) {
            throw std::runtime_error(std::move(error));
        }
    }
    const auto known = [&](const Json& type) {
        return type.is_string() && nodes.contains(schematic_text(type));
    };
    if (!live.is_null()) {
        require(known(live.at("api_node")) && known(live.at("listener_node")) &&
                    live.at("tcp_listener_nodes").is_array() &&
                    std::ranges::all_of(live.at("tcp_listener_nodes"), known) &&
                    !schematic_text(schematic_field(users, "live_setup_help")).empty(),
                "Invalid live users integration.");
    }
    if (!starter.is_null()) {
        require(live.is_object() && !schematic_text(schematic_field(users, "starter_label")).empty(),
                "Starter nodes require a live users integration and label.");
        const auto& config_template = starter.at("template");
        require(config_template.at("nodes").is_array() &&
                    std::ranges::all_of(config_template.at("nodes"),
                                        [&](const Json& node) { return known(node.at("type")); }),
                "Invalid user starter node types.");
        std::set<std::string> paths;
        for (const char* key : {"name", "secret", "token", "port"}) {
            const auto& slots = starter.at("slots").at(key);
            require(slots.is_array() && !slots.empty(), "Missing starter config slots.");
            for (const auto& slot : slots) {
                const auto& path = slot.get_ref<const std::string&>();
                require(path.starts_with('/') && paths.insert(path).second &&
                            config_template.contains(Json::json_pointer(path)),
                        "Invalid or duplicate starter config slot.");
            }
        }
        std::vector<std::string> grants;
        for (const auto& name_slot : starter.at("slots").at("name")) {
            const Json::json_pointer name_path(name_slot.get_ref<const std::string&>());
            const auto grant = name_path.parent_pointer();
            if (name_path.back() == "config" && is_generated_management_grant(schematic, starter, live, grant, paths)) {
                grants.push_back((grant / "operations").to_string());
            }
        }
        if (starter.contains("management_operations_slot")) {
            require(std::ranges::find(grants, users_starter_management_slot(schematic)) != grants.end(),
                    "The starter management operations slot must identify the generated client's scoped user-management grant.");
        } else if (grants.size() == 1) {
            // Older cached schematics identify this same grant only through the generated config and token slots.
            schematic.at("config").at("users_starter")["management_operations_slot"] = grants.front();
        }
    }
}

void validate_runtime(Json& schematic)
{
    const auto& runtime = schematic.at("runtime");
    if (runtime.at("startup_protocol") != "config-stdin" || runtime.at("log_protocol") != "dated-files" ||
        (runtime.at("users_protocol") != "auth-json-and-api-v1" && runtime.at("users_protocol") != "none")) {
        throw std::runtime_error("Unsupported WaterWall runtime protocol.");
    }
    const auto& core = schematic.at("core_settings");
    const auto& logs = runtime.at("logs");
    const auto& max_path = logs.at("max_path_bytes");
    require(max_path.is_number_integer() && max_path > 16 && max_path <= 4096 &&
                logs.at("levels").is_object(),
            "Invalid dated log limits or levels.");
    for (const auto& level : logs.at("levels")) {
        require(!schematic_text(level).empty(), "Invalid dated log level.");
    }
    validate_log_path(core, logs.at("directory_path"));
    for (const auto& source : logs.at("sources")) {
        validate_log_path(core, source.at("file_path"));
    }
    validate_users(schematic);
    const auto& packages = schematic.at("packages");
    if (!packages.is_object() || packages.empty()) {
        throw std::runtime_error("No WaterWall release packages are defined.");
    }
    for (const auto& package : packages) {
        if (!archive_name(schematic_text(package.at("archive"))) ||
            !valid_hash(schematic_text(package.at("sha256")))) {
            throw std::runtime_error("Invalid WaterWall release archive name or SHA-256.");
        }
    }
}

} // namespace

std::shared_ptr<const WaterWallSchematic> prepare_waterwall_schematic_file(const fs::path& path)
{
    try {
        std::error_code error;
        if (!fs::is_regular_file(path, error)) {
            throw std::runtime_error("Expected a regular schematic file.");
        }
        std::ifstream input(path, std::ios::binary);
        std::string bytes(max_waterwall_schematic_bytes + 1, '\0');
        input.read(bytes.data(), bytes.size());
        if (!input.eof() || input.bad()) {
            throw std::runtime_error("Cannot read schematic or it exceeds 2 MiB.");
        }
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        bool too_deep = false;
        auto document = Json::parse(
            bytes,
            [&](int depth, auto, Json&) {
                too_deep |= depth > 64;
                return !too_deep;
            },
            false);
        if (too_deep || !document.is_object()) {
            throw std::runtime_error("Expected a valid JSON object within 64 levels of nesting.");
        }
        if (path.filename() != schematic_filename(schematic_text(document.at("version")))) {
            throw std::runtime_error(
                "The filename must match the release version declared in the schematic.");
        }
        validate_runtime(document);
        return prepare_waterwall_schematic(std::move(document));
    } catch (const std::exception& error) {
        throw std::runtime_error("Cannot load WaterWall schematic " + path.string() + ". " + error.what());
    }
}

void load_waterwall_schematic_file(const fs::path& path)
{
    activate_waterwall_schematic(prepare_waterwall_schematic_file(path));
}

fs::path waterwall_bin_directory(const fs::path& backend_directory)
{
    return backend_directory / "waterwall_bin";
}

fs::path waterwall_release_directory(const fs::path& bin_directory, std::string_view version)
{
    return bin_directory / version;
}

fs::path waterwall_schematic_path(const fs::path& bin_directory, std::string_view version)
{
    return waterwall_release_directory(bin_directory, version) / schematic_filename(version);
}

std::shared_ptr<const WaterWallSchematic> prepare_waterwall_release_schematic(const fs::path& bin_directory,
                                                                            std::string_view version)
{
    if (!valid_waterwall_version(version)) {
        throw std::runtime_error("Invalid WaterWall release version.");
    }
    const auto path = waterwall_schematic_path(bin_directory, version);
    if (!trusted_path(bin_directory, S_IFDIR) || !trusted_path(path.parent_path(), S_IFDIR) ||
        !trusted_path(path, S_IFREG)) {
        throw std::runtime_error("Cannot load WaterWall schematic " + path.string() +
                                 ". Expected a regular schematic file in a release directory, both owned by root "
                                 "or Wave's user and writable only by their owner.");
    }
    return prepare_waterwall_schematic_file(path);
}

void select_waterwall_schematic(const fs::path& directory, const Json& settings)
{
    const auto selected = settings.find("waterwall_version");
    const auto version = selected == settings.end() ? bundled_waterwall_version : schematic_text(*selected);
    if (!valid_waterwall_version(version)) {
        throw std::runtime_error("waterwall_version in wave-db.json must be a release version such as " +
                                 std::string(bundled_waterwall_version) + '.');
    }
    const auto bin_directory = waterwall_bin_directory(directory);
    if (!prepare_directory(bin_directory)) {
        throw std::runtime_error("Cannot create or use WaterWall directory " + bin_directory.string() + '.');
    }
    migrate_legacy_schematics(bin_directory / "schematics", bin_directory);
    migrate_legacy_schematics(directory / "schematics", bin_directory);
    const auto bundled = waterwall_schematic_path(bin_directory, bundled_waterwall_version);
    seed_schematic(bundled, write_bundled_schematic);
    auto schematic = prepare_waterwall_release_schematic(bin_directory, version);
    migrate_legacy_binary(bin_directory);
    activate_waterwall_schematic(std::move(schematic));
}

} // namespace wave
