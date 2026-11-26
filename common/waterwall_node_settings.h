#pragma once

#include "waterwall_nodes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace wave::node_settings {

enum class Kind {
    text,
    boolean,
    integer,
    number,
    choice,
    string_list,
    object,
    object_list,
    array,
    text_or_list,
    integer_or_boolean,
    port,
    port_list,
    port_range,
    json,
    node_target,
};

struct Field {
    const char* key;
    const char* label;
    Kind kind = Kind::text;
    const char* hint = "";
    bool required = false;
    double minimum = std::numeric_limits<std::int32_t>::min();
    double maximum = std::numeric_limits<std::int32_t>::max();
    std::span<const char* const> choices{};
    std::span<const Field> children{};
    std::array<const char*, 2> aliases{};
    bool nullable = false;
    bool case_insensitive = false;
    bool allow_random_port = false;
    bool string_alias_fallback = false;
};

inline const char* field_key(const nlohmann::json& object, const Field& field)
{
    const char* first_present = nullptr;
    for (const auto* key : {field.key, field.aliases[0], field.aliases[1]}) {
        if (!key) {
            continue;
        }
        const auto value = object.find(key);
        if (value != object.end()) {
            if (!field.string_alias_fallback || value->is_string()) {
                return key;
            }
            if (!first_present) {
                first_present = key;
            }
        }
    }
    return first_present ? first_present : field.key;
}

namespace detail {

using enum Kind;

inline constexpr double int_max = std::numeric_limits<std::int32_t>::max();
inline constexpr double uint_max = std::numeric_limits<std::uint32_t>::max();

constexpr Field option(const char* key, const char* label, Kind kind, const char* hint = "", bool required = false)
{
    return {.key = key, .label = label, .kind = kind, .hint = hint, .required = required};
}

constexpr Field range(const char* key, const char* label, double minimum, double maximum = int_max,
                      const char* hint = "", bool required = false, Kind kind = integer)
{
    return {.key = key, .label = label, .kind = kind, .hint = hint, .required = required,
            .minimum = minimum, .maximum = maximum};
}

constexpr Field select(const char* key, const char* label, std::span<const char* const> values,
                       const char* hint = "", bool required = false)
{
    return {.key = key, .label = label, .kind = choice, .hint = hint, .required = required, .choices = values};
}

constexpr Field group(const char* key, const char* label, std::span<const Field> fields, Kind kind = object,
                      bool required = false, std::array<const char*, 2> aliases = {}, const char* hint = "")
{
    return {.key = key, .label = label, .kind = kind, .hint = hint, .required = required, .children = fields,
            .aliases = aliases};
}

constexpr Field target(const char* key, const char* label, bool required = false,
                       std::array<const char*, 2> aliases = {})
{
    return {.key = key, .label = label, .kind = node_target, .required = required, .aliases = aliases};
}

inline constexpr const char* dns_modes[] = {"native", "direct", "chain"};
inline constexpr const char* dns_strategies[] = {
    "core-settings", "accept-dns-returned-order", "prefer-ipv4", "prefer-ipv6", "only-ipv4", "only-ipv6",
};
inline constexpr Field dns_policy[] = {
    option("resolve", "Resolve domains", boolean, "Set explicitly when configuring a DNS behaviour.", true),
    select("mode", "DNS mode", dns_modes, "Native by default."),
    select("strategy", "Address selection", dns_strategies, "Uses core settings by default."),
    option("servers", "DNS servers", string_list, "Numeric IP addresses, optionally with a port. Required in direct and chain modes."),
    target("dns-next", "DNS branch"),
};
inline constexpr auto dns = group("dns-behaviour", "DNS behaviour", dns_policy);
inline constexpr auto dns_override = group("dns-behaviour", "DNS behaviour", std::span{dns_policy}.subspan(2, 1));
inline constexpr Field domain_resolver[] = {
    option("resolve", "Resolve domains", boolean, "Enabled by default."),
    dns_policy[1], dns_policy[2], dns_policy[3], dns_policy[4],
    option("verbose", "Verbose DNS logging", boolean),
};

inline bool text_value(const nlohmann::json& value)
{
    return value.is_string() && value.get_ref<const std::string&>().find('\0') == std::string::npos;
}

inline char lower_ascii(char character)
{
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
}

inline bool choice_value(const nlohmann::json& value, const Field& field)
{
    if (!text_value(value)) {
        return false;
    }
    const auto& input = value.get_ref<const std::string&>();
    return std::ranges::any_of(field.choices, [&](std::string_view choice) {
        return field.case_insensitive ? std::ranges::equal(input, choice, {}, lower_ascii, lower_ascii)
                                      : input == choice;
    });
}

inline int port_number(std::string_view value)
{
    int port = 0;
    if (value.empty()) {
        return 0;
    }
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && port > 0 && port <= 65535
               ? port
               : 0;
}

inline bool random_or_numeric_port(const nlohmann::json& value)
{
    if (!text_value(value)) {
        return false;
    }
    auto port = std::string_view(value.get_ref<const std::string&>());
    if (!port.starts_with("random(") || !port.ends_with(')')) {
        return port_number(port) != 0;
    }
    port.remove_prefix(7);
    port.remove_suffix(1);
    const auto comma = port.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    const auto minimum = port_number(port.substr(0, comma));
    const auto maximum = port_number(port.substr(comma + 1));
    return minimum > 0 && maximum >= minimum;
}

inline bool numeric_value(const nlohmann::json& value, double minimum, double maximum, bool integral)
{
    if (!value.is_number()) {
        return false;
    }
    const auto result = value.get<double>();
    return std::isfinite(result) && result >= minimum && result <= maximum &&
           (!integral || std::trunc(result) == result);
}

inline bool setting_value(const nlohmann::json& value, const Field& field)
{
    switch (field.kind) {
    case text: return text_value(value) && (!field.required || value != "");
    case node_target: return text_value(value) && value != "";
    case boolean: return value.is_boolean();
    case integer: return numeric_value(value, field.minimum, field.maximum, true);
    case number: return numeric_value(value, field.minimum, field.maximum, false);
    case choice: return choice_value(value, field);
    case string_list: return value.is_array() && std::ranges::all_of(value, text_value);
    case text_or_list: return text_value(value) || (value.is_array() && std::ranges::all_of(value, text_value));
    case object: return value.is_object();
    case object_list: return value.is_array() && std::ranges::all_of(value, [](const auto& row) { return row.is_object(); });
    case array: return value.is_array();
    case integer_or_boolean: return value.is_boolean() || numeric_value(value, field.minimum, field.maximum, true);
    case port:
        return numeric_value(value, field.minimum, field.maximum, true) || choice_value(value, field) ||
               (field.allow_random_port && random_or_numeric_port(value));
    case port_list: {
        const auto is_port = [&](const auto& item) { return numeric_value(item, field.minimum, field.maximum, true); };
        return is_port(value) || (value.is_array() && !value.empty() && std::ranges::all_of(value, is_port));
    }
    case port_range:
        return value.is_array() && value.size() == 2 && numeric_value(value[0], field.minimum, field.maximum, true) &&
               numeric_value(value[1], field.minimum, field.maximum, true) && value[0] <= value[1];
    case json: return true;
    }
    return false;
}

inline std::string validate_fields(std::span<const Field> fields, const nlohmann::json& value,
                                   const std::string& path = {})
{
    for (const auto& field : fields) {
        const auto* key = field_key(value, field);
        const auto item = value.find(key);
        const auto field_path = path + key;
        if (item == value.end()) {
            if (field.required) {
                return field_path + " is required.";
            }
            continue;
        }
        if (item->is_null() && field.nullable) {
            continue;
        }
        if (!setting_value(*item, field)) {
            return std::string("Invalid value for ") + field_path + ".";
        }
        if (field.kind == object) {
            if (auto issue = validate_fields(field.children, *item, field_path + "."); !issue.empty()) {
                return issue;
            }
        } else if (field.kind == object_list) {
            for (std::size_t index = 0; index < item->size(); ++index) {
                auto issue = validate_fields(field.children, (*item)[index], field_path + "[" + std::to_string(index) + "].");
                if (!issue.empty()) {
                    return issue;
                }
            }
        }
    }
    return {};
}

} // namespace detail

} // namespace wave::node_settings

#include "waterwall_node_settings_network.h"
#include "waterwall_node_settings_packet.h"
#include "waterwall_node_settings_security.h"
#include "waterwall_node_settings_transport.h"
#include "waterwall_node_settings_checks.h"

namespace wave::node_settings {

inline std::span<const Field> fields(std::string_view type)
{
    struct TypeFields {
        std::string_view type;
        std::span<const Field> fields;
    };
    using namespace detail;
    // Settings read by the v1.6.0 constructors and their parsing helpers.
    // An empty field list records a built-in with no tunnel-specific settings.
    static constexpr TypeFields schemas[] = {
        {"APIServer", api_server},
        {"AuthenticationClient", authentication_client},
        {"AuthenticationServer", authentication_server},
        {"Bgp4Client", {}},
        {"Bgp4Server", {}},
        {"BlackHole", black_hole},
        {"Bridge", bridge},
        {"ConnectionFisherClient", connection_fisher},
        {"ConnectionFisherServer", {}},
        {"ConnectionStripeClient", stripe_client},
        {"ConnectionStripeServer", stripe_server},
        {"ConnectionToPackets", connection_to_packets},
        {"Disturber", disturber},
        {"DomainResolver", domain_resolver},
        {"EncryptionClient", encryption},
        {"EncryptionServer", encryption},
        {"EnetClient", enet},
        {"EnetServer", enet},
        {"FakeTcpHeaderClient", fake_tcp_header},
        {"FakeTcpHeaderServer", fake_tcp_header},
        {"GnsClient", gns},
        {"GnsServer", gns},
        {"HalfDuplexClient", half_duplex_client},
        {"HalfDuplexServer", half_duplex_server},
        {"HeaderClient", header_client},
        {"HeaderServer", header_server},
        {"HttpClient", http_client},
        {"HttpProxyClient", http_proxy_client},
        {"HttpProxyServer", http_proxy_server},
        {"HttpServer", http_server},
        {"IpManipulator", ip_manipulator},
        {"IpOverrider", ip_overrider},
        {"JunkDatagramSender", junk_datagram_sender},
        {"KcpClient", kcp},
        {"KcpServer", kcp},
        {"KeepAliveClient", keep_alive},
        {"KeepAliveServer", {}},
        {"LoggerTunnel", logger_tunnel},
        {"MuxClient", mux_client},
        {"MuxServer", mux_server},
        {"ObfuscatorClient", obfuscator},
        {"ObfuscatorServer", obfuscator},
        {"PacketCounter", packet_counter},
        {"PacketGenerator", packet_generator},
        {"PacketSplitStream", packet_split_stream},
        {"PacketsToConnection", packets_to_connection},
        {"PacketsToStream", packets_to_stream},
        {"Ping6Client", ping6},
        {"Ping6Server", ping6},
        {"PingClient", ping},
        {"PingServer", ping},
        {"QuicClient", quic_client},
        {"QuicServer", quic_server},
        {"RawSocket", raw_socket},
        {"RealityClient", reality_client},
        {"RealityServer", reality_server},
        {"ReverseClient", reverse_client},
        {"ReverseServer", reverse_server},
        {"Router", router},
        {"RtmpClient", rtmp_client},
        {"RtmpServer", rtmp_server},
        {"SniffRouter", sniff_router},
        {"Socks5Client", socks5_client},
        {"Socks5Server", socks5_server},
        {"SoftIpLimiter", soft_ip_limiter},
        {"SpeedLimit", speed_limit},
        {"SpeedTestClient", speed_test_client},
        {"SpeedTestServer", speed_test_server},
        {"StreamFragmenter", stream_fragmenter},
        {"StreamToPackets", stream_to_packets},
        {"SynReply", syn_reply},
        {"TcpConnector", tcp_connector},
        {"TcpListener", tcp_listener},
        {"TcpUdpConnector", tcp_udp_connector},
        {"TcpUdpListener", tcp_listener},
        {"Template", {}},
        {"TesterClient", tester_client},
        {"TesterServer", tester_server},
        {"TlsClient", tls_client},
        {"TlsServer", tls_server},
        {"TrojanClient", trojan_client},
        {"TrojanServer", trojan_server},
        {"TunDevice", tun_device},
        {"UdpConnector", udp_connector},
        {"UdpListener", listener_base},
        {"UdpOverTcpClient", {}},
        {"UdpOverTcpServer", {}},
        {"UdpStatelessSocket", udp_stateless_socket},
        {"UserController", user_controller},
        {"VlessClient", vless_client},
        {"VlessServer", vless_server},
        {"VmessClient", vmess_client},
        {"VmessServer", vmess_server},
        {"WireGuardClient", wireguard_client},
        {"WireGuardServer", wireguard_server},
    };
    static_assert(std::ranges::equal(schemas, waterwall_node_types, {}, &TypeFields::type));
    const auto schema = std::ranges::find(schemas, type, &TypeFields::type);
    return schema == std::end(schemas) ? std::span<const Field>{} : schema->fields;
}

inline bool valid_value(const nlohmann::json& value, const Field& field)
{
    return (value.is_null() && field.nullable) || detail::setting_value(value, field);
}

inline std::string validate(std::string_view type, const nlohmann::json& settings)
{
    if (!settings.is_object()) {
        return "Node settings must be a JSON object.";
    }
    if (auto issue = detail::validate_fields(fields(type), settings); !issue.empty()) {
        return issue;
    }
    if (type == "DomainResolver") {
        if (const auto issue = detail::check_dns_policy(settings, type); !issue.empty()) {
            return std::string(issue);
        }
    } else if (const auto dns = settings.find("dns-behaviour");
               dns != settings.end() && std::ranges::find(fields(type), std::string_view("dns-behaviour"), &Field::key)
                                            != fields(type).end()) {
        if (const auto issue = detail::check_dns_policy(*dns, type); !issue.empty()) {
            return std::string(issue);
        }
    }
    if (const auto issue = detail::check_node_settings(type, settings); !issue.empty()) {
        return std::string(issue);
    }
    if (type == "PacketsToConnection" || type == "WireGuardServer") {
        if (auto issue = detail::check_fake_dns(settings); !issue.empty()) {
            return issue;
        }
    }
    return detail::validate_security_settings(type, settings);
}

} // namespace wave::node_settings
