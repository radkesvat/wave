#pragma once

#include <arpa/inet.h>

#include <functional>
#include <initializer_list>

namespace wave::node_settings::detail {

using Json = nlohmann::json;

inline const Json& setting(const Json& object, const char* key)
{
    static const Json missing;
    const auto found = object.find(key);
    return found == object.end() ? missing : *found;
}

inline int count_settings(const Json& object, std::initializer_list<const char*> keys)
{
    return std::ranges::count_if(keys, [&](const auto* key) { return object.contains(key); });
}

inline bool valid_ip(int family, std::string_view address)
{
    std::array<unsigned char, 16> bytes{};
    return ::inet_pton(family, std::string(address).c_str(), bytes.data()) == 1;
}

inline bool valid_dns_endpoint(const Json& value)
{
    const auto endpoint = std::string_view(value.get_ref<const std::string&>());
    if (endpoint.size() > 64) {
        return false;
    }
    if (valid_ip(AF_INET, endpoint) || valid_ip(AF_INET6, endpoint)) {
        return true;
    }
    if (endpoint.starts_with('[')) {
        const auto end = endpoint.find(']');
        return end != std::string_view::npos && valid_ip(AF_INET6, endpoint.substr(1, end - 1)) &&
               (end + 1 == endpoint.size() ||
                (endpoint[end + 1] == ':' && port_number(endpoint.substr(end + 2)) != 0));
    }
    const auto colon = endpoint.find(':');
    return colon != std::string_view::npos && valid_ip(AF_INET, endpoint.substr(0, colon)) &&
           port_number(endpoint.substr(colon + 1)) != 0;
}

inline std::string_view check_dns_policy(const Json& policy, std::string_view type)
{
    const auto& mode = setting(policy, "mode");
    const bool chain = mode == "chain";
    const bool direct = mode == "direct";
    if (setting(policy, "resolve") == false) {
        return count_settings(policy, {"mode", "strategy", "servers", "dns-next"}) > 0
                   ? "Disabled DNS cannot specify a mode, strategy, servers or a DNS branch." : "";
    }
    const auto& strategy = setting(policy, "strategy");
    if (type == "ConnectionToPackets" && !strategy.is_null() && strategy != "only-ipv4" &&
        strategy != "core-settings") {
        return "ConnectionToPackets DNS requires the only-ipv4 strategy.";
    }
    if (!chain && policy.contains("dns-next")) {
        return "A DNS branch requires chain mode.";
    }
    const bool explicit_branch = type == "ConnectionToPackets" || type == "HttpProxyClient" ||
                                 type == "TcpConnector" || type == "TcpUdpConnector" || type == "UdpConnector" ||
                                 type == "UdpStatelessSocket";
    if (chain && explicit_branch && !policy.contains("dns-next")) {
        return "Chain DNS requires a DNS branch for this node type.";
    }
    const auto servers = policy.find("servers");
    if (chain || direct) {
        if (servers == policy.end() || servers->empty() || servers->size() > 32 ||
            !std::ranges::all_of(*servers, valid_dns_endpoint)) {
            return "Direct or chain DNS requires 1–32 numeric DNS endpoints.";
        }
    } else if (servers != policy.end()) {
        return "DNS servers require direct or chain mode.";
    }
    return {};
}

inline std::string_view check_connectors(std::string_view type, const Json& settings)
{
    const auto count = count_settings(settings, {"addresses", "adresses", "destinations"});
    if (count == 0) {
        return setting(settings, "address").is_string() && setting(settings, "address") != "" && settings.contains("port")
                   ? "" : "Specify a destination address and port or a weighted destination list.";
    }
    const auto& field = type == "UdpConnector" ? udp_destination_group : tcp_destination_group;
    if (count > 1 || settings.contains("address") || settings.contains("port") ||
        settings.at(field_key(settings, field)).empty()) {
        return "Specify one nonempty weighted destination list or a single destination address and port.";
    }
    return {};
}

inline bool nonempty_text(const Json& value)
{
    return text_value(value) && value != "";
}

inline bool valid_domain_patterns(const Json& patterns)
{
    return patterns.is_array() ? !patterns.empty() && std::ranges::all_of(patterns, nonempty_text)
                               : nonempty_text(patterns);
}

inline std::string_view check_sniff_routes(const Json& routes)
{
    if (routes.empty()) {
        return "Remove the routes setting or enter at least one route.";
    }
    for (const auto& row : routes) {
        bool domains_required = !row.contains("detection");
        const auto& detection = setting(row, "detection");
        const auto check_mode = [&](const Json& item) {
            auto mode = item.get<std::string>();
            std::ranges::transform(mode, mode.begin(), lower_ascii);
            if (mode == "http1" || mode == "tls") {
                domains_required = true;
                return true;
            }
            return mode == "reverse" || mode == "reverse-tls" || mode == "reverse-handshake";
        };
        if ((detection.is_string() && !check_mode(detection)) ||
            (detection.is_array() && (detection.empty() || !std::ranges::all_of(detection, check_mode)))) {
            return "Detection accepts http1, tls, reverse, reverse-tls or reverse-handshake.";
        }
        const auto& domains = setting(row, "domains");
        const auto& domain = setting(row, "domain");
        if ((domains_required && domains.is_null() && domain.is_null()) ||
            (!domains.is_null() && !valid_domain_patterns(domains)) ||
            (!domain.is_null() && (!domains.is_null() || !nonempty_text(domain)))) {
            return "HTTP/1.1 or TLS sniffing needs domain patterns. Use either domains or domain on a route.";
        }
    }
    return {};
}

inline bool hexadecimal(char character)
{
    const auto lower = lower_ascii(character);
    return (character >= '0' && character <= '9') || (lower >= 'a' && lower <= 'f');
}

inline std::string_view check_fake_tcp(const Json& settings)
{
    const auto range = settings.find("client-port-range");
    if (range != settings.end() && (settings.contains("client-port") ||
        (*range)[1].get<int>() - (*range)[0].get<int>() >= 1024)) {
        return "Choose a single client port or a range of up to 1024 ports.";
    }
    const int first = range != settings.end() ? (*range)[0].get<int>() : settings.value("client-port", 40001);
    const int last = range != settings.end() ? (*range)[1].get<int>() : settings.value("client-port", 40016);
    const int server = settings.value("server-port", 40000);
    if (server >= first && server <= last) {
        return "The server port must be outside the client port selection.";
    }
    const auto options = settings.find("options");
    if (options != settings.end()) {
        const auto& hex = options->get_ref<const std::string&>();
        if (hex.size() > 80 || hex.size() % 8 != 0 || !std::ranges::all_of(hex, hexadecimal)) {
            return "TCP options must contain up to 40 bytes of hex in four-byte groups.";
        }
    }
    const auto flags = settings.find("flags");
    if (flags != settings.end() && (flags->empty() || flags->size() > 32)) {
        return "Specify one to 32 TCP flag combinations.";
    }
    if (setting(settings, "acknowledgment") == "random_packet" && setting(settings, "increment-acknowledgment") == true) {
        return "A random acknowledgment per packet cannot be incremented.";
    }
    return {};
}

inline std::string_view check_fragmenter(const Json& settings)
{
    const bool timed = settings.at("mode") == "timed";
    if (settings.contains("count") == timed || settings.contains("duration-ms") != timed) {
        return "Counter mode requires count. Timed mode requires duration-ms. Enter only the setting for the selected mode.";
    }
    if (settings.contains("tls-hello-timeout-ms") && setting(settings, "tls-hello-fragment") != true) {
        return "The TLS hello timeout requires TLS hello fragmentation.";
    }
    double previous_offset = 0;
    const auto& cuts = settings.at("cuts");
    if (cuts.size() > 64) {
        return "Specify up to 64 fragment cuts.";
    }
    for (const auto& cut : cuts) {
        if (!cut.is_array() || cut.size() != 3 || !numeric_value(cut[0], 1, uint_max, true) ||
            cut[0].get<double>() <= previous_offset || !numeric_value(cut[1], 0, uint_max, true) ||
            !numeric_value(cut[2], 0, 100, true)) {
            return "Each cut needs [offset, delay_ms, chance_percent] with positive, strictly increasing offsets.";
        }
        previous_offset = cut[0].get<double>();
    }
    return {};
}

inline std::string_view check_ping6(const Json& settings)
{
    const bool encapsulated = setting(settings, "mode") == "ipv6-in-ipv4";
    const int count = count_settings(settings, {"local-ipv6", "peer-ipv6"});
    if (!encapsulated && count == 0) {
        return {};
    }
    if (encapsulated && count == 2 && valid_ip(AF_INET6, settings.at("local-ipv6").get_ref<const std::string&>()) &&
        valid_ip(AF_INET6, settings.at("peer-ipv6").get_ref<const std::string&>())) {
        return {};
    }
    return "Set both local and peer IPv6 addresses for ipv6-in-ipv4 mode. Omit them in direct mode.";
}

inline std::string_view check_kcp(const Json& settings)
{
    if (settings.value("kcp-initial-cwnd", 1) > settings.value("kcp-send-window", 8192) ||
        settings.value("kcp-ack-batch", 1) > settings.value("kcp-recv-window", 8192)) {
        return "The initial congestion window and ACK batch must fit their respective send and receive windows.";
    }
    if (setting(settings, "fec") == true &&
        (settings.value("send-mtu", 126) < 126 ||
         settings.value("fec-data-shards", 10) + settings.value("fec-parity-shards", 3) > 255)) {
        return "Forward error correction requires an MTU of at least 126 bytes and up to 255 total shards.";
    }
    return {};
}

inline std::string_view check_api_clients(const Json& clients)
{
    if (clients.empty() || clients.size() > 256) {
        return "Enter one to 256 API clients.";
    }
    for (const auto& client : clients) {
        const auto& token = client.at("token").get_ref<const std::string&>();
        if (token.size() != 64 || !std::ranges::all_of(token, hexadecimal)) {
            return "Each API client needs a token containing exactly 64 hexadecimal digits.";
        }
        const auto& permissions = client.at("allow");
        if (permissions.empty() || permissions.size() > 512) {
            return "Each API client needs one to 512 permission entries.";
        }
        for (const auto& permission : permissions) {
            const auto& operations = permission.at("operations");
            if (operations.empty() || operations.size() > 64 ||
                !std::ranges::all_of(operations, [](const Json& op) {
                    return op != "" && op != "*" && op.get_ref<const std::string&>().size() <= 64;
                })) {
                return "Each API permission needs one to 64 explicitly named operations.";
            }
        }
    }
    return {};
}

inline bool ipv4_cidr(std::string_view value)
{
    const auto slash = value.find('/');
    if (slash == std::string_view::npos || slash + 1 == value.size()) {
        return false;
    }
    int prefix = 0;
    const auto result = std::from_chars(value.data() + slash + 1, value.data() + value.size(), prefix);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && prefix >= 0 && prefix <= 32 &&
           valid_ip(AF_INET, value.substr(0, slash));
}

inline bool ipv4_text(const Json& value)
{
    return text_value(value) && valid_ip(AF_INET, value.get_ref<const std::string&>());
}

inline std::string check_fake_dns(const Json& settings)
{
    if (count_settings(settings, {"fake-dns", "fake_dns", "mapdns"}) > 1) {
        return "Use a single fake DNS setting name.";
    }
    const auto& field = packets_to_connection[3];
    const auto fake_dns = settings.find(field_key(settings, field));
    if (fake_dns == settings.end() || fake_dns->is_boolean()) {
        return {};
    }
    if (!fake_dns->is_object()) {
        return "Fake DNS must be a boolean or an object.";
    }
    if (auto issue = validate_fields(fake_dns_fields, *fake_dns, "fake-dns."); !issue.empty()) {
        return issue;
    }
    for (const auto* key : {"address", "network", "netmask"}) {
        if (fake_dns->contains(key) && !ipv4_text(fake_dns->at(key))) {
            return std::string("Fake DNS ") + key + " requires an IPv4 address.";
        }
    }
    return {};
}

inline std::string_view check_packet_analysis(std::string_view type, const Json& settings)
{
    const auto& cidrs = settings.at(field_key(settings, packet_generator[0]));
    const auto valid_range = [](const Json& item) { return ipv4_cidr(item.get_ref<const std::string&>()); };
    if (cidrs.is_array() ? cidrs.empty() || !std::ranges::all_of(cidrs, valid_range) : !valid_range(cidrs)) {
        return "Enter at least one valid IPv4 CIDR range.";
    }
    if (type == "PacketGenerator") {
        if (!ipv4_text(settings.at("dest-ipv4"))) {
            return "Enter a valid destination IPv4 address.";
        }
        const auto& protocol = settings.at("protocol-number");
        if (protocol != "icmp" && protocol != "ICMP" &&
            (settings.value("dest-port", 0) == 0 || !settings.contains("src-port") || settings.at("src-port") == 0)) {
            return "TCP, UDP and ALL require a nonzero destination port and a nonzero or random source port.";
        }
        return {};
    }

    const auto& protocol = settings.at("protocol-number");
    if (protocol.is_string() && std::ranges::equal(protocol.get_ref<const std::string&>(), std::string_view("all"), {},
                                                   lower_ascii, std::identity{})) {
        return {};
    }
    const auto protocol_number = [](const Json& value) {
        if (numeric_value(value, 0, 255, true)) {
            return value.get<int>();
        }
        if (value.is_string()) {
            auto name = value.get<std::string>();
            std::ranges::transform(name, name.begin(), lower_ascii);
            if (name == "tcp") {
                return 6;
            }
            if (name == "udp") {
                return 17;
            }
            if (name == "icmp") {
                return 1;
            }
        }
        return -1;
    };
    if (protocol.is_array()) {
        std::array<bool, 256> seen{};
        if (!protocol.empty() && std::ranges::all_of(protocol, [&](const auto& item) {
                const int number = protocol_number(item);
                if (number < 0 || seen[number]) {
                    return false;
                }
                seen[number] = true;
                return true;
            })) {
            return {};
        }
    } else if (protocol_number(protocol) >= 0) {
        return {};
    }
    return "Choose TCP, UDP, ICMP, ALL, an IP protocol number, or a nonempty list of distinct protocols.";
}

inline std::string_view check_node_settings(std::string_view type, const Json& settings)
{
    if (type == "TcpListener" || type == "UdpListener" || type == "TcpUdpListener") {
        return count_settings(settings, {"port", "port-range"}) == 1 ? "" : "Choose a listen port, a port list or a port range.";
    }
    if (type == "TcpConnector" || type == "UdpConnector" || type == "TcpUdpConnector") {
        return check_connectors(type, settings);
    }
    if (type == "Router" && settings.contains("rules") && settings.at("rules").empty()) {
        return "Remove the rules setting or enter at least one routing rule.";
    }
    if (type == "SniffRouter" && settings.contains("routes")) {
        return check_sniff_routes(settings.at("routes"));
    }
    if (type == "HalfDuplexClient" && settings.contains("upload") != settings.contains("download")) {
        return "Choose both upload and download branches or leave both unset.";
    }
    if (type == "PacketSplitStream" && settings.at("up") == settings.at("down")) {
        return "Choose different up and down targets.";
    }
    if (type == "IpManipulator") {
        if (settings.contains("protoswap-tcp-2")) {
            return "WaterWall 1.6.0 only supports one TCP replacement protocol number. Remove protoswap-tcp-2.";
        }
        if ((settings.contains("smuggle-sni") && !settings.contains("real-sni-upstream-node")) ||
            (setting(settings, "smuggle-fin") == true && !settings.contains("real-fin-upstream-node"))) {
            return "Choose an upstream node for each enabled SNI or FIN branch.";
        }
    }
    if ((type == "KcpClient" || type == "KcpServer" || type == "EnetClient" || type == "EnetServer") &&
        settings.value("ping-interval-ms", 10000) >= settings.value("no-recv-timeout-ms", 30000)) {
        return "The ping interval must be shorter than the receive timeout.";
    }
    if (type == "KcpClient" || type == "KcpServer") {
        return check_kcp(settings);
    }
    if (type == "FakeTcpHeaderClient" || type == "FakeTcpHeaderServer") {
        return check_fake_tcp(settings);
    }
    if (type == "Ping6Client" || type == "Ping6Server") {
        return check_ping6(settings);
    }
    if (type == "StreamFragmenter") {
        return check_fragmenter(settings);
    }
    if (type == "MuxClient") {
        const auto& mode = settings.at("mode");
        const auto* required_key = mode == "timer" ? "connection-duration-ms" :
                                   mode == "counter" ? "connection-capacity" : "per-worker-connections-count";
        return settings.contains(required_key) ? "" : "Enter the value required by the selected connection mode.";
    }
    if (type == "SpeedLimit" && count_settings(settings, {"bytes-per-sec", "kilo-bytes-per-sec", "mega-bytes-per-sec"}) != 1) {
        return "Enter exactly one rate.";
    }
    if (type == "SpeedTestClient" &&
        count_settings(settings, {"target-bits-per-sec", "udp-target-bits-per-sec", "target-megabits-per-sec"}) > 1) {
        return "Enter at most one target bit rate.";
    }
    if ((type == "QuicClient" || type == "QuicServer") &&
        std::max(settings.value("receive-window-bytes", 1048576), (settings.value("max-buffered-buffers", 1024) + 1) * 24)
            > settings.value("max-buffered-bytes", 2097152)) {
        return "The receive window and buffered entry allocations must fit the byte limit.";
    }
    if (type == "RawSocket") {
        if (settings.contains("bypass-conntrack")) {
            return "WaterWall 1.6.0 requires connection tracking bypass. Remove bypass-conntrack.";
        }
        if (settings.contains("capture-protocols") && settings.contains("dont-capture-protocols")) {
            return "Choose either allowed or excluded capture protocols.";
        }
        if (count_settings(settings, {"capture-ips", "listen-ips", "capture-ip"}) != 0 &&
            !settings.contains("capture-filter-mode")) {
            return "Capture ranges require a capture filter mode.";
        }
    }
    if (type == "APIServer") {
        return check_api_clients(settings.at("clients"));
    }
    if (type == "TesterClient" || type == "TesterServer") {
        const bool packet_mode = setting(settings, "packet-mode") == true;
        const bool stateless = setting(settings, "packet-stateless") == true;
        if (!packet_mode && (stateless || settings.contains("packet-ipv4"))) {
            return "Stateless packets and packet IPv4 settings require packet mode.";
        }
        if (stateless && settings.value("max-payload-size", 0) > 0) {
            return "Maximum payload size is unavailable with stateless packets.";
        }
    }
    if (type == "PacketCounter" || type == "PacketGenerator") {
        return check_packet_analysis(type, settings);
    }
    if (type == "ReverseServer" && settings.value("activation-min-size", 64) >
                                       settings.value("activation-max-size", 1024)) {
        return "Minimum activation size cannot exceed maximum activation size.";
    }
    if (type == "TunDevice") {
        if (!ipv4_cidr(settings.at("device-ip").get_ref<const std::string&>())) {
            return "Enter an IPv4 address and prefix for the device.";
        }
        const auto& dns = setting(settings, "dns");
        if (!dns.is_null() && (dns.empty() || dns.size() > 2 || !std::ranges::all_of(dns, ipv4_text))) {
            return "Device DNS must contain one or two IPv4 addresses.";
        }
    }
    return {};
}

} // namespace wave::node_settings::detail
