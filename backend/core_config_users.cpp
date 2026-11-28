#include "core_config_users.h"

#include "core_config_name.h"
#include "schematic_users_ui.h"
#include "unicode_casefold.h"
#include "users_roster.h"
#include "waterwall_schematic.h"

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

class UserDatabaseAccessError : public UserError {
  public:
    using UserError::UserError;
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

const Field* canonical_field(std::span<const Field> fields, std::string_view key)
{
    const auto found = std::ranges::find(fields, key, &Field::key);
    return found == fields.end() ? nullptr : &*found;
}

const Field* editable_number_field(std::string_view path)
{
    std::span<const Field> fields = user_fields;
    while (path.starts_with('/')) {
        path.remove_prefix(1);
        const auto separator = path.find('/');
        const auto* field = canonical_field(fields, path.substr(0, separator));
        if (!field || field->access != Access::edit) {
            return nullptr;
        }
        if (separator == path.npos) {
            return field->kind == Kind::uint64 || field->kind == Kind::interval ? field : nullptr;
        }
        fields = field->children;
        path.remove_prefix(separator);
    }
    return nullptr;
}

constexpr SchematicUserNumberBounds adapter_number_bounds(const Field& field)
{
    if (field.access == Access::create) {
        return explicit_user_id_bounds;
    }
    return field.kind == Kind::interval ? user_stat_interval_bounds : SchematicUserNumberBounds{};
}

std::string validate_number_bounds(const Json& descriptor, const Field& field)
{
    const auto domain = adapter_number_bounds(field);
    const auto bounds = schematic_user_number_bounds(descriptor, domain);
    const auto& path = descriptor.at("path").get_ref<const std::string&>();
    if (bounds.minimum < domain.minimum || bounds.maximum > domain.maximum || bounds.minimum > bounds.maximum) {
        return "Invalid bounds for offline user number " + path + '.';
    }
    if (descriptor.value("gigabytes", false)) {
        const auto megabytes = whole_megabyte_bounds(bounds);
        if (megabytes.minimum > megabytes.maximum) {
            return "The bounds for " + path + " contain no whole megabyte. Use a byte field or widen the range.";
        }
    }
    return {};
}

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
            number && *number <= adapter_number_bounds(field).maximum) {
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
    std::size_t page = 0;
    std::size_t page_count = 0;
    std::size_t matched = 0;

    Json response(std::size_t total)
    {
        return {{"users", std::move(rows)},
                {"editable", true},
                {"page", page},
                {"page_count", page_count},
                {"matched", matched},
                {"total", total}};
    }
};

Json public_user(const Json& row)
{
    auto user = read_fields(row, user_fields, ReadMode::disk);
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
    return user;
}

bool oversized_user(const Json& user)
{
    return user.dump().size() > max_public_user_bytes;
}

Json saved_roster_row(Json user)
{
    if (oversized_user(user)) {
        user["oversized"] = true;
    }
    return make_users_roster_row(user);
}

struct SavedRoster {
    std::optional<Json> rows = Json::array();
    std::size_t bytes = 2;

    void append(Json user)
    {
        if (!rows) {
            return;
        }
        auto summary = saved_roster_row(std::move(user));
        const auto size = summary.dump().size() + (rows->empty() ? 0 : 1);
        if (rows->size() == max_users_roster_rows || size > max_users_roster_bytes - bytes) {
            rows.reset();
            return;
        }
        bytes += size;
        rows->push_back(std::move(summary));
    }
};

using UserIndex = std::map<std::uint64_t, std::size_t>;

UserIndex index_users(const Json& rows, SavedRoster* roster = nullptr)
{
    UserIndex index;
    std::set<std::string> names;
    std::set<std::string_view> passwords;
    std::set<std::string> uuids;
    std::map<Address, Address> ipv4;
    std::map<Address, Address> ipv6;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        auto user = public_user(row);
        const auto id = user.find("id");
        const auto* value = select_field(row, password_field, true);
        const auto* password = value ? value->get_ptr<const std::string*>() : nullptr;
        if (id == user.end() || *id == "0" || !password || password->empty()) {
            throw UserError("Every AuthenticationServer user needs a nonzero ID and a password.");
        }
        const auto account_id = *decimal(id->get_ref<const std::string&>());
        if (!index.emplace(account_id, i).second) {
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
        if (roster) {
            roster->append(std::move(user));
        }
    }
    return index;
}

UserList public_users_page(const Json& rows, const UserIndex& index, std::uint64_t requested_page,
                           std::string_view search)
{
    UserList result;
    std::vector<std::size_t> matches;
    if (!search.empty() && search.find_first_not_of("0123456789") == search.npos) {
        if (const auto id = decimal(search)) {
            if (const auto row = index.find(*id); row != index.end()) {
                matches.push_back(row->second);
            }
        }
    } else if (!search.empty()) {
        const auto name_query = unicode_casefold(search);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto user = public_user(rows[i]);
            const auto name = user.find("name");
            if (name == user.end()) {
                continue;
            }
            const auto& text = name->get_ref<const std::string&>();
            if (text.size() <= max_public_user_bytes &&
                unicode_casefold(text).find(name_query) != std::string::npos && !oversized_user(user)) {
                matches.push_back(i);
            }
        }
    }
    result.matched = search.empty() ? rows.size() : matches.size();
    result.page_count = (result.matched + users_per_page - 1) / users_per_page;
    if (result.page_count > 0) {
        result.page = static_cast<std::size_t>(std::min<std::uint64_t>(requested_page, result.page_count - 1));
    }
    const auto start = result.page * users_per_page;
    for (std::size_t i = start; i < result.matched && i - start < users_per_page; ++i) {
        auto user = public_user(rows[search.empty() ? i : matches[i]]);
        if (oversized_user(user)) {
            user = {{"id", user["id"]}, {"enabled", user["enabled"]}, {"oversized", true}};
        }
        result.rows.push_back(std::move(user));
    }
    return result;
}

std::uint64_t new_user_id(const UserIndex& users)
{
    const auto& field = waterwall_schematic().at("ui").at("users").at("fields").at("id");
    const auto bounds = schematic_user_number_bounds(field, explicit_user_id_bounds);
    const auto id = next_sequential_user_id(users, bounds, field.value("gigabytes", false));
    if (!id) {
        throw UserError(exhausted_user_ids);
    }
    return *id;
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

int open_users_directory(const fs::path& root, std::string_view config_name, std::string_view db_name)
{
    const int directory = CoreConfigUsers::valid_filename(db_name) ? open_directory(root, config_name) : -1;
    if (directory < 0) {
        throw UserDatabaseAccessError("The AuthenticationServer database path is unavailable.");
    }
    return directory;
}

Json read_saved_database(int directory, const std::string& name)
{
    const File file(openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!private_file(file.get())) {
        throw UserDatabaseAccessError(
            "The AuthenticationServer database must be a private regular file owned by Wave's user.");
    }
    return parse_database(read_database(file.get()));
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
    std::uint64_t suffix;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&suffix), sizeof(suffix)) != 1) {
        throw UserError("Could not generate a temporary AuthenticationServer database filename.");
    }
    const auto temporary = ".auth-tmp-" + std::to_string(suffix);
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

std::string CoreConfigUsers::validate_schematic_editor(const Json& users)
{
    const auto& controls = users.at("fields");
    constexpr struct {
        const char* role;
        const char* key;
        Kind kind;
        Access access = Access::edit;
    } roles[] = {
        {"id", "id", Kind::uint64, Access::create},
        {"password", "password", Kind::password},
        {"group", "gid", Kind::uint64},
        {"name", "name", Kind::text},
        {"email", "email", Kind::text},
        {"notes", "notes", Kind::text},
        {"network", "wireguard-allowed-ips", Kind::cidr},
        {"enabled", "enabled", Kind::boolean},
    };
    for (const auto& role : roles) {
        const auto control = controls.find(role.role);
        if (control == controls.end()) {
            continue;
        }
        const auto key = schematic_text(control->at("key"));
        const auto* field = canonical_field(user_fields, key);
        if (key != role.key || !field || field->kind != role.kind || field->access != role.access ||
            (role.kind == Kind::uint64 &&
             schematic_text(control->at("path")) != "/" + std::string(role.key))) {
            return std::string("Invalid offline user editor mapping for role ") + role.role + '.';
        }
        if (field->kind == Kind::uint64) {
            if (auto error = validate_number_bounds(*control, *field); !error.empty()) {
                return error;
            }
        }
    }
    const auto validate_numbers = [](const Json& numbers) -> std::string {
        for (const auto& descriptor : numbers) {
            const auto path = schematic_text(descriptor.at("path"));
            const auto* field = editable_number_field(path);
            if (!field) {
                return "Invalid editable offline user number path " + std::string(path) + '.';
            }
            if (auto error = validate_number_bounds(descriptor, *field); !error.empty()) {
                return error;
            }
        }
        return {};
    };
    if (auto error = validate_numbers(schematic_field(users, "access_fields")); !error.empty()) {
        return error;
    }
    for (const auto& group : schematic_field(users, "limit_groups")) {
        if (auto error = validate_numbers(group.at("fields")); !error.empty()) {
            return error;
        }
    }
    return {};
}

bool CoreConfigUsers::valid_filename(std::string_view db_name)
{
    return !db_name.empty() && db_name.front() != '.' && db_name != "config.json" &&
           db_name.ends_with(".json") && core_config_name(db_name) == db_name;
}

bool CoreConfigUsers::valid_backup_database(std::string_view bytes)
{
    try {
        auto database = parse_database(bytes);
        static_cast<void>(index_users(database_rows(database)));
        return true;
    } catch (const UserError&) {
        return false;
    }
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

CoreConfigUsers::Result CoreConfigUsers::load_roster(std::string_view config_name, std::string_view db_name) const
{
    try {
        const File directory(open_users_directory(root_, config_name, db_name));
        auto database = read_saved_database(directory.get(), std::string(db_name));
        const auto users = database_rows(database);
        SavedRoster roster;
        index_users(users, &roster);
        if (!roster.rows) {
            return {false, "The saved users roster exceeds Wave's 100,000-account or 32 MiB summary limit."};
        }
        return {true, {}, std::move(*roster.rows)};
    } catch (const UserDatabaseAccessError& error) {
        return {.success = false, .message = error.what(), .database_unavailable = true};
    } catch (const UserError& error) {
        return {false, error.what()};
    }
}

CoreConfigUsers::Result CoreConfigUsers::execute(std::string_view action, std::string_view config_name,
                                                 std::string_view db_name, const Json& input,
                                                 bool include_roster_change) const
{
    const bool list = action == "get_config_users";
    const bool detail = action == "get_config_user";
    const bool read_password = action == "get_config_user_password";
    const bool create = action == "create_config_user";
    const bool edit = action == "edit_config_user";
    const bool reset_traffic = action == "reset_config_user_traffic";
    if (!list && !detail && !read_password && !create && !edit && !reset_traffic &&
        action != "delete_config_user") {
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
    std::string_view search;
    if (const auto value = input.find("search"); value != input.end()) {
        const auto* text = value->get_ptr<const std::string*>();
        if (!text || text->size() > 512 || text->find('\0') != text->npos) {
            return {false, "Enter an account ID or a name of up to 512 bytes."};
        }
        search = trim(*text);
    }
    const bool compact_reply = schematic_field(input, "roster") == true;
    try {
        const File directory(open_users_directory(root_, config_name, db_name));
        const std::string name(db_name);
        auto database = read_saved_database(directory.get(), name);
        const bool wrapper = database.contains("users");
        auto rows = database_rows(database);
        const auto index = index_users(rows);
        if (list) {
            return {true, {}, public_users_page(rows, index, page, search).response(index.size())};
        }
        Json patch;
        if (create || edit) {
            const auto user = input.find("user");
            if (user == input.end() || user->empty()) {
                return {false, "Supply fields for the AuthenticationServer user."};
            }
            patch = read_fields(*user, user_fields, create ? ReadMode::create : ReadMode::edit);
        } else if (reset_traffic) {
            patch["stats"]["traffic"] = {{"up", "0"}, {"down", "0"}};
        }
        std::string changed_id;
        std::optional<std::size_t> changed_row;
        if (create) {
            if (!patch.contains("password")) {
                return {false, "Enter a password for the AuthenticationServer user."};
            }
            if (patch.contains("id")) {
                if (patch["id"] == "0") {
                    return {false, "Choose a nonzero AuthenticationServer user ID."};
                }
            } else {
                patch["id"] = std::to_string(new_user_id(index));
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch());
            patch["time"]["created-at-ms"] = std::to_string(now.count());
            changed_id = patch.at("id").get<std::string>();
            changed_row = rows.size();
            rows.push_back(std::move(patch));
        } else {
            const auto id = input.find("user_id");
            const auto number =
                id != input.end() && id->is_string() ? unsigned_value(*id, false) : std::nullopt;
            const auto row = number ? index.find(*number) : index.end();
            if (row == index.end()) {
                return {false, "That AuthenticationServer user is unavailable."};
            }
            auto& record = rows.at(row->second);
            if (read_password) {
                // Indexing validated the credential in every row.
                const auto* password = select_field(record, password_field, true);
                return {true, {}, {{"password", *password}}};
            }
            if (detail || (edit && compact_reply)) {
                auto user = public_user(record);
                if (oversized_user(user)) {
                    return {false, "This saved account is too large to edit. It can be removed by ID."};
                }
                if (detail) {
                    return {true, {}, {{"user", std::move(user)}}};
                }
            }
            changed_id = std::to_string(row->first);
            if (edit || reset_traffic) {
                patch_fields(record, patch, user_fields);
                changed_row = row->second;
            } else {
                rows.erase(rows.begin() + static_cast<Json::difference_type>(row->second));
            }
        }
        const auto updated_index = index_users(rows);
        auto reply = compact_reply
                         ? Json{{"user_id", changed_id}}
                         : public_users_page(rows, updated_index, page, search).response(updated_index.size());
        std::optional<Json> roster_change;
        if (include_roster_change) {
            roster_change = Json{{"user_id", changed_id},
                                 {"user", changed_row ? saved_roster_row(public_user(rows.at(*changed_row)))
                                                      : Json(nullptr)}};
        }
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
                std::move(reply), std::move(roster_change)};
    } catch (const UserDatabaseAccessError& error) {
        return {.success = false, .message = error.what(), .database_unavailable = true};
    } catch (const UserError& error) {
        return {false, error.what()};
    }
}

} // namespace wave
