#include "waterwall_users.h"

#include "waterwall_schematic.h"

#include <openssl/rand.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <thread>

namespace wave {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t page_size = 50;
constexpr std::size_t max_list_limit = 100;
constexpr std::size_t max_read_request_bytes = 4 * 1024;
constexpr std::size_t max_mutation_request_bytes = 32 * 1024;
constexpr std::size_t max_response_body_bytes = 256 * 1024;
constexpr std::size_t max_envelope_overhead = 8 * 1024;
constexpr std::size_t max_status_response_bytes = 4 * 1024;
constexpr std::size_t max_mutation_response_bytes = 256;
constexpr auto peer_verification_timeout = 300ms;

using Operation = WaterWallUsersOperation;
using Outcome = WaterWallUsersOutcome;

constexpr bool is_mutation(Operation operation)
{
    return operation == Operation::create || operation == Operation::update || operation == Operation::password;
}

const Json& field(const Json& object, const char* name)
{
    static const Json missing;
    const auto it = object.find(name);
    return it == object.end() ? missing : *it;
}

std::string_view text(const Json& value)
{
    const auto* string = value.get_ptr<const std::string*>();
    return string ? std::string_view(*string) : std::string_view{};
}

const Json& live_users_spec()
{
    const auto& schematic = waterwall_schematic();
    if (schematic_field(schematic_field(schematic, "runtime"), "users_protocol") == "auth-json-and-api-v1") {
        return schematic_field(schematic_field(schematic, "config"), "live_users");
    }
    static const Json unsupported;
    return unsupported;
}

template <typename Match> bool for_each_api(const Json& config, Match match)
{
    const auto& nodes = field(config, "nodes");
    const auto& spec = live_users_spec();
    if (!spec.is_object() || !nodes.is_array()) {
        return false;
    }
    std::map<std::string_view, const Json*> apis;
    for (const auto& node : nodes) {
        if (field(node, "type") == spec.at("api_node")) {
            const auto name = text(field(node, "name"));
            if (!name.empty()) {
                apis.emplace(name, &node);
            }
        }
    }
    for (const auto& node : nodes) {
        if (field(node, "type") == spec.at("listener_node")) {
            const auto api = apis.find(text(field(node, "next")));
            if (api != apis.end() && match(field(node, "settings"), field(*api->second, "settings"))) {
                return true;
            }
        }
    }
    return false;
}

std::string local_address(const Json& settings)
{
    const std::string address(text(field(settings, "address")));
    in_addr ipv4{};
    if (address.find('\0') != address.npos) {
        return {};
    }
    if (::inet_pton(AF_INET, address.c_str(), &ipv4) == 1) {
        if (ipv4.s_addr == INADDR_ANY) {
            return "127.0.0.1";
        }
        if (ntohl(ipv4.s_addr) >> 24 == 127) {
            return address;
        }
    }
    in6_addr ipv6{};
    if (::inet_pton(AF_INET6, address.c_str(), &ipv6) == 1 &&
        (IN6_IS_ADDR_LOOPBACK(&ipv6) || IN6_IS_ADDR_UNSPECIFIED(&ipv6))) {
        return "::1";
    }
    return {};
}

std::uint16_t port_number(const Json& value)
{
    return value.is_number_integer() && value >= 1 && value <= 65535 ? value.get<std::uint16_t>() : 0;
}

template <typename Visit> void visit_port_ranges(const Json& settings, Visit visit)
{
    const auto& ports = field(settings, "port");
    if (ports.is_array()) {
        for (const auto& entry : ports) {
            if (const auto port = port_number(entry)) {
                visit(port, port);
            }
        }
    } else if (const auto port = port_number(ports)) {
        visit(port, port);
    }
    const auto& range = field(settings, "port-range");
    if (range.is_array() && range.size() == 2) {
        const auto first = port_number(range.front());
        const auto last = port_number(range.back());
        if (first && first <= last) {
            visit(first, last);
        }
    }
}

bool can_receive_at(const Json& settings, const std::string& destination)
{
    const std::string address(text(field(settings, "address")));
    in_addr v4{};
    in6_addr v6{};
    if (::inet_pton(AF_INET, address.c_str(), &v4) == 1) {
        return v4.s_addr == INADDR_ANY ? destination.find(':') == std::string::npos : address == destination;
    }
    if (::inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&v6)) {
            return ::inet_pton(AF_INET, destination.c_str(), &v4) == 1 &&
                   std::memcmp(&v4, v6.s6_addr + 12, sizeof(v4)) == 0;
        }
        return IN6_IS_ADDR_UNSPECIFIED(&v6) || (IN6_IS_ADDR_LOOPBACK(&v6) && destination == "::1");
    }
    // A nonnumeric bind address might also resolve to the API's loopback endpoint.
    return true;
}

// Include even redirected ranges when checking for overlap with other configured listeners.
void mark_bound_tcp_ports(const Json& config, const std::string& address, const Json* except_listener,
                          std::vector<bool>& occupied)
{
    const auto& listener_types = schematic_field(live_users_spec(), "tcp_listener_nodes");
    for (const auto& node : field(config, "nodes")) {
        const auto& settings = field(node, "settings");
        if (&settings != except_listener &&
            std::ranges::find(listener_types, field(node, "type")) != listener_types.end() &&
            can_receive_at(settings, address)) {
            visit_port_ranges(settings, [&](unsigned first, unsigned last) {
                std::fill(occupied.begin() + first, occupied.begin() + last + 1, true);
            });
        }
    }
}

bool allows_user_operation(const Json& client, std::string_view config, std::string_view node,
                           Operation operation)
{
    const auto& allow = field(client, "allow");
    return allow.is_array() && std::ranges::any_of(allow, [&](const Json& entry) {
               const auto& operations = field(entry, "operations");
               return field(entry, "config") == config && field(entry, "node") == node &&
                      operations.is_array() &&
                      std::ranges::find(operations, waterwall_users_operation_name(operation)) != operations.end();
           });
}

bool wait_for(int fd, short events, Clock::time_point deadline)
{
    pollfd descriptor{fd, events, 0};
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        const auto timeout = static_cast<int>(std::max(0ms, remaining).count());
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready >= 0 || errno != EINTR) {
            return ready > 0 && (descriptor.revents & events) != 0;
        }
    }
    return false;
}

template <typename Transfer>
bool transfer_all(int fd, std::span<char> bytes, short events, Clock::time_point deadline, Transfer transfer)
{
    while (!bytes.empty() && wait_for(fd, events, deadline)) {
        const auto count = transfer(fd, bytes.data(), bytes.size());
        if (count > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return false;
        }
    }
    return bytes.empty();
}

std::string proc_tcp_address(const sockaddr* address)
{
    std::ostringstream key;
    key << std::hex << std::uppercase << std::setfill('0');
    std::uint16_t port;
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        key << std::setw(8) << v4->sin_addr.s_addr;
        port = v4->sin_port;
    } else {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        std::uint32_t words[4];
        std::memcpy(words, &v6->sin6_addr, sizeof(words));
        for (const auto word : words) {
            key << std::setw(8) << word;
        }
        port = v6->sin6_port;
    }
    key << ':' << std::setw(4) << ntohs(port);
    return key.str();
}

bool owns_socket(const std::filesystem::path& process, const std::string& inode, Clock::time_point deadline)
{
    const auto socket = "socket:[" + inode + ']';
    std::error_code ec;
    for (std::filesystem::directory_iterator fd(process / "fd", ec), end;
         !ec && fd != end && Clock::now() < deadline;
         fd.increment(ec)) {
        std::error_code link_error;
        if (std::filesystem::read_symlink(fd->path(), link_error) == socket) {
            return true;
        }
    }
    return false;
}

bool wait_for_core_peer(int fd, const sockaddr* peer, pid_t pid, Clock::time_point deadline)
{
    sockaddr_storage local{};
    auto* local_address = reinterpret_cast<sockaddr*>(&local);
    socklen_t size = sizeof(local);
    if (::getsockname(fd, local_address, &size) != 0 || local_address->sa_family != peer->sa_family) {
        return false;
    }
    const auto local_key = proc_tcp_address(local_address);
    const auto peer_key = proc_tcp_address(peer);
    const std::filesystem::path process = "/proc/" + std::to_string(pid);
    const auto tcp = process / "net" / (peer->sa_family == AF_INET ? "tcp" : "tcp6");

    // Check the reverse endpoint tuple and accepted socket inode before giving the peer an operator token.
    while (Clock::now() < deadline) {
        std::ifstream rows(tcp);
        if (!rows) {
            return false;
        }
        std::string line;
        while (std::getline(rows, line) && Clock::now() < deadline) {
            std::istringstream fields(line);
            std::string slot, from, to, state, queues, timer, retransmits, uid, timeout, inode;
            if (fields >> slot >> from >> to >> state && from == peer_key && to == local_key &&
                state == "01" && fields >> queues >> timer >> retransmits >> uid >> timeout >> inode &&
                inode != "0") {
                if (owns_socket(process, inode, deadline)) {
                    return true;
                }
                break;
            }
        }
        std::this_thread::sleep_until(std::min(deadline, Clock::now() + 20ms));
    }
    return false;
}

struct Exchange {
    std::string body;
    std::string_view error;
    bool request_sent = false;
};

Exchange exchange(pid_t core_pid, const std::string& host, std::uint16_t port, std::string& frame,
                  std::size_t max_envelope_bytes, Clock::time_point send_deadline, Clock::time_point reply_deadline)
{
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    v4.sin_family = AF_INET;
    v6.sin6_family = AF_INET6;
    v4.sin_port = v6.sin6_port = htons(port);
    const bool is_v4 = ::inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1;
    if (!is_v4 && ::inet_pton(AF_INET6, host.c_str(), &v6.sin6_addr) != 1) {
        return {{}, "connection_failed"};
    }
    const auto* address =
        is_v4 ? reinterpret_cast<const sockaddr*>(&v4) : reinterpret_cast<const sockaddr*>(&v6);
    struct Socket {
        int fd;
        ~Socket()
        {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } socket{::socket(address->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (socket.fd < 0) {
        return {{}, "connection_failed"};
    }
    if (::connect(socket.fd, address, is_v4 ? sizeof(v4) : sizeof(v6)) < 0) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (errno != EINPROGRESS || !wait_for(socket.fd, POLLOUT, send_deadline) ||
            ::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            return {{}, "connection_failed"};
        }
    }
    const auto verify_deadline = std::min(send_deadline, Clock::now() + peer_verification_timeout);
    if (core_pid <= 0 || !wait_for_core_peer(socket.fd, address, core_pid, verify_deadline)) {
        return {{}, "unverified_peer"};
    }
    const auto send = [](int fd, const char* data, std::size_t size) {
        return ::send(fd, data, size, MSG_NOSIGNAL);
    };
    const auto receive = [](int fd, char* data, std::size_t size) { return ::recv(fd, data, size, 0); };
    std::uint32_t size = 0;
    if (!transfer_all(socket.fd, frame, POLLOUT, send_deadline, send)) {
        // The gateway cannot parse an incomplete frame.
        return {{}, "connection_failed"};
    }
    if (!transfer_all(socket.fd, {reinterpret_cast<char*>(&size), sizeof(size)}, POLLIN, reply_deadline, receive)) {
        return {{}, "connection_failed", true};
    }
    size = ntohl(size);
    if (size == 0 || size > max_envelope_bytes) {
        return {{}, size == 0 ? "invalid_response" : "response_too_large", true};
    }
    std::string body(size, '\0');
    if (!transfer_all(socket.fd, body, POLLIN, reply_deadline, receive)) {
        return {{}, "connection_failed", true};
    }
    return {std::move(body), {}, true};
}

Json parse(std::string_view body)
{
    constexpr int max_depth = 16;
    std::array<std::set<std::string>, max_depth + 1> keys;
    bool invalid = false;
    const auto check = [&](int depth, Json::parse_event_t event, const Json& value) {
        if (depth > max_depth || invalid) {
            invalid = true;
            return false;
        }
        if (event == Json::parse_event_t::object_start) {
            keys[depth].clear();
        } else if (event == Json::parse_event_t::key && !keys[depth - 1].emplace(text(value)).second) {
            invalid = true;
        }
        if (value.is_string() && text(value).find('\0') != std::string_view::npos) {
            invalid = true;
        }
        return !invalid;
    };
    auto parsed = Json::parse(body, check, false);
    return invalid || !parsed.is_object() ? Json{} : std::move(parsed);
}

std::optional<std::uint64_t> unsigned_number(std::string_view number)
{
    if (number.empty() || (number.size() > 1 && number.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto end = number.data() + number.size();
    const auto parsed = std::from_chars(number.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional{result} : std::nullopt;
}

std::optional<std::uint64_t> unsigned_number(const Json& value)
{
    return unsigned_number(text(value));
}

Json legacy_page(const Json& body)
{
    const auto& rows = field(body, "users");
    const auto total = unsigned_number(field(body, "total_users"));
    const auto& cursor = field(body, "next_cursor");
    if (!rows.is_array() || rows.size() > page_size || !total || *total < rows.size() ||
        (!cursor.is_null() && !valid_waterwall_users_cursor(text(cursor)))) {
        return nullptr;
    }
    auto users = Json::array();
    for (const auto& row : rows) {
        const auto& id = field(row, "id");
        const auto& enabled = field(row, "enabled");
        const auto& usage = field(row, "usage");
        const auto& upload = field(usage, "u");
        const auto& download = field(usage, "d");
        const auto name = text(field(row, "name_preview"));
        if (!unsigned_number(id) || !enabled.is_boolean() || !unsigned_number(upload) ||
            !unsigned_number(download) || name.size() > 256) {
            return nullptr;
        }
        Json user{
            {"id", id}, {"name", name}, {"enabled", enabled}, {"usage", {{"u", upload}, {"d", download}}}};
        if (const auto& group = field(row, "gid"); unsigned_number(group)) {
            user["group"] = group;
        }
        users.push_back(std::move(user));
    }
    return {{"available", true}, {"users", std::move(users)}, {"total", *total}, {"next_cursor", cursor}};
}

enum class FieldKind { object, number, interval, boolean, string };

struct AccountField {
    const char* wave;
    const char* api;
    FieldKind kind;
    std::span<const AccountField> children = {};
    bool read_only = false;
};

constexpr AccountField directions[] = {
    {"up", "u", FieldKind::number}, {"down", "d", FieldKind::number}};
constexpr AccountField traffic[] = {directions[0], directions[1], {"total", "total", FieldKind::number}};
constexpr AccountField limits[] = {
    {"traffic", "traffic", FieldKind::object, traffic},
    {"bandwidth", "bandwidth", FieldKind::object, directions},
    {"ips", "ips", FieldKind::number},
    {"devices", "devices", FieldKind::number},
    {"connections-in", "cons_in", FieldKind::number},
    {"connections-out", "cons_out", FieldKind::number}};
constexpr AccountField summary_times[] = {
    {"expire-at-ms", "expire_at_ms", FieldKind::number},
    {"expire-after-first-usage-ms", "expire_after_first_usage_ms", FieldKind::number},
    {"first-usage-at-ms", "first_usage_at_ms", FieldKind::number, {}, true}};
constexpr AccountField detail_times[] = {
    summary_times[0], summary_times[1], summary_times[2],
    {"created-at-ms", "created_at_ms", FieldKind::number, {}, true}};
constexpr AccountField account_fields[] = {
    {"gid", "gid", FieldKind::number},
    {"enabled", "enabled", FieldKind::boolean},
    {"name", "name", FieldKind::string},
    {"email", "email", FieldKind::string},
    {"notes", "notes", FieldKind::string},
    {"wireguard-allowed-ips", "wireguard_allowed_ips", FieldKind::string},
    {"record-stat-interval-ms", "record_stat_interval_ms", FieldKind::interval},
    {"limit", "limit", FieldKind::object, limits},
    {"time", "time", FieldKind::object, detail_times}};

bool valid_account_leaf(const Json& value, FieldKind kind)
{
    switch (kind) {
    case FieldKind::object: return value.is_object();
    case FieldKind::number: return unsigned_number(value).has_value();
    case FieldKind::interval: {
        const auto interval = unsigned_number(value);
        return interval && *interval <= std::numeric_limits<int>::max();
    }
    case FieldKind::boolean: return value.is_boolean();
    case FieldKind::string:
        return value.is_string() && text(value).find('\0') == std::string_view::npos;
    }
    return false;
}

Json account_default(FieldKind kind)
{
    switch (kind) {
    case FieldKind::object: return nullptr;
    case FieldKind::number: return "0";
    case FieldKind::interval: return "120000";
    case FieldKind::boolean: return true;
    case FieldKind::string: return "";
    }
    return nullptr;
}

Json project_account_fields(const Json& user, std::span<const AccountField> fields)
{
    if (!user.is_object()) {
        return nullptr;
    }
    auto normalized = Json::object();
    for (const auto& descriptor : fields) {
        const auto& value = field(user, descriptor.api);
        if (!valid_account_leaf(value, descriptor.kind)) {
            return nullptr;
        }
        auto projected = descriptor.kind == FieldKind::object ? project_account_fields(value, descriptor.children)
                                                             : value;
        if (projected.is_null()) {
            return nullptr;
        }
        normalized[descriptor.wave] = std::move(projected);
    }
    return normalized;
}

Json api_account_fields(const Json& patch, std::span<const AccountField> fields)
{
    if (!patch.is_object() || patch.empty()) {
        return nullptr;
    }
    auto encoded = Json::object();
    for (const auto& [key, value] : patch.items()) {
        const auto descriptor = std::ranges::find_if(fields, [&](const AccountField& entry) {
            return entry.wave == key;
        });
        if (descriptor == fields.end() || descriptor->read_only) {
            return nullptr;
        }
        auto mapped = value.is_null() ? account_default(descriptor->kind) : value;
        if (!valid_account_leaf(mapped, descriptor->kind)) {
            return nullptr;
        }
        if (descriptor->kind == FieldKind::object) {
            mapped = api_account_fields(mapped, descriptor->children);
        }
        if (mapped.is_null()) {
            return nullptr;
        }
        encoded[descriptor->api] = std::move(mapped);
    }
    return encoded;
}

Json request_body(const WaterWallUsersRequest& request)
{
    if (request.operation == Operation::status) {
        return Json::object();
    }
    if (request.operation == Operation::list) {
        if (request.list_limit == 0 || request.list_limit > max_list_limit) {
            return nullptr;
        }
        Json body{{"limit", request.list_limit}};
        if (!request.cursor.empty()) {
            if (!valid_waterwall_users_cursor(request.cursor)) {
                return nullptr;
            }
            body["cursor"] = request.cursor;
        }
        return body;
    }
    if (!valid_waterwall_user_id(request.id)) {
        return nullptr;
    }
    Json body{{"id", request.id}};
    if (request.operation == Operation::get) {
        return body;
    }
    if (request.operation == Operation::update || request.operation == Operation::password) {
        if (!valid_waterwall_user_edit_token(request.if_match)) {
            return nullptr;
        }
        body["if_match"] = request.if_match;
    }
    if (request.operation == Operation::create || request.operation == Operation::password) {
        if (request.password.empty() || request.password.find('\0') != std::string::npos) {
            return nullptr;
        }
        body["password"] = request.password;
    }
    const bool has_creation_fields = !request.fields.is_null() &&
                                     !(request.fields.is_object() && request.fields.empty());
    if (request.operation == Operation::update ||
        (request.operation == Operation::create && has_creation_fields)) {
        auto patch = api_account_fields(request.fields, account_fields);
        if (patch.is_null()) {
            return nullptr;
        }
        body[request.operation == Operation::update ? "patch" : "config"] = std::move(patch);
    }
    return body;
}

bool valid_revision(const Json& body)
{
    return unsigned_number(field(body, "config_revision")).value_or(0) != 0 &&
           valid_waterwall_user_edit_token(text(field(body, "edit_token")));
}

bool valid_account_view_revision(const Json& body)
{
    return valid_revision(body) && unsigned_number(field(body, "stats_revision")).value_or(0) != 0 &&
           unsigned_number(field(body, "accounting_time_ms")).has_value();
}

Json public_detail(const Json& body, std::string_view id)
{
    const auto& user = field(body, "user");
    if (!valid_account_view_revision(body) || field(user, "id") != id) {
        return nullptr;
    }
    auto account = project_account_fields(user, account_fields);
    auto usage = project_account_fields(field(user, "usage"), directions);
    if (account.is_null() || usage.is_null()) {
        return nullptr;
    }
    account["id"] = id;
    account["stats"]["traffic"] = std::move(usage);
    Json result{{"user", std::move(account)},
                {"if_match", body.at("edit_token")},
                {"config_revision", body.at("config_revision")},
                {"stats_revision", body.at("stats_revision")},
                {"accounting_time_ms", body.at("accounting_time_ms")}};
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (wall >= 0) {
        result["host_time_ms"] = std::to_string(wall);
    }
    return result;
}

bool valid_name_preview(const Json& value, std::string_view state)
{
    if (state == "invalid_encoding") {
        return value.is_null();
    }
    return (state == "complete" || state == "truncated") && value.is_string() && text(value).size() <= 256;
}

Json public_page(const Json& body, std::size_t list_limit)
{
    const auto& rows = field(body, "users");
    const auto& total_users = field(body, "total_users");
    const auto total = unsigned_number(total_users);
    const auto& cursor = field(body, "next_cursor");
    if (!valid_account_view_revision(body) || !rows.is_array() || rows.size() > list_limit || !total ||
        *total < rows.size() || !body.contains("next_cursor") ||
        (!cursor.is_null() && (rows.empty() || !valid_waterwall_users_cursor(text(cursor))))) {
        return nullptr;
    }
    std::set<std::string_view> ids;
    auto users = Json::array();
    for (const auto& row : rows) {
        const auto& id = field(row, "id");
        const auto& group = field(row, "gid");
        const auto& enabled = field(row, "enabled");
        const auto& name = field(row, "name_preview");
        const auto& name_state = field(row, "name_preview_state");
        if (!valid_waterwall_user_id(text(id)) || !ids.insert(text(id)).second || !unsigned_number(group) ||
            !enabled.is_boolean() || !row.contains("name_preview") || !valid_name_preview(name, text(name_state))) {
            return nullptr;
        }
        auto usage = project_account_fields(field(row, "usage"), directions);
        auto limits = project_account_fields(field(row, "traffic_limit"), traffic);
        auto times = project_account_fields(field(row, "time"), summary_times);
        if (usage.is_null() || limits.is_null() || times.is_null()) {
            return nullptr;
        }
        users.push_back({{"id", id},
                         {"gid", group},
                         {"enabled", enabled},
                         {"name", name},
                         {"name_preview_state", name_state},
                         {"stats", {{"traffic", std::move(usage)}}},
                         {"limit", {{"traffic", std::move(limits)}}},
                         {"time", std::move(times)}});
    }
    return {{"available", true},
            {"users", std::move(users)},
            {"total", total_users},
            {"next_cursor", cursor},
            {"config_revision", body.at("config_revision")},
            {"stats_revision", body.at("stats_revision")},
            {"accounting_time_ms", body.at("accounting_time_ms")}};
}

template <std::size_t Size>
bool one_of(std::string_view value, const std::string_view (&allowed)[Size])
{
    return std::ranges::find(allowed, value) != std::end(allowed);
}

bool select_fields(Json& output, const Json& input, std::initializer_list<const char*> names, FieldKind kind)
{
    for (const auto* name : names) {
        const auto& value = field(input, name);
        if (!valid_account_leaf(value, kind)) {
            return false;
        }
        output[name] = value;
    }
    return true;
}

Json public_status(const Json& body)
{
    auto status = Json::object();
    if (!valid_revision(body) || unsigned_number(field(body, "stats_revision")).value_or(0) == 0 ||
        !select_fields(status, body, {"database_loaded", "database_dirty", "primary_checkpoint_valid"},
                       FieldKind::boolean) ||
        !select_fields(status, body, {"user_count", "configured_producer_count", "session_record_count",
                                     "config_revision", "stats_revision"}, FieldKind::number)) {
        return nullptr;
    }

    const auto& save = field(body, "primary_save");
    const auto& last_result = field(save, "last_result");
    if (!one_of(text(last_result), {"not_attempted", "success", "failure"})) {
        return nullptr;
    }
    auto primary_save = Json{{"last_result", last_result}};
    for (const char* name : {"last_attempt_age_ms", "last_success_age_ms"}) {
        const auto& age = field(save, name);
        if (!save.contains(name) || (!age.is_null() && !unsigned_number(age))) {
            return nullptr;
        }
        primary_save[name] = age;
    }
    status["primary_save"] = std::move(primary_save);

    const auto& audit = field(body, "audit");
    const auto& last_error = field(audit, "last_error");
    auto public_audit = Json::object();
    if (!select_fields(public_audit, audit, {"enabled", "healthy"}, FieldKind::boolean) ||
        !select_fields(public_audit, audit, {"queued_records", "written_records", "dropped_records",
                                            "suppressed_rejections", "write_failures"}, FieldKind::number) ||
        !audit.contains("last_error") ||
        (!last_error.is_null() && !one_of(text(last_error), {"queue_full", "sequence_exhausted", "open",
                                                            "write", "flush", "rotate", "close", "invalid-file"}))) {
        return nullptr;
    }
    public_audit["last_error"] = last_error;
    status["audit"] = std::move(public_audit);
    return {{"status", std::move(status)}};
}

Json project_response(const Json& body, const WaterWallUsersRequest& request, bool legacy_list)
{
    switch (request.operation) {
    case Operation::status: return public_status(body);
    case Operation::list: return legacy_list ? legacy_page(body) : public_page(body, request.list_limit);
    case Operation::get: return public_detail(body, request.id);
    case Operation::create:
    case Operation::update:
    case Operation::password:
        if (valid_revision(body) && field(body, "id") == request.id) {
            return {{"id", body.at("id")}, {"config_revision", body.at("config_revision")}};
        }
        return nullptr;
    }
    return nullptr;
}

bool valid_node_error(std::string_view code, Operation operation)
{
    if (one_of(code, {"invalid_request", "busy", "timeout", "response_too_large"})) {
        return true;
    }
    switch (operation) {
    case Operation::status: return false;
    case Operation::list: return one_of(code, {"invalid_cursor", "stale_cursor"});
    case Operation::get: return one_of(code, {"not_found", "record_too_large", "invalid_record_encoding"});
    case Operation::create:
        return one_of(code, {"id_exists", "name_exists", "credentials_exist", "invalid_allowed_ips",
                             "allowed_ips_overlap", "resource_limit", "user_too_large", "creation_failed",
                             "revision_exhausted"});
    case Operation::update:
        return one_of(code, {"conflict", "not_found", "invalid_allowed_ips", "name_exists",
                             "allowed_ips_overlap", "resource_limit", "user_too_large", "revision_exhausted"});
    case Operation::password:
        return one_of(code, {"conflict", "not_found", "credentials_exist", "resource_limit",
                             "user_too_large", "password_change_failed", "revision_exhausted"});
    }
    return false;
}

std::string_view error_message(std::string_view code)
{
    if (code == "connection_failed") {
        return "Waiting for the WaterWall API to respond.";
    }
    if (code == "unverified_peer") {
        return "Wave could not verify the WaterWall API listener.";
    }
    if (code == "invalid_response") {
        return "The WaterWall API returned an invalid reply.";
    }
    if (code == "response_too_large") {
        return "The WaterWall API reply exceeded its size limit.";
    }
    if (code == "not_found") {
        return "WaterWall could not find that user.";
    }
    if (code == "record_too_large") {
        return "This account exceeds the WaterWall API details limit.";
    }
    if (code == "invalid_record_encoding") {
        return "This account has text that WaterWall could not read.";
    }
    if (code == "conflict") {
        return "The WaterWall user configuration changed. Review current account details.";
    }
    if (code == "busy") {
        return "WaterWall is busy. Try again.";
    }
    if (code == "timeout") {
        return "The WaterWall API operation timed out.";
    }
    if (one_of(code, {"forbidden", "unauthorized"})) {
        return "The WaterWall API denied this operation. Check the API client permission.";
    }
    if (code == "id_exists") {
        return "An account with that ID already exists.";
    }
    if (code == "name_exists") {
        return "An account with that name already exists.";
    }
    if (code == "credentials_exist") {
        return "Those credentials already belong to an account.";
    }
    if (code == "invalid_allowed_ips") {
        return "Enter a valid WireGuard allowed IP range.";
    }
    if (code == "allowed_ips_overlap") {
        return "That WireGuard allowed IP range overlaps another account.";
    }
    return "WaterWall declined the user operation.";
}

WaterWallUsersReply failure(Outcome outcome, std::string_view code, std::string_view message = {})
{
    if (message.empty()) {
        message = error_message(code);
    }
    if (outcome == Outcome::unknown) {
        return {{{"code", code}},
                std::string(message) + " The write outcome is unknown.", outcome};
    }
    return {{{"code", code}}, std::string(message), outcome};
}

std::string make_frame(const WaterWallUsersEndpoint& endpoint, std::string_view node,
                       const WaterWallUsersRequest& request, std::string_view correlation)
{
    const auto body = request_body(request);
    if (body.is_null()) {
        return {};
    }
    auto encoded_body = body.dump();
    const auto body_limit = is_mutation(request.operation) ? max_mutation_request_bytes : max_read_request_bytes;
    if (encoded_body.size() > body_limit) {
        return {};
    }
    const Json envelope{{"id", correlation},
                        {"auth", {{"client", endpoint.client}, {"token", endpoint.token}}},
                        {"selector", {{"config", endpoint.config}, {"node", node}}},
                        {"operation", waterwall_users_operation_name(request.operation)},
                        {"content_type", "application/json"},
                        {"body_encoding", "utf8"},
                        {"body", std::move(encoded_body)}};
    const auto payload = envelope.dump();
    // The inner JSON is UTF-8 and already escapes control characters. Its quotes and
    // backslashes can double in the envelope, apart from the bounded selector and auth fields.
    if (payload.size() > 2 * body_limit + max_envelope_overhead) {
        return {};
    }
    const auto size = htonl(static_cast<std::uint32_t>(payload.size()));
    std::string frame(reinterpret_cast<const char*>(&size), sizeof(size));
    frame += payload;
    return frame;
}

struct EndpointReply {
    WaterWallUsersReply reply;
    bool try_other_endpoint = false;
};

EndpointReply request_users(pid_t core_pid, const WaterWallUsersEndpoint& endpoint, std::uint16_t port,
                            std::string_view node, const WaterWallUsersRequest& request, bool legacy_list,
                            Clock::time_point send_deadline, Clock::time_point reply_deadline)
{
    // Each connection admits exactly one request.
    constexpr std::string_view correlation = "wave-users";
    std::string frame;
    try {
        frame = make_frame(endpoint, node, request, correlation);
    } catch (const Json::type_error&) {
        return {failure(Outcome::not_sent, "invalid_request", "Enter valid WaterWall user fields.")};
    }
    if (frame.empty()) {
        return {failure(Outcome::not_sent, "invalid_request",
                        "The WaterWall user request has invalid fields or exceeds its size limit.")};
    }

    auto reply_body_limit = max_response_body_bytes;
    if (is_mutation(request.operation)) {
        reply_body_limit = max_mutation_response_bytes;
    } else if (request.operation == Operation::status) {
        reply_body_limit = max_status_response_bytes;
    }
    const auto envelope_limit = legacy_list ? max_response_body_bytes : 2 * reply_body_limit + max_envelope_overhead;
    auto reply = exchange(core_pid, endpoint.address, port, frame, envelope_limit, send_deadline, reply_deadline);
    const bool possibly_delivered_write = reply.request_sent && is_mutation(request.operation);
    const auto unconfirmed = [&](std::string_view code) -> EndpointReply {
        return {failure(possibly_delivered_write ? Outcome::unknown : Outcome::not_sent, code),
                !possibly_delivered_write};
    };
    if (!reply.error.empty()) {
        return unconfirmed(reply.error);
    }

    const auto envelope = parse(reply.body);
    if (!envelope.is_object() || envelope.size() != 3 || !envelope.contains("id") ||
        !envelope.contains("error") || !envelope.contains("result")) {
        return unconfirmed("invalid_response");
    }
    const auto& response_id = field(envelope, "id");
    const auto& gateway_error = field(envelope, "error");
    const auto& result = field(envelope, "result");
    if (!gateway_error.is_null()) {
        const auto code = text(field(gateway_error, "code"));
        const auto& message = field(gateway_error, "message");
        if (!gateway_error.is_object() || gateway_error.size() != 2 || !result.is_null() ||
            code.empty() || text(message).empty() || text(message).size() > 512) {
            return unconfirmed("invalid_response");
        }
        // The verified gateway sends this framing rejection before it has parsed a request ID.
        if (response_id.is_null() && code == "invalid_request") {
            return {failure(Outcome::not_sent, code,
                            "The WaterWall API rejected the request frame. Reduce the request size or check "
                            "the listener's request limit.")};
        }
        if (response_id != correlation) {
            return unconfirmed("invalid_response");
        }
        if (one_of(code, {"invalid_request", "invalid_encoding", "unauthorized", "forbidden", "target_not_found",
                          "ambiguous_target", "unsupported_content_type", "busy", "shutting_down",
                          "unsupported_api", "unsupported_operation"})) {
            return {failure(Outcome::not_sent, code)};
        }
        if (one_of(code, {"timeout", "response_too_large", "invalid_response"})) {
            return unconfirmed(code);
        }
        return unconfirmed("invalid_response");
    }

    const auto& result_code = field(result, "code");
    const auto result_body = text(field(result, "body"));
    if (response_id != correlation || !result.is_object() || result.size() != 4 ||
        field(result, "body_encoding") != "utf8" || field(result, "content_type") != "application/json" ||
        !result_code.is_number_integer() || (result_code != 0 && result_code != 1) ||
        result_body.size() > reply_body_limit) {
        return unconfirmed("invalid_response");
    }
    const auto body = parse(result_body);
    if (body.is_null()) {
        return unconfirmed("invalid_response");
    }
    if (result_code == 1) {
        const auto& error = field(body, "error");
        const auto code = text(field(error, "code"));
        if (body.size() != 1 || !error.is_object() || error.size() != 1 ||
            !valid_node_error(code, request.operation)) {
            return unconfirmed("invalid_response");
        }
        if (!request.cursor.empty() && one_of(code, {"stale_cursor", "invalid_cursor"})) {
            auto response = failure(Outcome::rejected, code,
                                    "The WaterWall user list changed. Reloading from the first page.");
            response.result["reset_cursor"] = true;
            return {std::move(response)};
        }
        return {failure(code == "conflict" ? Outcome::conflict : Outcome::rejected, code)};
    }
    auto result_data = project_response(body, request, legacy_list);
    if (result_data.is_null()) {
        return unconfirmed("invalid_response");
    }
    std::optional<WaterWallUsersRevisions> revisions;
    if (!legacy_list && (request.operation == Operation::list || request.operation == Operation::status)) {
        const bool listing = request.operation == Operation::list;
        revisions = WaterWallUsersRevisions{
            *unsigned_number(body.at("config_revision")), *unsigned_number(body.at("stats_revision")),
            *unsigned_number(body.at(listing ? "total_users" : "user_count")),
            body.at("edit_token").get<std::string>(),
            listing ? unsigned_number(field(body, "accounting_time_ms")) : std::nullopt};
    }
    return {{std::move(result_data), {}, Outcome::applied, std::move(revisions)}};
}

} // namespace

bool has_waterwall_users_api(const Json& config)
{
    return for_each_api(config, [](const Json&, const Json&) { return true; });
}

std::optional<std::uint16_t> available_waterwall_api_port(std::span<const Json> configs)
{
    constexpr unsigned first_port = 49152;
    constexpr unsigned port_count = 65536 - first_port;
    std::uint16_t offset;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&offset), sizeof(offset)) != 1) {
        return std::nullopt;
    }
    const int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (socket < 0) {
        return std::nullopt;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::vector<bool> occupied(65536);
    for (const auto& config : configs) {
        mark_bound_tcp_ports(config, "127.0.0.1", nullptr, occupied);
    }
    std::optional<std::uint16_t> selected;
    // Check the whole dynamic port range from a random starting point, including saved listeners.
    for (unsigned step = 0; step < port_count; ++step) {
        const auto port = static_cast<std::uint16_t>(first_port + (offset + step) % port_count);
        if (occupied[port]) {
            continue;
        }
        address.sin_port = htons(port);
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            selected = port;
            break;
        }
        if (errno != EADDRINUSE) {
            break;
        }
    }
    ::close(socket);
    return selected;
}

std::vector<WaterWallUsersListener> waterwall_users_listeners(const std::map<int, Json>& launched, int id)
{
    std::vector<WaterWallUsersListener> listeners;
    for_each_api(launched.at(id), [&](const Json& listener, const Json& api) {
        auto address = local_address(listener);
        const auto& range = field(listener, "port-range");
        if (address.empty() || (range.is_array() && range.size() == 2 && range.front() != range.back() &&
                                field(listener, "multiport-backend") == "iptables")) {
            return false;
        }
        std::vector<bool> occupied(65536);
        for (const auto& [_, config] : launched) {
            mark_bound_tcp_ports(config, address, &listener, occupied);
        }
        // Share this bounded port list across all grants on this listener.
        auto ports = std::make_shared<std::vector<std::uint16_t>>();
        bool has_tcp_port = false;
        visit_port_ranges(listener, [&](unsigned first, unsigned last) {
            has_tcp_port = true;
            for (auto port = first; port <= last; ++port) {
                if (!occupied[port]) {
                    ports->push_back(static_cast<std::uint16_t>(port));
                    occupied[port] = true;
                }
            }
        });
        if (has_tcp_port) {
            listeners.push_back({std::move(address), std::move(ports), field(api, "clients")});
        }
        return false;
    });
    return listeners;
}

std::vector<WaterWallUsersEndpoint> waterwall_users_endpoints(std::span<const WaterWallUsersListener> listeners,
                                                             std::string_view config, std::string_view node,
                                                             WaterWallUsersOperation operation, bool& ambiguous)
{
    std::vector<WaterWallUsersEndpoint> endpoints;
    if (config.empty() || config.size() > 256 || config.find('\0') != config.npos) {
        return endpoints;
    }
    for (const auto& listener : listeners) {
        if (!listener.clients.is_array()) {
            continue;
        }
        for (const auto& client : listener.clients) {
            const auto name = text(field(client, "name"));
            const auto token = text(field(client, "token"));
            const bool valid_token =
                token.size() == 64 && token.find_first_not_of("0123456789abcdefABCDEF") == token.npos;
            if (!name.empty() && name.size() <= 256 && name.find('\0') == name.npos && valid_token &&
                allows_user_operation(client, config, node, operation)) {
                if (listener.ports->empty()) {
                    ambiguous = true;
                } else {
                    endpoints.push_back({listener.address, listener.ports, std::string(name), std::string(token),
                                         std::string(config)});
                }
                break;
            }
        }
    }
    return endpoints;
}

bool valid_waterwall_user_id(std::string_view id)
{
    return unsigned_number(id).value_or(0) != 0;
}

bool valid_waterwall_user_edit_token(std::string_view token)
{
    return token.size() == 48 && token.find_first_not_of("0123456789abcdef") == token.npos;
}

bool valid_waterwall_users_cursor(std::string_view cursor)
{
    return cursor.size() == 64 && cursor.find_first_not_of("0123456789abcdef") == cursor.npos;
}

WaterWallUsersReply request_waterwall_users(pid_t core_pid, std::span<const WaterWallUsersEndpoint> endpoints,
                                            std::string_view node, const WaterWallUsersRequest& request,
                                            bool legacy_list, const std::function<bool()>& run_is_current)
{
    const auto deadline = Clock::now() + 2s;
    auto reply = failure(Outcome::not_sent, "connection_failed");
    bool try_another = true;
    for (std::size_t i = 0; i < endpoints.size() && try_another; ++i) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        // A large range on one listener must leave time to try independent listeners.
        const auto remaining = static_cast<Clock::duration::rep>(endpoints.size() - i);
        const auto listener_deadline = now + (deadline - now) / remaining;
        // The ports share one API handler. A delivered write can only be confirmed by its own reply.
        const auto reply_deadline = is_mutation(request.operation) ? deadline : listener_deadline;
        const auto& ports = *endpoints[i].ports;
        for (std::size_t j = 0; j < ports.size() && try_another; ++j) {
            if (!run_is_current()) {
                reply = failure(Outcome::not_sent, "run_changed", "WaterWall changed run. Reload this user tab.");
                try_another = false;
                break;
            }
            const auto start = Clock::now();
            if (start >= listener_deadline) {
                break;
            }
            const auto ports_left = static_cast<Clock::duration::rep>(ports.size() - j);
            // Give even the first port in a wide range enough time for local peer verification.
            const auto share =
                std::max<Clock::duration>(peer_verification_timeout, (listener_deadline - start) / ports_left);
            auto attempt = request_users(core_pid, endpoints[i], ports[j], node, request, legacy_list,
                                         std::min(listener_deadline, start + share), reply_deadline);
            reply = std::move(attempt.reply);
            try_another = attempt.try_other_endpoint;
        }
    }
    if (legacy_list && !reply.error.empty()) {
        reply.result = reply.result.contains("reset_cursor") ? Json{{"reset_cursor", true}} : Json{};
    }
    return reply;
}

} // namespace wave
