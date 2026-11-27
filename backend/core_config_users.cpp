#include "core_config_users.h"

#include "core_config_name.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t max_database_bytes = 16 * 1024 * 1024;
constexpr std::size_t users_per_page = 50;
constexpr std::size_t max_public_user_bytes = 16 * 1024;
constexpr std::string_view whitespace = " \n\r\t\v\f";

class UserError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class File {
  public:
    explicit File(int fd) : fd_(fd) {}
    ~File()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    int get() const { return fd_; }

    bool close_file() { return close(std::exchange(fd_, -1)) == 0; }

  private:
    int fd_;
};

std::string_view trim(std::string_view value)
{
    const auto first = value.find_first_not_of(whitespace);
    if (first == value.npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

std::optional<std::uint64_t> decimal(std::string_view text)
{
    if (!text.empty()) {
        std::uint64_t number = 0;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, number);
        if (parsed.ec == std::errc{} && parsed.ptr == end) {
            return number;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_value(const Json& value, bool from_disk)
{
    if (const auto* string = value.get_ptr<const std::string*>()) {
        const auto number = decimal(from_disk ? trim(*string) : std::string_view(*string));
        if (number && (from_disk || std::to_string(*number) == *string)) {
            return number;
        }
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (!from_disk || number <= std::numeric_limits<std::int64_t>::max()) {
            return number;
        }
    } else if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return value.get<std::uint64_t>();
    }
    return std::nullopt;
}

struct Network {
    int family;
    std::array<unsigned char, 16> first{};
    std::array<unsigned char, 16> last{};
    std::string canonical;
};

Network parse_network(std::string_view input)
{
    const auto separator = input.find('/');
    const auto address = std::string(input.substr(0, separator));
    const int family = address.find(':') == address.npos ? AF_INET : AF_INET6;
    const int bytes = family == AF_INET ? 4 : 16;
    const auto prefix = separator == input.npos ? std::nullopt : decimal(input.substr(separator + 1));
    Network network{family, {}, {}, {}};
    if (!prefix || *prefix > static_cast<unsigned>(bytes * 8) ||
        inet_pton(family, address.c_str(), network.first.data()) != 1) {
        throw UserError("WireGuard allowed IPs must contain one IPv4 or IPv6 CIDR.");
    }
    for (int i = 0; i < bytes; ++i) {
        const auto network_bits = std::clamp(static_cast<int>(*prefix) - i * 8, 0, 8);
        const auto mask = static_cast<unsigned char>(0xffu << (8 - network_bits));
        network.first[i] &= mask;
        network.last[i] = network.first[i] | static_cast<unsigned char>(~mask);
    }
    std::array<char, INET6_ADDRSTRLEN> normalized{};
    if (!inet_ntop(family, network.first.data(), normalized.data(), normalized.size())) {
        throw UserError("Could not normalize the WireGuard allowed IP range.");
    }
    network.canonical = std::string(normalized.data()) + '/' + std::to_string(*prefix);
    return network;
}

using Address = std::array<unsigned char, 16>;
void insert_network(const Network& network, std::map<Address, Address>& ranges)
{
    const auto next = ranges.lower_bound(network.first);
    if ((next != ranges.end() && next->first <= network.last) ||
        (next != ranges.begin() && std::prev(next)->second >= network.first)) {
        throw UserError("Two users cannot have overlapping WireGuard allowed IPs.");
    }
    ranges.emplace_hint(next, network.first, network.last);
}

std::string uuid_key(std::string_view value)
{
    if (value.size() != 32 && value.size() != 36) {
        return {};
    }
    std::string key;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (value.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (c != '-') {
                return {};
            }
        } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            key += c;
        } else if (c >= 'A' && c <= 'F') {
            key += static_cast<char>(c + ('a' - 'A'));
        } else {
            return {};
        }
    }
    return key;
}

enum class Kind { text, password, cidr, boolean, uint64, interval, object };
enum class ReadMode { disk, create, edit };
enum class Access { edit, create, read_only };

struct Field {
    std::string_view key;
    Kind kind;
    std::string_view alias = {};
    std::span<const Field> children = {};
    bool alias_first = false;
    Access access = Access::edit;
};

constexpr Field directions[] = {{"up", Kind::uint64, "u", {}, true}, {"down", Kind::uint64, "d", {}, true}};
constexpr Field traffic_limit[] = {directions[0], directions[1], {"total", Kind::uint64}};
constexpr Field counters[] = {{"ips", Kind::uint64, "ip", {}, true},
                              {"devices", Kind::uint64},
                              {"connections-in", Kind::uint64, "cons-in", {}, true},
                              {"connections-out", Kind::uint64, "cons-out", {}, true}};
constexpr Field limits[] = {{"traffic", Kind::object, {}, traffic_limit},
                            {"bandwidth", Kind::object, {}, directions},
                            counters[0],
                            counters[1],
                            counters[2],
                            counters[3]};
constexpr Field times[] = {{"created-at-ms", Kind::uint64, "created_at_ms", {}, false, Access::read_only},
                           {"first-usage-at-ms", Kind::uint64, "first_usage_at_ms", {}, false, Access::read_only},
                           {"expire-at-ms", Kind::uint64, "expires-at-ms"},
                           {"expire-after-first-usage-ms", Kind::uint64, "expire-after-first-use-ms"}};
constexpr Field stats[] = {{"traffic", Kind::object, {}, directions},
                           {"speed", Kind::object, {}, directions},
                           counters[0],
                           counters[1],
                           counters[2],
                           counters[3]};
constexpr Field user_fields[] = {{"id", Kind::uint64, "user-id", {}, false, Access::create},
                                 {"password", Kind::password, "pass"},
                                 {"name", Kind::text},
                                 {"email", Kind::text},
                                 {"notes", Kind::text},
                                 {"wireguard-allowed-ips", Kind::cidr},
                                 {"enabled", Kind::boolean, "enable"},
                                 {"gid", Kind::uint64, "group-id"},
                                 {"record-stat-interval-ms", Kind::interval},
                                 {"limit", Kind::object, {}, limits},
                                 {"time", Kind::object, {}, times},
                                 {"stats", Kind::object, {}, stats, false, Access::read_only}};
constexpr auto& password_field = user_fields[1];

const Json* select_field(const Json& object, const Field& field, bool from_disk)
{
    const auto primary = object.find(field.key);
    if (from_disk && !field.alias.empty() && (field.alias_first || primary == object.end())) {
        const auto alias = object.find(field.alias);
        if (alias != object.end()) {
            return &*alias;
        }
    }
    return primary != object.end() ? &*primary : nullptr;
}

Json read_fields(const Json& object, std::span<const Field> fields, ReadMode mode,
                 std::string_view path = {});

Json read_value(const Json& value, const Field& field, ReadMode mode, const std::string& path)
{
    const bool from_disk = mode == ReadMode::disk;
    const auto* string = value.get_ptr<const std::string*>();
    if (from_disk && field.kind != Kind::password &&
        (value.is_null() ||
         (string && string->empty() && field.kind != Kind::text && field.kind != Kind::cidr))) {
        return nullptr;
    }
    if (mode == ReadMode::edit && value.is_null() &&
        (field.kind == Kind::uint64 || field.kind == Kind::interval)) {
        return nullptr;
    }
    switch (field.kind) {
    case Kind::text:
    case Kind::password:
    case Kind::cidr:
        if (string && string->find('\0') == string->npos &&
            (field.kind != Kind::password || !string->empty())) {
            if (field.kind == Kind::cidr) {
                const auto cidr = trim(*string);
                return cidr.empty() ? std::string{} : parse_network(cidr).canonical;
            }
            return *string;
        }
        break;
    case Kind::boolean:
        if (value.is_boolean()) {
            return value;
        }
        break;
    case Kind::uint64:
    case Kind::interval:
        if (const auto number = unsigned_value(value, from_disk);
            number && (field.kind == Kind::uint64 || *number <= std::numeric_limits<int>::max())) {
            return std::to_string(*number);
        }
        break;
    case Kind::object:
        if (value.is_object() && (from_disk || !value.empty())) {
            return read_fields(value, field.children, mode, path + '.');
        }
        break;
    }
    throw UserError("Invalid value for " + path + '.');
}

Json read_fields(const Json& object, std::span<const Field> fields, ReadMode mode, std::string_view path)
{
    if (!object.is_object()) {
        throw UserError("A user record must be an object.");
    }
    const bool from_disk = mode == ReadMode::disk;
    Json normalized = Json::object();
    for (const auto& field : fields) {
        const bool writable = field.access == Access::edit ||
                              (mode == ReadMode::create && field.access == Access::create);
        if (from_disk ? field.kind == Kind::password : !writable) {
            continue;
        }
        if (const auto* value = select_field(object, field, from_disk)) {
            auto normalized_value =
                read_value(*value, field, mode, std::string(path) + std::string(field.key));
            if (!from_disk || !normalized_value.is_null()) {
                normalized[field.key] = std::move(normalized_value);
            }
        }
    }
    if (!from_disk && normalized.size() != object.size()) {
        throw UserError("A user change contains an unknown or read-only field.");
    }
    return normalized;
}

void patch_fields(Json& row, const Json& patch, std::span<const Field> fields)
{
    for (const auto& field : fields) {
        const auto value = patch.find(field.key);
        if (value == patch.end()) {
            continue;
        }
        if (field.kind == Kind::object) {
            auto& nested = row[field.key];
            if (!nested.is_object()) {
                nested = Json::object();
            }
            patch_fields(nested, *value, field.children);
            if (nested.empty()) {
                row.erase(std::string(field.key));
            }
        } else {
            if (value->is_null()) {
                row.erase(std::string(field.key));
            } else {
                row[field.key] = *value;
            }
            if (!field.alias.empty()) {
                row.erase(std::string(field.alias));
            }
        }
    }
    // WaterWall reads legacy top-level time fields after the nested time object.
    if (const auto time = patch.find("time"); time != patch.end()) {
        for (const auto& field : times) {
            if (time->contains(field.key)) {
                row.erase(std::string(field.key));
                row.erase(std::string(field.alias));
            }
        }
    }
}

std::string read_database(int fd)
{
    std::string data;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw UserError("Could not read the AuthenticationServer database.");
        }
        if (count == 0) {
            return data;
        }
        if (static_cast<std::size_t>(count) > max_database_bytes - data.size()) {
            throw UserError("This AuthenticationServer database exceeds Wave's 16 "
                            "MiB editing limit.");
        }
        data.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

Json parse_database(std::string_view data)
{
    if (data.find('\0') != data.npos) {
        throw UserError("The AuthenticationServer database contains a NUL byte.");
    }
    std::vector<std::set<std::string>> keys;
    auto check = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) {
            throw UserError("The AuthenticationServer database is nested too deeply.");
        }
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == Json::parse_event_t::key || event == Json::parse_event_t::value) {
            const auto* text = value.get_ptr<const std::string*>();
            if (text && (text->find('\0') != text->npos ||
                         (event == Json::parse_event_t::key && !keys.back().insert(*text).second))) {
                throw UserError("The AuthenticationServer database contains a "
                                "duplicate key or a NUL character.");
            }
        }
        return true;
    };
    auto database = Json::parse(data, check, false);
    if (database.is_discarded()) {
        throw UserError("The AuthenticationServer database contains invalid JSON.");
    }
    return database;
}

Json database_rows(Json& database)
{
    if (database.is_null()) {
        return Json::array();
    }
    if (database.is_array()) {
        return std::move(database);
    }
    if (!database.is_object()) {
        throw UserError("The AuthenticationServer database must contain user records.");
    }
    if (const auto users = database.find("users"); users != database.end()) {
        if (users->is_array()) {
            return std::move(*users);
        }
        if (users->is_null()) {
            return Json::array();
        }
        throw UserError("The AuthenticationServer database users field must be an array.");
    }
    if ((database.contains("password") && database["password"].is_string()) ||
        (database.contains("pass") && database["pass"].is_string())) {
        auto rows = Json::array();
        rows.push_back(std::move(database));
        return rows;
    }
    Json rows = Json::array();
    for (auto& [name, row] : database.items()) {
        if (!row.is_object()) {
            throw UserError("An AuthenticationServer user record must be an object.");
        }
        const auto explicit_name = row.find("name");
        if (explicit_name == row.end() || explicit_name->is_null() ||
            (explicit_name->is_string() && explicit_name->get_ref<const std::string&>().empty())) {
            row["name"] = name;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

struct UserList {
    Json rows = Json::array();
    std::map<std::uint64_t, std::size_t> index;
    std::size_t page = 0;
    std::size_t page_count = 0;

    Json response(bool editable)
    {
        return {{"users", std::move(rows)},
                {"editable", editable},
                {"page", page},
                {"page_count", page_count},
                {"total", index.size()}};
    }
};

UserList public_users(const Json& rows, std::uint64_t requested_page)
{
    UserList result;
    result.page_count = (rows.size() + users_per_page - 1) / users_per_page;
    if (result.page_count > 0) {
        result.page =
            static_cast<std::size_t>(std::min<std::uint64_t>(requested_page, result.page_count - 1));
    }
    const auto start = result.page * users_per_page;
    std::set<std::string> names;
    std::set<std::string_view> passwords;
    std::set<std::string> uuids;
    std::map<Address, Address> ipv4;
    std::map<Address, Address> ipv6;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        auto user = read_fields(row, user_fields, ReadMode::disk);
        const auto id = user.find("id");
        const auto* value = select_field(row, password_field, true);
        const auto* password = value ? value->get_ptr<const std::string*>() : nullptr;
        if (id == user.end() || *id == "0" || !password || password->empty()) {
            throw UserError("Every AuthenticationServer user needs a nonzero ID and a password.");
        }
        if (!result.index.emplace(*decimal(id->get_ref<const std::string&>()), i).second) {
            throw UserError("AuthenticationServer user IDs must be unique.");
        }
        const auto uuid = uuid_key(*password);
        if (!passwords.insert(*password).second || (!uuid.empty() && !uuids.insert(uuid).second)) {
            throw UserError("AuthenticationServer user passwords must be unique.");
        }
        const auto name = user.find("name");
        if (name != user.end() && *name != "" && !names.insert(name->get_ref<const std::string&>()).second) {
            throw UserError("AuthenticationServer user names must be unique.");
        }
        if (const auto cidr = user.find("wireguard-allowed-ips"); cidr != user.end() && *cidr != "") {
            const auto network = parse_network(cidr->get_ref<const std::string&>());
            insert_network(network, network.family == AF_INET ? ipv4 : ipv6);
        }
        if (!user.contains("enabled")) {
            user["enabled"] = true;
        }
        for (const auto& field : times) {
            if (const auto* value = select_field(row, field, true)) {
                auto top = read_value(*value, field, ReadMode::disk, std::string(field.key));
                if (!top.is_null()) {
                    user["time"][field.key] = std::move(top);
                }
            }
        }
        if (i >= start && i - start < users_per_page) {
            if (user.dump().size() > max_public_user_bytes) {
                user = {{"id", *id}, {"enabled", user["enabled"]}, {"oversized", true}};
            }
            result.rows.push_back(std::move(user));
        }
    }
    return result;
}

std::uint64_t random_number()
{
    std::uint64_t number;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&number), sizeof(number)) != 1) {
        throw UserError("Could not generate an AuthenticationServer identifier.");
    }
    return number;
}

std::uint64_t new_user_id(const UserList& users)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto id = random_number();
        if (id != 0 && !users.index.contains(id)) {
            return id;
        }
    }
    throw UserError("Could not generate a unique AuthenticationServer user ID.");
}

bool trusted_directory(int fd)
{
    struct stat info{};
    return fd >= 0 && fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) &&
           (info.st_uid == geteuid() || info.st_uid == 0) && !(info.st_mode & (S_IWGRP | S_IWOTH));
}

int open_directory(const fs::path& root, std::string_view config_name)
{
    const std::string config(config_name);
    if (config.empty() || core_config_name(config) != config) {
        return -1;
    }
    const File configs(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!trusted_directory(configs.get())) {
        return -1;
    }
    const int directory =
        openat(configs.get(), config.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (trusted_directory(directory)) {
        return directory;
    }
    if (directory >= 0) {
        close(directory);
    }
    return -1;
}

bool private_file(int fd)
{
    struct stat info{};
    return fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
           info.st_uid == geteuid() && ((info.st_mode & 07777) == 0600 || fchmod(fd, 0600) == 0);
}

bool write_all(int fd, std::string_view data)
{
    while (!data.empty()) {
        const auto count = write(fd, data.data(), data.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(count));
    }
    return fsync(fd) == 0;
}

// A failed directory sync can only be reported as a warning after publishing
// the new primary.
enum class Publication { existing, saved, sync_failed };
Publication publish(int directory, const std::string& name, std::string_view data, bool replace)
{
    const auto temporary = ".auth-tmp-" + std::to_string(random_number());
    File file(
        openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) {
        throw UserError("Could not stage the AuthenticationServer database.");
    }
    if (fchmod(file.get(), 0600) != 0 || !write_all(file.get(), data) || !file.close_file()) {
        unlinkat(directory, temporary.c_str(), 0);
        throw UserError("Could not write the AuthenticationServer database.");
    }
    if (replace) {
        if (renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) {
            unlinkat(directory, temporary.c_str(), 0);
            throw UserError("Could not replace the AuthenticationServer database.");
        }
    } else {
        const int status = linkat(directory, temporary.c_str(), directory, name.c_str(), 0);
        const int failure = errno;
        unlinkat(directory, temporary.c_str(), 0);
        if (status != 0) {
            if (failure == EEXIST) {
                return Publication::existing;
            }
            throw UserError("Could not create the AuthenticationServer database.");
        }
    }
    return fsync(directory) == 0 ? Publication::saved : Publication::sync_failed;
}

} // namespace

CoreConfigUsers::CoreConfigUsers(const fs::path& executable_directory)
    : root_(executable_directory / "configs")
{
}

bool CoreConfigUsers::valid_filename(std::string_view db_name)
{
    return !db_name.empty() && db_name.front() != '.' && db_name != "config.json" &&
           db_name.ends_with(".json") && core_config_name(db_name) == db_name;
}

fs::path CoreConfigUsers::relative_path(std::string_view config_name, std::string_view db_name)
{
    return fs::path("configs") / config_name / db_name;
}

std::string CoreConfigUsers::prepare(std::string_view config_name, std::string_view db_name,
                                     bool& created) const
{
    created = false;
    const File directory(open_directory(root_, config_name));
    if (directory.get() < 0 || !valid_filename(db_name)) {
        return "The AuthenticationServer database path is unavailable.";
    }
    const std::string name(db_name);
    try {
        const File existing(
            openat(directory.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (existing.get() < 0 && errno == ENOENT) {
            // Let WaterWall verify and restore a recovery backup if its primary is missing.
            const auto backup_name = name + ".backup";
            const File backup(
                openat(directory.get(), backup_name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            if (backup.get() >= 0 || errno != ENOENT) {
                return private_file(backup.get())
                           ? std::string{}
                           : "The AuthenticationServer recovery backup must be a private regular file.";
            }
            const auto status = publish(directory.get(), name, "{\"users\":[]}\n", false);
            if (status != Publication::existing) {
                created = true;
                return status == Publication::saved ? std::string{}
                                                    : "Could not sync the new AuthenticationServer database.";
            }
            const File concurrent(
                openat(directory.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            if (private_file(concurrent.get())) {
                return {};
            }
        } else if (private_file(existing.get())) {
            return {};
        }
    } catch (const UserError& error) {
        return error.what();
    }
    return "The AuthenticationServer database must be a private regular file "
           "owned by Wave's user.";
}

bool CoreConfigUsers::remove(std::string_view config_name, std::string_view db_name) const
{
    const File directory(open_directory(root_, config_name));
    if (directory.get() < 0 || !valid_filename(db_name)) {
        return false;
    }
    const std::string name(db_name);
    const File file(openat(directory.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0 && errno == ENOENT) {
        return true;
    }
    return private_file(file.get()) && unlinkat(directory.get(), name.c_str(), 0) == 0 &&
           fsync(directory.get()) == 0;
}

CoreConfigUsers::Result CoreConfigUsers::execute(std::string_view action, std::string_view config_name,
                                                 std::string_view db_name, const Json& input) const
{
    const bool list = action == "get_config_users";
    const bool read_password = action == "get_config_user_password";
    const bool create = action == "create_config_user";
    const bool edit = action == "edit_config_user";
    if (!list && !read_password && !create && !edit && action != "delete_config_user") {
        return {false, "Unknown AuthenticationServer user action."};
    }
    std::uint64_t page = 0;
    if (const auto value = input.find("page"); value != input.end()) {
        const auto number = value->is_number_integer() ? unsigned_value(*value, false) : std::nullopt;
        if (!number) {
            return {false, "Choose a nonnegative AuthenticationServer users page."};
        }
        page = *number;
    }
    const File directory(open_directory(root_, config_name));
    if (directory.get() < 0 || !valid_filename(db_name)) {
        return {false, "The AuthenticationServer database path is unavailable."};
    }
    const std::string name(db_name);
    const File file(openat(directory.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!private_file(file.get())) {
        return {false, "The AuthenticationServer database must be a private "
                       "regular file owned by Wave's user."};
    }
    try {
        auto database = parse_database(read_database(file.get()));
        const bool wrapper = database.contains("users");
        auto rows = database_rows(database);
        auto listing = public_users(rows, page);
        if (list) {
            return {true, {}, listing.response(true)};
        }
        Json patch;
        if (create || edit) {
            const auto user = input.find("user");
            if (user == input.end() || user->empty()) {
                return {false, "Supply fields for the AuthenticationServer user."};
            }
            patch = read_fields(*user, user_fields, create ? ReadMode::create : ReadMode::edit);
        }
        if (create) {
            if (!patch.contains("password")) {
                return {false, "Enter a password for the AuthenticationServer user."};
            }
            if (patch.contains("id")) {
                if (patch["id"] == "0") {
                    return {false, "Choose a nonzero AuthenticationServer user ID."};
                }
            } else {
                patch["id"] = std::to_string(new_user_id(listing));
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch());
            patch["time"]["created-at-ms"] = std::to_string(now.count());
            rows.push_back(std::move(patch));
        } else {
            const auto id = input.find("user_id");
            const auto number =
                id != input.end() && id->is_string() ? unsigned_value(*id, false) : std::nullopt;
            const auto row = number ? listing.index.find(*number) : listing.index.end();
            if (row == listing.index.end()) {
                return {false, "That AuthenticationServer user is unavailable."};
            }
            if (read_password) {
                // The listing has already validated the credential in every database row.
                const auto* password = select_field(rows.at(row->second), password_field, true);
                return {true, {}, {{"password", *password}}};
            }
            if (edit) {
                patch_fields(rows.at(row->second), patch, user_fields);
            } else {
                rows.erase(rows.begin() + static_cast<Json::difference_type>(row->second));
            }
        }
        listing = public_users(rows, page);
        if (!wrapper) {
            database = Json::object();
        }
        database["users"] = std::move(rows);
        const auto serialized = database.dump() + '\n';
        if (serialized.size() > max_database_bytes) {
            return {false, "This AuthenticationServer database exceeds Wave's 16 MiB "
                           "editing limit."};
        }
        const auto status = publish(directory.get(), name, serialized, true);
        return {true,
                status == Publication::sync_failed ? "Users saved. The config directory could not be synced."
                                                   : "Saved AuthenticationServer users.",
                listing.response(true)};
    } catch (const UserError& error) {
        return {false, error.what()};
    }
}

} // namespace wave
