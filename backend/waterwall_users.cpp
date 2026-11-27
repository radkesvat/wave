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
#include <map>
#include <span>
#include <sstream>
#include <thread>

namespace wave {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t page_size = 50;
constexpr std::size_t max_response_bytes = 256 * 1024;

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
    return schematic_field(schematic_field(waterwall_schematic(), "config"), "live_users");
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

std::uint16_t listener_port(const Json& settings)
{
    const auto& port = field(settings, "port");
    const auto& range = field(settings, "port-range");
    const bool socket_range =
        port.is_null() && range.is_array() && range.size() == 2 &&
        (range.front() == range.back() || field(settings, "multiport-backend") == "socket");
    const auto& first = socket_range ? range.front() : port.is_array() && !port.empty() ? port.front() : port;
    return first.is_number_integer() && first >= 1 && first <= 65535 ? first.get<std::uint16_t>() : 0;
}

bool can_receive_at(const Json& settings, const WaterWallUsersEndpoint& endpoint)
{
    const auto port = endpoint.port;
    const auto& ports = field(settings, "port");
    const auto& range = field(settings, "port-range");
    const bool matching_port =
        ports == port || (ports.is_array() && std::ranges::find(ports, Json(port)) != ports.end()) ||
        (range.is_array() && range.size() == 2 && range.front().is_number_integer() &&
         range.back().is_number_integer() && range.front() <= port && range.back() >= port);
    if (!matching_port) {
        return false;
    }
    const std::string address(text(field(settings, "address")));
    in_addr v4{};
    in6_addr v6{};
    if (::inet_pton(AF_INET, address.c_str(), &v4) == 1) {
        return v4.s_addr == INADDR_ANY ? endpoint.address.find(':') == std::string::npos
                                       : address == endpoint.address;
    }
    if (::inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&v6)) {
            return ::inet_pton(AF_INET, endpoint.address.c_str(), &v4) == 1 &&
                   std::memcmp(&v4, v6.s6_addr + 12, sizeof(v4)) == 0;
        }
        return IN6_IS_ADDR_UNSPECIFIED(&v6) || (IN6_IS_ADDR_LOOPBACK(&v6) && endpoint.address == "::1");
    }
    // A nonnumeric bind address might also resolve to the API's loopback endpoint.
    return true;
}

bool allows_list_users(const Json& client, std::string_view config, std::string_view node)
{
    const auto& allow = field(client, "allow");
    return allow.is_array() && std::ranges::any_of(allow, [&](const Json& entry) {
               const auto& operations = field(entry, "operations");
               return field(entry, "config") == config && field(entry, "node") == node &&
                      operations.is_array() &&
                      std::ranges::find(operations, "list-users") != operations.end();
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

bool owns_socket(const std::filesystem::path& process, const std::string& inode)
{
    const auto socket = "socket:[" + inode + ']';
    std::error_code ec;
    for (std::filesystem::directory_iterator fd(process / "fd", ec), end; !ec && fd != end;
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
                if (owns_socket(process, inode)) {
                    return true;
                }
                break;
            }
        }
        std::this_thread::sleep_until(std::min(deadline, Clock::now() + 20ms));
    }
    return false;
}

std::string exchange(pid_t core_pid, const WaterWallUsersEndpoint& endpoint, std::string& frame)
{
    const auto deadline = Clock::now() + 2s;
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    v4.sin_family = AF_INET;
    v6.sin6_family = AF_INET6;
    v4.sin_port = v6.sin6_port = htons(endpoint.port);
    const bool is_v4 = ::inet_pton(AF_INET, endpoint.address.c_str(), &v4.sin_addr) == 1;
    if (!is_v4 && ::inet_pton(AF_INET6, endpoint.address.c_str(), &v6.sin6_addr) != 1) {
        return {};
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
        return {};
    }
    if (::connect(socket.fd, address, is_v4 ? sizeof(v4) : sizeof(v6)) < 0) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (errno != EINPROGRESS || !wait_for(socket.fd, POLLOUT, deadline) ||
            ::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            return {};
        }
    }
    if (!wait_for_core_peer(socket.fd, address, core_pid, std::min(deadline, Clock::now() + 300ms))) {
        return {};
    }
    const auto send = [](int fd, const char* data, std::size_t size) {
        return ::send(fd, data, size, MSG_NOSIGNAL);
    };
    const auto receive = [](int fd, char* data, std::size_t size) { return ::recv(fd, data, size, 0); };
    std::uint32_t size = 0;
    if (!transfer_all(socket.fd, frame, POLLOUT, deadline, send) ||
        !transfer_all(socket.fd, {reinterpret_cast<char*>(&size), sizeof(size)}, POLLIN, deadline, receive)) {
        return {};
    }
    size = ntohl(size);
    if (size == 0 || size > max_response_bytes) {
        return {};
    }
    std::string body(size, '\0');
    return transfer_all(socket.fd, body, POLLIN, deadline, receive) ? body : std::string{};
}

Json parse(std::string_view body)
{
    bool too_deep = false;
    auto parsed = Json::parse(body, [&](int depth, auto, auto&) { return !(too_deep |= depth > 16); }, false);
    return too_deep || !parsed.is_object() ? Json{} : std::move(parsed);
}

std::optional<std::uint64_t> unsigned_number(const Json& value)
{
    const auto number = text(value);
    if (number.empty()) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto end = number.data() + number.size();
    const auto parsed = std::from_chars(number.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional{result} : std::nullopt;
}

Json public_page(const Json& body)
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

} // namespace

bool has_waterwall_users_api(const Json& config)
{
    return for_each_api(config, [](const Json&, const Json&) { return true; });
}

std::size_t waterwall_tcp_bindings(const Json& config, const WaterWallUsersEndpoint& endpoint)
{
    const auto& nodes = field(config, "nodes");
    if (!nodes.is_array()) {
        return 0;
    }
    const auto& listener_types = schematic_field(live_users_spec(), "tcp_listener_nodes");
    return static_cast<std::size_t>(std::ranges::count_if(nodes, [&](const Json& node) {
        const auto& type = field(node, "type");
        return std::ranges::find(listener_types, type) != listener_types.end() &&
               can_receive_at(field(node, "settings"), endpoint);
    }));
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
    WaterWallUsersEndpoint endpoint{"127.0.0.1", 0, {}, {}, {}};
    std::optional<std::uint16_t> selected;
    // Check the whole dynamic port range from a random starting point, including saved listeners.
    for (unsigned step = 0; step < port_count; ++step) {
        endpoint.port = static_cast<std::uint16_t>(first_port + (offset + step) % port_count);
        if (std::ranges::any_of(configs, [&](const Json& config) {
                return waterwall_tcp_bindings(config, endpoint) != 0;
            })) {
            continue;
        }
        address.sin_port = htons(endpoint.port);
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            selected = endpoint.port;
            break;
        }
        if (errno != EADDRINUSE) {
            break;
        }
    }
    ::close(socket);
    return selected;
}

std::optional<WaterWallUsersEndpoint> waterwall_users_endpoint(const Json& config, std::string_view node)
{
    const auto logical_name = text(field(config, "name"));
    if (logical_name.empty()) {
        return std::nullopt;
    }
    std::optional<WaterWallUsersEndpoint> endpoint;
    for_each_api(config, [&](const Json& listener, const Json& api) {
        auto address = local_address(listener);
        const auto port = listener_port(listener);
        const auto& clients = field(api, "clients");
        if (address.empty() || port == 0 || !clients.is_array()) {
            return false;
        }
        for (const auto& client : clients) {
            const auto name = text(field(client, "name"));
            const auto token = text(field(client, "token"));
            if (!name.empty() && name.size() <= 256 && name.find('\0') == name.npos &&
                valid_waterwall_users_cursor(token) && allows_list_users(client, logical_name, node)) {
                endpoint.emplace(std::move(address), port, std::string(name), std::string(token),
                                 std::string(logical_name));
                return true;
            }
        }
        return false;
    });
    return endpoint;
}

bool valid_waterwall_users_cursor(std::string_view cursor)
{
    return cursor.size() == 64 && cursor.find_first_not_of("0123456789abcdefABCDEF") == cursor.npos;
}

WaterWallUsersPage list_waterwall_users(pid_t core_pid, const WaterWallUsersEndpoint& endpoint,
                                        std::string_view node, std::string_view cursor)
{
    // There is one request per connection, so no other in-flight frame can share this ID.
    constexpr std::string_view id = "wave-users";
    Json query = {{"limit", page_size}};
    if (!cursor.empty()) {
        query["cursor"] = cursor;
    }
    const Json request{{"id", id},
                       {"auth", {{"client", endpoint.client}, {"token", endpoint.token}}},
                       {"selector", {{"config", endpoint.config}, {"node", node}}},
                       {"operation", "list-users"},
                       {"content_type", "application/json"},
                       {"body_encoding", "utf8"},
                       {"body", query.dump()}};
    const auto payload = request.dump();
    const auto size = htonl(static_cast<std::uint32_t>(payload.size()));
    std::string frame(reinterpret_cast<const char*>(&size), sizeof(size));
    frame += payload;
    const auto response = exchange(core_pid, endpoint, frame);
    if (response.empty()) {
        return {nullptr, "Waiting for the WaterWall API to respond."};
    }
    const auto envelope = parse(response);
    if (field(envelope, "id") != id || !envelope.contains("error")) {
        return {nullptr, "The WaterWall API returned an invalid reply."};
    }
    if (!field(envelope, "error").is_null()) {
        return {nullptr,
                "The WaterWall API declined the user list request. Check the API client permission."};
    }
    const auto& result = field(envelope, "result");
    const auto body = parse(text(field(result, "body")));
    if (field(result, "body_encoding") != "utf8" || field(result, "content_type") != "application/json" ||
        !field(result, "code").is_number_integer() || body.is_null()) {
        return {nullptr, "The WaterWall API returned an invalid reply."};
    }
    if (field(result, "code") != 0) {
        const auto& code = field(field(body, "error"), "code");
        if (!cursor.empty() && (code == "stale_cursor" || code == "invalid_cursor")) {
            return {{{"reset_cursor", true}},
                    "The WaterWall user list changed. Reloading from the first page."};
        }
        return {nullptr, "WaterWall could not list users right now."};
    }
    auto page = public_page(body);
    return page.is_null() ? WaterWallUsersPage{nullptr, "The WaterWall API returned an invalid user list."}
                          : WaterWallUsersPage{std::move(page), {}};
}

} // namespace wave
