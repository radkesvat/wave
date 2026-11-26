#pragma once

namespace wave::node_settings::detail {

inline constexpr auto send_buffer = range("large-send-buffer", "Send buffer", 1, int_max,
                                           "Boolean preset or buffer size in bytes.", false, integer_or_boolean);
inline constexpr auto recv_buffer = range("large-recv-buffer", "Receive buffer", 1, int_max,
                                           "Boolean preset or buffer size in bytes.", false, integer_or_boolean);
inline constexpr auto interface_name = option("interface", "Network interface", text);
inline constexpr auto source_ip = option("source-ip", "Source IP address", text);
inline constexpr auto fwmark = option("fwmark", "Firewall mark", integer);
inline constexpr auto no_delay = option("nodelay", "Disable Nagle's algorithm", boolean);
inline constexpr auto fast_open = option("fastopen", "TCP Fast Open", boolean);
inline constexpr auto target_address = option("address", "Destination address", text,
                                               "Address, hostname, src_context->address or dest_context->address.");
inline constexpr const char* dynamic_ports[] = {"src_context->port", "dest_context->port"};
inline constexpr Field tcp_port{.key = "port", .label = "Destination port", .kind = port,
                                .hint = "A port in 1–65535, src_context->port or dest_context->port.",
                                .minimum = 1, .maximum = 65535, .choices = dynamic_ports};
inline constexpr Field udp_port{.key = "port", .label = "Destination port", .kind = port,
                                .hint = "A port in 1–65535, a context port or random(min,max).",
                                .minimum = 1, .maximum = 65535, .choices = dynamic_ports, .allow_random_port = true};

constexpr Field required(Field field)
{
    field.required = true;
    return field;
}

constexpr Field nullable(Field field)
{
    field.nullable = true;
    return field;
}

inline constexpr auto weight = range("weight", "Selection weight", 1, int_max, "", true);
inline constexpr Field tcp_destinations[] = {
    required(target_address), required(tcp_port), weight, no_delay, fast_open, fwmark, dns_override,
    nullable(interface_name), nullable(source_ip), send_buffer, recv_buffer,
};
inline constexpr Field udp_destinations[] = {required(target_address), required(udp_port), weight};
inline constexpr std::array<const char*, 2> destination_aliases{"adresses", "destinations"};
inline constexpr auto tcp_destination_group =
    group("addresses", "Weighted destinations", tcp_destinations, object_list, false, destination_aliases,
          "Use this list or a single destination address and port.");
inline constexpr auto udp_destination_group =
    group("addresses", "Weighted destinations", udp_destinations, object_list, false, destination_aliases,
          "Use this list or a single destination address and port.");
inline constexpr const char* balance_modes[] = {"connection", "packet"};
inline constexpr auto balance_mode = select("balance-mode", "UDP load balancing", balance_modes,
                                            "Connection-based by default.");

inline constexpr Field tcp_connector[] = {
    target_address, tcp_port, tcp_destination_group, dns, no_delay, fast_open,
    fwmark, interface_name, source_ip, send_buffer, recv_buffer,
};
inline constexpr Field udp_connector[] = {
    target_address, udp_port, udp_destination_group, dns, balance_mode,
    fwmark, interface_name, source_ip, send_buffer, recv_buffer,
};
inline constexpr Field tcp_udp_connector[] = {
    target_address, tcp_port, tcp_destination_group, dns, balance_mode, no_delay, fast_open,
    fwmark, interface_name, source_ip, send_buffer, recv_buffer,
};

inline constexpr const char* multiport_backends[] = {"iptables", "socket"};
inline constexpr Field listener_base[] = {
    option("address", "Listen address", text, "IP address to bind.", true),
    range("port", "Listen port", 1, 65535, "One port or a list of ports. Use with no port range.", false, port_list),
    range("port-range", "Listen port range", 1, 65535, "Two ports in ascending order.", false, port_range),
    select("multiport-backend", "Multiport backend", multiport_backends),
    option("balance-group", "Balance group", text),
    option("balance-interval", "Balance interval", integer),
    interface_name,
    fwmark,
    option("whitelist", "Allowed address ranges", string_list, "Each entry is an IP address or CIDR."),
    option("blacklist", "Blocked address ranges", string_list, "Each entry is an IP address or CIDR."),
    send_buffer,
    recv_buffer,
};
inline constexpr Field tcp_listener[] = {
    listener_base[0], listener_base[1], listener_base[2], listener_base[3], listener_base[4], listener_base[5],
    listener_base[6], listener_base[7], listener_base[8], listener_base[9], listener_base[10], listener_base[11],
    no_delay,
    range("initial-idle-timeout-ms", "Initial idle timeout (ms)", 1),
    range("active-idle-timeout-ms", "Active idle timeout (ms)", 1),
};
inline constexpr Field udp_stateless_socket[] = {
    option("listen-address", "Listen address", text, "IP address to bind.", true),
    range("listen-port", "Listen port", 0, 65535, "0 selects an available port.", true),
    dns, fwmark, interface_name, source_ip,
    option("verbose", "Verbose logging", boolean), send_buffer, recv_buffer,
};

inline constexpr Field router_rule[] = {
    target("target", "Rule target", true),
    option("source-ips", "Source IP matches", text_or_list, "IP addresses, CIDR ranges or GeoIP tokens."),
    range("source-port", "Source port matches", 0, 65535, "One port or an array of ports.", false, port_list),
    range("source-port-range", "Source port range", 0, 65535, "Two ports in ascending order.", false, port_range),
    option("destination-ip", "Destination IP matches", text_or_list, "IP addresses, CIDR ranges or GeoIP tokens."),
    range("destination-port", "Destination port matches", 0, 65535, "One port or an array of ports.", false, port_list),
    range("destination-port-range", "Destination port range", 0, 65535, "Two ports in ascending order.", false, port_range),
    option("destination-domain", "Destination domains", text_or_list, "Exact domains, wildcards or GeoSite tokens."),
    option("network", "Network protocols", text_or_list, "tcp, udp, icmp or packet."),
    option("line-mark", "Line mark", text, "32 hexadecimal digits."),
    option("resolver-dns", "DNS-generated traffic", boolean),
    option("protocol", "Application protocols", text_or_list, "http1, tls or bittorrent."),
    option("attributes", "Sniffed attributes", string_list, "Supports http_upgrade_present."),
    option("username", "Usernames", text_or_list),
    option("password", "Passwords", text_or_list),
};
inline constexpr Field router_dns_policy[] = {
    dns_policy[0], dns_policy[1], dns_policy[2], dns_policy[3], dns_policy[4],
};
inline constexpr Field router[] = {
    group("rules", "Routing rules", router_rule, object_list, false, {}, "Evaluated in order before the default next node."),
    option("sniffing", "Sniffing modes", string_list, "full, http1, http2, http, tls, quic, http3 or bittorrent."),
    option("sniff-even-if-domain-is-already-provided", "Sniff known domains", boolean),
    group("dns-behaviour", "DNS behaviour", router_dns_policy),
    option("geoip-db-path", "GeoIP database path", text),
    option("geosite-db-path", "GeoSite database path", text),
};
inline constexpr Field sniff_rule[] = {
    Field{.key = "next", .label = "Route target", .kind = node_target, .required = true,
          .aliases = {"target"}, .string_alias_fallback = true},
    Field{.key = "domains", .label = "Domains", .kind = text_or_list,
          .hint = "Exact domains or wildcard domains.", .aliases = {"domain"}},
    option("detection", "Detection modes", text_or_list, "http1, tls, reverse, reverse-tls, reverse-handshake or an array of modes."),
    range("reverse-secret-length", "Reverse secret length", 1, 1024),
    option("reverse-secret", "Reverse secret", text),
};
inline constexpr Field sniff_router[] = {
    group("routes", "Routes", sniff_rule, object_list),
};
inline constexpr Field bridge[] = {target("pair", "Pair", true)};
inline constexpr Field half_duplex_client[] = {
    target("upload", "Upload"), target("download", "Download"),
};
inline constexpr Field half_duplex_server[] = {target("pair", "Pair")};
inline constexpr Field packet_split_stream[] = {target("up", "Up", true), target("down", "Down", true)};

} // namespace wave::node_settings::detail
