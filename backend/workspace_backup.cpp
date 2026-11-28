#include "workspace_backup.h"

#include "core_config_files.h"
#include "core_config_graph.h"
#include "core_config_name.h"
#include "core_config_users.h"
#include "core_settings.h"
#include "panel_settings.h"
#include "private_temp_directory.h"
#include "waterwall_schematic.h"
#include "waterwall_schematic_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr auto manifest_path = "manifest.json";
constexpr auto format = "wave-workspace-backup";
constexpr auto journal_directory = ".wave-restore";
constexpr std::string_view staging_prefix = ".wave-restore-stage-";
constexpr std::string_view completed_prefix = ".wave-restore-complete-";
constexpr std::string_view payload_prefix = ".wave-workspace-restore-staged-files-";
constexpr auto publishing_filename = "publishing";
constexpr auto journal_filename = "journal.json";
constexpr std::size_t max_metadata_bytes = 2 * 1024 * 1024;
static_assert(payload_prefix.size() + 32 > max_core_config_name_bytes);

class File {
  public:
    explicit File(int fd) : fd_(fd) {}
    ~File()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int get() const { return fd_; }

  private:
    int fd_;
};

bool trusted_mode(const struct stat& info, mode_t type, bool private_file)
{
    return (info.st_mode & S_IFMT) == type && (info.st_uid == ::geteuid() || info.st_uid == 0) &&
           !(info.st_mode & (private_file ? 0077 : 0022));
}

std::optional<struct stat> path_info(const fs::path& path, mode_t type, bool private_file = false)
{
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
    } else if (trusted_mode(info, type, private_file)) {
        return info;
    }
    throw std::runtime_error("A workspace backup path is unsafe or inaccessible.");
}

bool exists_as(const fs::path& path, mode_t type, bool private_file = false)
{
    return path_info(path, type, private_file).has_value();
}

std::optional<std::string> read_file(const fs::path& path, std::size_t limit, bool private_file)
{
    File file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0 && errno == ENOENT) {
        return std::nullopt;
    }
    struct stat info{};
    if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !trusted_mode(info, S_IFREG, private_file) ||
        (private_file && (info.st_nlink != 1 || info.st_uid != ::geteuid()))) {
        throw std::runtime_error("A workspace backup file is unsafe or inaccessible.");
    }
    std::string bytes;
    std::array<char, 8192> buffer;
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw std::runtime_error("Could not read a workspace backup file.");
        }
        if (count == 0) {
            return bytes;
        }
        if (static_cast<std::size_t>(count) > limit - bytes.size()) {
            throw std::runtime_error("A workspace backup file exceeds its size limit.");
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

void sync_directory(const fs::path& path)
{
    File directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0 || ::fsync(directory.get()) != 0) {
        throw std::runtime_error("Could not sync the workspace restore directory.");
    }
}

void write_file(const fs::path& path, std::string_view bytes)
{
    File file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) {
        throw std::runtime_error("Could not stage a workspace restore file.");
    }
    while (!bytes.empty()) {
        const auto count = ::write(file.get(), bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Could not write a workspace restore file.");
        }
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    if (::fsync(file.get()) != 0) {
        throw std::runtime_error("Could not sync a workspace restore file.");
    }
}

Json parse_object(std::string_view bytes, std::size_t limit, std::string_view subject)
{
    std::string error;
    if (bytes.size() <= limit) {
        auto value = parse_plain_core_config_json(bytes, "Invalid JSON.", error);
        if (error.empty() && value.is_object()) {
            return value;
        }
    }
    throw std::runtime_error("Invalid " + std::string(subject) + " in the workspace backup.");
}

bool valid_config_name(std::string_view name)
{
    return !name.empty() && core_config_name(name) == name;
}

bool valid_workspace_path(std::string_view name)
{
    constexpr std::string_view prefix = "configs/";
    if (!name.starts_with(prefix)) {
        return false;
    }
    name.remove_prefix(prefix.size());
    const auto slash = name.find('/');
    if (slash == name.npos || !valid_config_name(name.substr(0, slash))) {
        return false;
    }
    auto filename = name.substr(slash + 1);
    if (filename == "config.json") {
        return true;
    }
    constexpr std::string_view recovery_suffix = ".backup";
    if (filename.ends_with(recovery_suffix)) {
        filename.remove_suffix(recovery_suffix.size());
    }
    return CoreConfigUsers::valid_filename(filename);
}

void validate_manifest(const Json& manifest)
{
    const auto& configs = schematic_field(manifest, "configs");
    const auto& settings = schematic_field(manifest, "core_settings");
    const auto& version = schematic_field(manifest, "version");
    if (schematic_field(manifest, "format") != format || !version.is_number_integer() || version != 1 ||
        !valid_waterwall_version(schematic_text(schematic_field(manifest, "waterwall_version"))) ||
        !settings.is_object() || settings.dump().size() > core_settings::max_bytes) {
        throw std::runtime_error("Invalid or unsupported Wave workspace backup manifest.");
    }
    if (const auto error = core_config_registry_error(configs, true); !error.empty()) {
        throw std::runtime_error("The workspace backup config registry " + std::string(error));
    }
}

struct ConfigDatabases {
    std::vector<std::string> paths;
    bool relocated = false;
};

ConfigDatabases config_databases(Json& config, std::string_view name, const fs::path& directory,
                                 const Json& schematic, bool restoring)
{
    ConfigDatabases databases;
    std::set<std::string> seen;
    for (auto& node : config.at("nodes")) {
        const auto type = schematic_text(schematic_field(node, "type"));
        const auto& definition = schematic_field(schematic_field(schematic, "nodes"), type);
        if (schematic_field(definition, "user_database") != true) {
            continue;
        }
        const auto key = schematic_text(schematic_field(definition, "database_key"));
        const auto configured = schematic_text(schematic_field(schematic_field(node, "settings"), key));
        auto filename = auth_database_name(node, name, directory, definition);
        const fs::path path(configured);
        if (restoring && path.is_absolute()) {
            const auto normalized = path.lexically_normal();
            const auto candidate = normalized.filename().string();
            // Absolute paths exported from another host can only name the same managed file.
            if (CoreConfigUsers::valid_filename(candidate) &&
                normalized.parent_path().filename() == name &&
                normalized.parent_path().parent_path().filename() == "configs") {
                filename = candidate;
            }
            if (!filename.empty()) {
                node["settings"][std::string(key)] = CoreConfigUsers::relative_path(name, filename).generic_string();
                databases.relocated = true;
            }
        }
        if (filename.empty() || !seen.insert(filename).second) {
            throw std::runtime_error("Config " + std::string(name) +
                                     " needs a unique AuthenticationServer database inside its config directory.");
        }
        databases.paths.push_back(CoreConfigUsers::relative_path(name, filename).generic_string());
    }
    return databases;
}

void validate_config(const Json& config, std::string_view name, bool enabled, const WaterWallSchematic* schematic)
{
    // A disabled draft may retain node types from the release that authored it.
    static const Json empty;
    if (auto issue = validate_config_graph(config, enabled ? empty : config, schematic); !issue.empty()) {
        throw std::runtime_error("Config " + std::string(name) + ": " + issue);
    }
}

void check_config_compatibility(std::vector<std::string>& warnings, const Json& config, std::string_view name,
                                const Json& schematic)
{
    const auto& definitions = schematic_field(schematic, "nodes");
    std::set<std::string_view> unknown;
    for (const auto& node : config.at("nodes")) {
        const auto type = schematic_text(node.at("type"));
        if (!schematic_field(definitions, type).is_object() && unknown.insert(type).second) {
            const auto version = schematic_text(schematic.at("version"));
            warnings.push_back("Config " + std::string(name) + " contains node type " + std::string(type) +
                               ", which WaterWall " + std::string(version) + " does not support. "
                               "Edit the config or select a compatible release before enabling it.");
        }
    }
}

void validate_database_pair(const BackupEntries& entries, const std::string& primary, const std::string& recovery,
                            std::string_view config)
{
    // WaterWall parses the recovery file only when the primary is absent or malformed.
    // Keep any safe, present file in the archive exactly as it was read.
    for (const auto* path : {&primary, &recovery}) {
        const auto file = entries.find(*path);
        if (file != entries.end() && CoreConfigUsers::valid_backup_database(file->second)) {
            return;
        }
    }
    if (!entries.contains(primary) && !entries.contains(recovery)) {
        throw std::runtime_error("An AuthenticationServer database for " + std::string(config) + " is missing.");
    }
    throw std::runtime_error("Config " + std::string(config) +
                             " needs a valid AuthenticationServer database or recovery file.");
}

void validate_core_settings(const Json& settings, const Json& schema)
{
    const auto normalized = core_settings::normalize(settings, schematic_field(schema, "core_settings"));
    if (!normalized.issue.empty()) {
        throw std::runtime_error("The workspace has invalid WaterWall core settings. " + normalized.issue);
    }
}

void check_dependency(std::vector<std::string>& missing, const BackupEntries& entries, const fs::path& directory,
                      const Json& setting, std::string_view owner, std::string_view name, bool is_directory = false)
{
    const auto value = schematic_text(setting);
    if (value.empty()) {
        return;
    }
    const auto path = (directory / fs::path(value)).lexically_normal();
    const auto relative = path.lexically_relative(directory);
    std::error_code error;
    if (!entries.contains(relative.generic_string()) &&
        !(is_directory ? fs::is_directory(path, error) : fs::is_regular_file(path, error))) {
        missing.push_back(std::string(owner) + " needs " + std::string(name) + " at " + std::string(value) + '.');
    }
}

bool has_router_token(const Json& patterns, std::string_view prefix, bool allow_negation)
{
    const auto matches = [=](const Json& value) {
        auto pattern = schematic_text(value);
        if (allow_negation && pattern.starts_with('!')) {
            pattern.remove_prefix(1);
        }
        return std::ranges::equal(pattern.substr(0, prefix.size()), prefix, [](char actual, char expected) {
            return (actual >= 'A' && actual <= 'Z' ? actual - 'A' + 'a' : actual) == expected;
        });
    };
    return patterns.is_array() ? std::ranges::any_of(patterns, matches) : matches(patterns);
}

void check_router_dependencies(std::vector<std::string>& missing, const BackupEntries& entries,
                               const fs::path& directory, const Json& settings, std::string_view owner)
{
    const auto& rules = schematic_field(settings, "rules");
    if (!rules.is_array()) {
        return;
    }
    const auto check = [&](const char* key) {
        const auto& setting = schematic_field(settings, key);
        if (schematic_text(setting).empty()) {
            missing.push_back(std::string(owner) + " needs a path for the Router setting " + key + '.');
        } else {
            check_dependency(missing, entries, directory, setting, owner, key);
        }
    };
    if (std::ranges::any_of(rules, [](const Json& rule) {
            return has_router_token(schematic_field(rule, "source-ips"), "geoip:", true) ||
                   has_router_token(schematic_field(rule, "destination-ip"), "geoip:", true);
        })) {
        check("geoip-db-path");
    }
    if (std::ranges::any_of(rules, [](const Json& rule) {
            return has_router_token(schematic_field(rule, "destination-domain"), "geosite:", false);
        })) {
        check("geosite-db-path");
    }
}

struct RestoreFile {
    std::string path;
    bool old_file;
    bool new_file;
};

bool valid_restore_id(std::string_view id)
{
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == id.npos;
}

fs::path restore_payload(const fs::path& root, const fs::path& folder, const std::set<std::string>& new_directories,
                         std::string_view id)
{
    // An existing config directory can be its own mount. New directories are created on the configs mount.
    const auto& parent = new_directories.contains(folder.filename().string()) ? root : folder;
    return parent / (std::string(payload_prefix) + std::string(id));
}

void discard_journal(const fs::path& directory, const fs::path& journal)
{
    // Remove the recovery record atomically before deleting its contents.
    sync_directory(directory);
    PrivateTempDirectory completed(directory, completed_prefix);
    const auto path = completed.path();
    if (path.empty()) {
        throw std::runtime_error("Could not retire the workspace restore journal.");
    }
    fs::rename(journal, path / "restore");
    completed.release();
    sync_directory(path);
    sync_directory(directory);
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

void roll_back(const fs::path& directory, const fs::path& journal, const std::vector<RestoreFile>& files,
               const std::set<std::string>& new_directories, std::string_view payload_id)
{
    const auto root = directory / "configs";
    if (!exists_as(root, S_IFDIR)) {
        throw std::runtime_error("The config directory is missing during workspace restore recovery.");
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        const auto destination = directory / file.path;
        if (!exists_as(destination.parent_path(), S_IFDIR)) {
            if (file.old_file) {
                throw std::runtime_error("A config directory required for restore recovery is missing.");
            }
            continue;
        }
        if (file.old_file) {
            const auto payload = payload_id.empty()
                                     ? journal
                                     : restore_payload(root, destination.parent_path(), new_directories, payload_id);
            const auto old = payload / (std::to_string(i) + ".old");
            if (exists_as(payload, S_IFDIR, true) && exists_as(old, S_IFREG)) {
                if (payload_id.empty()) {
                    const auto undo = journal / "undo";
                    fs::remove(undo);
                    fs::create_hard_link(old, undo);
                    fs::rename(undo, destination);
                } else {
                    fs::rename(old, destination);
                    sync_directory(destination.parent_path());
                    sync_directory(payload);
                    continue;
                }
            } else if (payload_id.empty() || !exists_as(destination, S_IFREG)) {
                throw std::runtime_error("A previous workspace file required for recovery is missing.");
            }
        } else if (exists_as(destination, S_IFREG)) {
            fs::remove(destination);
        }
        sync_directory(destination.parent_path());
    }
    for (const auto& name : new_directories) {
        const auto path = root / name;
        if (exists_as(path, S_IFDIR) && fs::is_empty(path)) {
            fs::remove(path);
        }
    }
    sync_directory(root);
}

void recover_journal(const fs::path& directory, const fs::path& journal, const Json& committed)
{
    const auto bytes = read_file(journal / journal_filename, max_metadata_bytes, true);
    if (!bytes) {
        throw std::runtime_error("The workspace restore journal is incomplete.");
    }
    const auto record = parse_object(*bytes, max_metadata_bytes, "restore journal");
    const auto& id = schematic_field(record, "id");
    const auto version = record.find("version");
    const auto stage_on_destination = version != record.end();
    const auto payload_id = stage_on_destination ? schematic_text(id) : std::string_view{};
    const auto& stored_files = schematic_field(record, "files");
    const auto& stored_directories = schematic_field(record, "new_directories");
    if (!id.is_string() || schematic_text(id).empty() || !stored_files.is_array() ||
        (stage_on_destination && (*version != 2 || !valid_restore_id(payload_id))) ||
        stored_files.size() > max_backup_entries * 2 || !stored_directories.is_array() ||
        stored_directories.size() > max_workspace_configs) {
        throw std::runtime_error("The workspace restore journal is invalid.");
    }
    std::vector<RestoreFile> files;
    std::set<std::string> directories;
    for (const auto& row : stored_files) {
        const auto name = schematic_text(schematic_field(row, "path"));
        const auto& old_file = schematic_field(row, "old_file");
        if (!valid_workspace_path(name) || !old_file.is_boolean()) {
            throw std::runtime_error("The workspace restore journal contains an invalid file.");
        }
        files.push_back({std::string(name), old_file.get<bool>(), false});
    }
    for (const auto& row : stored_directories) {
        const auto name = schematic_text(row);
        if (!valid_config_name(name)) {
            throw std::runtime_error("The workspace restore journal contains an invalid directory.");
        }
        directories.emplace(name);
    }
    if (id != committed && (!stage_on_destination || exists_as(journal / publishing_filename, S_IFREG, true))) {
        roll_back(directory, journal, files, directories, payload_id);
    }
    if (stage_on_destination) {
        // The database writer can report a successful rename after a failed directory sync.
        // Keep rollback files until that commit is durable on the database filesystem.
        if (id == committed) {
            sync_directory(directory);
        }
        const auto root = directory / "configs";
        if (!exists_as(root, S_IFDIR)) {
            throw std::runtime_error("The config directory is missing during workspace restore recovery.");
        }
        std::set<fs::path> payloads;
        for (const auto& file : files) {
            const auto folder = (directory / file.path).parent_path();
            if (exists_as(folder, S_IFDIR) || directories.contains(folder.filename().string())) {
                payloads.insert(restore_payload(root, folder, directories, payload_id));
            }
        }
        for (const auto& payload : payloads) {
            if (exists_as(payload, S_IFDIR, true)) {
                fs::remove_all(payload);
                sync_directory(payload.parent_path());
            }
        }
    }
    discard_journal(directory, journal);
}

} // namespace

BackupEntries capture_workspace_backup(const fs::path& directory, const Json& settings, const Json& configs)
{
    const Json manifest = {{"format", format}, {"version", 1}, {"waterwall_version", waterwall_version()},
                           {"core_settings", settings}, {"configs", configs}};
    validate_manifest(manifest);
    validate_core_settings(settings, waterwall_schematic());

    BackupEntries entries{{manifest_path, manifest.dump(2) + '\n'}};
    std::size_t size = entries.begin()->second.size();
    const auto capture = [&](const std::string& path, std::size_t limit, bool private_file) {
        auto bytes = read_file(directory / path, limit, private_file);
        if (!bytes) {
            return false;
        }
        if (bytes->size() > max_backup_archive_bytes - size || entries.size() >= max_backup_entries) {
            throw std::runtime_error("This workspace exceeds the backup size limit.");
        }
        size += bytes->size();
        entries.emplace(path, std::move(*bytes));
        return true;
    };

    const auto root = directory / "configs";
    if (!configs.empty() && !exists_as(root, S_IFDIR)) {
        throw std::runtime_error("The workspace config directory is missing.");
    }
    for (const auto& row : configs) {
        const auto name = schematic_text(row.at("name"));
        const auto path = CoreConfigFiles::relative_path(name).generic_string();
        if (!exists_as(root / name, S_IFDIR) || !capture(path, max_core_config_bytes, false)) {
            throw std::runtime_error("Config " + std::string(name) + " is missing from disk.");
        }
        auto config = parse_object(entries.at(path), max_core_config_bytes, "config file");
        validate_config(config, name, row.at("enabled").get<bool>(), nullptr);
        for (const auto& database : config_databases(config, name, directory, waterwall_schematic(), false).paths) {
            const auto recovery = database + ".backup";
            capture(database, max_backup_entry_bytes, true);
            capture(recovery, max_backup_entry_bytes, true);
            validate_database_pair(entries, database, recovery, name);
        }
    }
    return entries;
}

WorkspaceBackup prepare_workspace_backup(const fs::path& directory, BackupEntries entries)
{
    const auto manifest_file = entries.find(manifest_path);
    if (manifest_file == entries.end()) {
        throw std::runtime_error("This ZIP has no Wave workspace backup manifest.");
    }
    auto manifest = parse_object(manifest_file->second, max_metadata_bytes, "manifest");
    validate_manifest(manifest);
    entries.erase(manifest_file);
    const auto version = schematic_text(manifest.at("waterwall_version"));
    std::shared_ptr<const WaterWallSchematic> schematic;
    try {
        schematic = prepare_waterwall_release_schematic(waterwall_bin_directory(directory), version);
    } catch (const std::runtime_error&) {
        throw std::runtime_error("WaterWall " + std::string(version) +
                                 " needs a valid matching schematic before restoring this backup.");
    }
    const auto& schema = waterwall_schematic(*schematic);
    validate_core_settings(manifest.at("core_settings"), schema);

    std::set<std::string> expected;
    std::vector<std::string> absent;
    std::vector<std::string> missing;
    std::vector<std::string> warnings;
    const auto& settings = manifest.at("core_settings");
    const auto& dns = schematic_field(settings, "dns");
    check_dependency(missing, entries, directory, schematic_field(schematic_field(settings, "misc"), "libs-path"),
                     "Core settings", "the tunnel library directory", true);
    check_dependency(missing, entries, directory, schematic_field(dns, "resolvconf-path"), "Core settings",
                     "the resolver configuration file");
    check_dependency(missing, entries, directory, schematic_field(dns, "hosts-path"), "Core settings", "the hosts file");
    for (const auto& row : manifest.at("configs")) {
        const auto name = schematic_text(row.at("name"));
        const auto path = CoreConfigFiles::relative_path(name).generic_string();
        auto file = entries.find(path);
        if (file == entries.end()) {
            throw std::runtime_error("The backup is missing the config file for " + std::string(name) + '.');
        }
        expected.insert(path);
        auto config = parse_object(file->second, max_core_config_bytes, "config file");
        const bool enabled = row.at("enabled").get<bool>();
        validate_config(config, name, enabled, schematic.get());
        if (!enabled) {
            check_config_compatibility(warnings, config, name, schema);
        }
        const auto databases = config_databases(config, name, directory, schema, true);
        for (const auto& node : config.at("nodes")) {
            const auto& settings = schematic_field(node, "settings");
            for (const auto* key : {"cert-file", "key-file", "ca-file"}) {
                check_dependency(missing, entries, directory, schematic_field(settings, key), name, key);
            }
            if (schematic_field(node, "type") == "Router") {
                check_router_dependencies(missing, entries, directory, settings, name);
            }
        }
        for (const auto& database : databases.paths) {
            const auto recovery = database + ".backup";
            validate_database_pair(entries, database, recovery, name);
            for (const auto& candidate : {database, recovery}) {
                if (const auto found = entries.find(candidate); found != entries.end()) {
                    expected.insert(candidate);
                } else {
                    absent.push_back(candidate);
                }
            }
        }
        if (databases.relocated) {
            file->second = config.dump(2) + '\n';
            if (file->second.size() > max_core_config_bytes) {
                throw std::runtime_error("A relocated config exceeds the config size limit.");
            }
        }
    }
    if (entries.size() != expected.size()) {
        throw std::runtime_error("The workspace backup contains an unexpected file.");
    }
    return {std::move(manifest), std::move(entries), std::move(absent), std::move(missing), std::move(warnings),
            std::move(schematic)};
}

void recover_workspace_restore(const PanelDatabase& database)
{
    const auto& directory = database.directory();
    const auto journal = directory / journal_directory;
    if (exists_as(journal, S_IFDIR, true)) {
        recover_journal(directory, journal, schematic_field(database.initial_contents(), "workspace_restore_id"));
    }
    // The database lock makes abandoned staging directories safe to retire at startup.
    std::error_code error;
    const fs::directory_iterator end;
    for (fs::directory_iterator entry(directory, error); !error && entry != end; entry.increment(error)) {
        const auto name = entry->path().filename().string();
        const auto staged = [&](std::string_view prefix) {
            return name.starts_with(prefix) && name.size() == prefix.size() + 6;
        };
        struct stat info{};
        if ((staged(staging_prefix) || staged(completed_prefix)) && ::lstat(entry->path().c_str(), &info) == 0 &&
            info.st_uid == ::geteuid() && trusted_mode(info, S_IFDIR, true)) {
            std::error_code ignored;
            fs::remove_all(entry->path(), ignored);
        }
    }
}

WorkspaceRestoreFiles existing_workspace_restore_files(const fs::path& directory, const WorkspaceBackup& backup)
{
    if (!exists_as(directory / "configs", S_IFDIR)) {
        throw std::runtime_error("The workspace config directory is missing.");
    }
    WorkspaceRestoreFiles files;
    const auto inspect = [&](const std::string& path) {
        const auto destination = directory / path;
        if (exists_as(destination.parent_path(), S_IFDIR)) {
            if (const auto file = path_info(destination, S_IFREG, !path.ends_with("/config.json"))) {
                // Include metadata changes, in-place edits, and file replacement in the precondition.
                files.emplace(path, std::array<std::uintmax_t, 5>{
                    file->st_dev, file->st_ino, static_cast<std::uintmax_t>(file->st_size),
                    static_cast<std::uintmax_t>(file->st_ctim.tv_sec),
                    static_cast<std::uintmax_t>(file->st_ctim.tv_nsec)});
            }
        }
    };
    for (const auto& [path, contents] : backup.files) {
        inspect(path);
    }
    for (const auto& path : backup.absent_files) {
        inspect(path);
    }
    return files;
}

void apply_workspace_backup(PanelDatabase& database, const WorkspaceBackup& backup, const std::string& restore_id)
{
    const auto& directory = database.directory();
    const auto journal = directory / journal_directory;
    const auto root = directory / "configs";
    if (exists_as(journal, S_IFDIR, true)) {
        throw std::runtime_error("Restart Wave to finish recovery from a previous restore.");
    }
    if (!exists_as(root, S_IFDIR)) {
        throw std::runtime_error("The workspace config directory is missing.");
    }
    if (!valid_restore_id(restore_id)) {
        throw std::runtime_error("The workspace restore ID is invalid.");
    }
    const PrivateTempDirectory stage(directory, staging_prefix);
    if (stage.path().empty()) {
        throw std::runtime_error("Could not stage the workspace restore.");
    }

    std::vector<RestoreFile> files;
    std::set<std::string> new_directories;
    auto recorded_files = Json::array();
    const auto stage_file = [&](const std::string& path, const std::string* bytes) {
        const auto destination = directory / path;
        const auto folder = destination.parent_path();
        const auto had_directory = exists_as(folder, S_IFDIR);
        const auto old_file = had_directory && exists_as(destination, S_IFREG, !path.ends_with("/config.json"));
        if (!bytes && !old_file) {
            return;
        }
        if (!had_directory) {
            new_directories.insert(folder.filename().string());
        }
        files.push_back({path, old_file, bytes != nullptr});
        recorded_files.push_back({{"path", path}, {"old_file", old_file}});
    };
    for (const auto& [path, bytes] : backup.files) {
        stage_file(path, &bytes);
    }
    for (const auto& path : backup.absent_files) {
        stage_file(path, nullptr);
    }

    const Json record = {{"version", 2}, {"id", restore_id}, {"files", std::move(recorded_files)},
                         {"new_directories", new_directories}};
    const auto serialized = record.dump();
    if (serialized.size() > max_metadata_bytes) {
        throw std::runtime_error("The workspace restore journal exceeds its size limit.");
    }
    write_file(stage.path() / journal_filename, serialized);
    sync_directory(stage.path());
    fs::rename(stage.path(), journal);
    try {
        // Persist the locations of all private payload directories before creating any of them.
        sync_directory(directory);
        std::set<fs::path> payloads;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];
            const auto folder = (directory / file.path).parent_path();
            const auto payload = restore_payload(root, folder, new_directories, restore_id);
            if (payloads.insert(payload).second && ::mkdir(payload.c_str(), 0700) != 0) {
                throw std::runtime_error("Could not stage workspace files on the destination filesystem.");
            }
            if (file.new_file) {
                write_file(payload / (std::to_string(i) + ".new"), backup.files.at(file.path));
            }
        }
        for (const auto& payload : payloads) {
            sync_directory(payload);
            sync_directory(payload.parent_path());
        }
        write_file(journal / publishing_filename, {});
        sync_directory(journal);

        for (const auto& name : new_directories) {
            if (::mkdir((root / name).c_str(), 0700) != 0) {
                throw std::runtime_error("Could not create a restored config directory.");
            }
        }
        std::set<fs::path> changed;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];
            const auto destination = directory / file.path;
            const auto payload = restore_payload(root, destination.parent_path(), new_directories, restore_id);
            const auto index = std::to_string(i);
            if (file.old_file) {
                // Keep the actual old inode in its own filesystem. This also works for a readable
                // root-owned file and preserves its ownership and mode if recovery is needed.
                fs::rename(destination, payload / (index + ".old"));
                sync_directory(payload);
                sync_directory(destination.parent_path());
            }
            if (file.new_file) {
                fs::rename(payload / (index + ".new"), destination);
            }
            changed.insert(destination.parent_path());
        }
        for (const auto& folder : changed) {
            sync_directory(folder);
        }
        sync_directory(root);
        database.save_workspace_restore(backup.manifest.at("waterwall_version"), backup.manifest.at("core_settings"),
                                        backup.manifest.at("configs"), restore_id);
    } catch (...) {
        recover_journal(directory, journal, nullptr);
        throw;
    }

    try {
        recover_journal(directory, journal, restore_id);
    } catch (const std::runtime_error&) {
        // The database records the commit and startup can finish journal cleanup.
    }
}

} // namespace wave
