#pragma once

namespace wave::node_settings::detail {

inline constexpr const char* context_port[] = {"dest_context->port"};
inline constexpr Field connection_to_packets[] = {
    option("source-ipv4", "Virtual source IPv4", text, "A routable IPv4 unicast address.", true),
    option("target-address", "Target address", text, "IPv4, hostname or dest_context->address."),
    Field{.key = "port", .label = "Target port", .kind = port, .minimum = 1, .maximum = 65535, .choices = context_port},
    range("mtu", "MTU (bytes)", 576, 9000),
    dns,
    range("tcp-connect-timeout-ms", "Connect timeout (ms)", 1, uint_max),
    range("max-pending-bytes", "Pending bytes limit", 1024, 67108864),
};
inline constexpr Field fake_dns_fields[] = {
    option("enabled", "Enabled", boolean),
    option("address", "Listener IPv4 address", text, "198.18.0.2 by default."),
    option("network", "Synthetic network", text, "100.64.0.0 by default."),
    option("netmask", "Synthetic netmask", text, "255.192.0.0 by default."),
    range("port", "Port", 1, 65535, "53 by default."),
    range("ttl", "TTL", 0),
    range("cache-size", "Cache size", 1, 262144),
};
inline constexpr Field packets_to_connection[] = {
    range("mtu", "MTU (bytes)", 68, 65535),
    range("udp-idle-timeout-ms", "UDP idle timeout (ms)", 1, uint_max),
    range("max-pending-bytes", "Pending bytes limit", 1024, 67108864),
    Field{.key = "fake-dns", .label = "Fake DNS", .kind = json,
          .hint = "Use a boolean or an object with enabled, address, network, netmask, port, ttl and cache-size.",
          .aliases = {"fake_dns", "mapdns"}},
};

inline constexpr const char* validation_levels[] = {"none", "loose", "hard"};
inline constexpr const char* packet_protocols[] = {"both", "tcp", "udp"};
inline constexpr auto validation_level = select("packet-validation-level", "Packet validation", validation_levels);
inline constexpr auto sensitive_mode = option("sensitive-mode", "Sensitive mode", boolean);
inline constexpr Field stream_to_packets[] = {validation_level, sensitive_mode};
inline constexpr Field packets_to_stream[] = {
    select("protocol", "Protocol", packet_protocols, "Both TCP and UDP by default."),
    validation_level, sensitive_mode,
    range("interval-ms", "Heartbeat interval (ms)", 1, int_max, "50 by default."),
    range("tolerance-ms", "Heartbeat timeout (ms)", 1),
};

inline constexpr const char* ping_identifiers[] = {"random"};
inline constexpr Field ping[] = {
    Field{.key = "identifier", .label = "Identifier", .kind = port, .hint = "Random by default.",
          .minimum = 0, .maximum = 65535, .choices = ping_identifiers},
    range("sequence-start", "Initial sequence", 0, 65535),
    option("send-replies", "Send replies", boolean),
};
inline constexpr const char* ping6_modes[] = {"direct", "ipv6-in-ipv4"};
inline constexpr Field ping6[] = {
    ping[0], ping[1], ping[2],
    select("mode", "Framing mode", ping6_modes, "Direct by default."),
    option("local-ipv6", "Local IPv6 address", text, "Required in ipv6-in-ipv4 mode."),
    option("peer-ipv6", "Peer IPv6 address", text, "Required in ipv6-in-ipv4 mode."),
};

inline constexpr const char* capture_modes[] = {"source-ip"};
inline constexpr const char* fragment_policies[] = {"reassemble", "preserve-fragments"};
inline constexpr const char* opaque_flow_modes[] = {"prefix", "peer", "spread", "auto"};
inline constexpr auto fragment_policy = select("fragment-policy", "Fragment policy", fragment_policies, "Reassemble by default.");
inline constexpr auto opaque_flow_mode = select("opaque-flow-mode", "Opaque flow mode", opaque_flow_modes, "Prefix by default.");
inline constexpr Field raw_socket[] = {
    option("raw-device-name", "Raw device name", text),
    option("capture-device-name", "Capture device name", text),
    Field{.key = "capture-ips", .label = "IPv4 capture ranges", .kind = text_or_list,
          .aliases = {"listen-ips", "capture-ip"}},
    select("capture-filter-mode", "Capture filter", capture_modes, "Required when capture ranges are configured."),
    option("capture-protocols", "Allowed IP protocol numbers", array, "Integer entries from 0 to 255."),
    option("dont-capture-protocols", "Excluded IP protocol numbers", array, "Integer entries from 0 to 255."),
    option("skip-sysctl", "Skip sysctl changes", boolean),
    option("multi-queue", "Multiple queues", boolean),
    option("kernel-queue-tune", "Tune kernel queues", boolean),
    range("kernel-queue-packets", "Kernel queue limit (packets)", 1),
    range("kernel-receive-buffer", "Kernel receive buffer (bytes)", 1),
    range("mark", "Firewall mark", 1, uint_max),
    fragment_policy, opaque_flow_mode,
};
inline constexpr Field tun_device[] = {
    option("device-name", "Device name", text, "", true),
    option("device-ip", "Device IPv4 and prefix", text, "", true),
    range("device-mtu", "Device MTU (bytes)", 68, 65535),
    option("gso", "Generic segmentation offload", boolean),
    option("multi-queue", "Multiple queues", boolean),
    option("kernel-queue-tune", "Tune kernel queues", boolean),
    range("kernel-queue-length", "Kernel queue length", 1),
    fragment_policy, opaque_flow_mode,
    option("system-route", "Enable the system route", boolean),
    option("route-table", "Route table", text, "Use off, main, auto or a platform route-table name."),
    option("loop-protection", "Route loop protection", boolean),
    option("route-cidrs", "Routed IPv4 prefixes", string_list),
    option("route-exclude-cidrs", "Excluded route prefixes", string_list),
    nullable(option("dns", "Device DNS servers", string_list, "List one or two IPv4 addresses.")),
};
inline constexpr Field syn_reply[] = {
    option("reply-source-ip", "Reply source IPv4 address", text),
    range("ttl", "TTL", 1, 255),
    option("fake-checksum", "Use fake checksum", boolean),
};

inline constexpr const char* fragment_modes[] = {"counter", "timed"};
inline constexpr Field stream_fragmenter[] = {
    select("mode", "Mode", fragment_modes, "", true),
    range("count", "Payload count", 0, uint_max, "Required in counter mode."),
    range("duration-ms", "Duration (ms)", 0, uint_max, "Required in timed mode."),
    option("cuts", "Fragment cuts", array, "Up to 64 [offset, delay_ms, chance_percent] entries, with increasing offsets.", true),
    range("bypass_chance", "Bypass chance (%)", 0, 100),
    option("wait-for-est", "Wait for connection established", boolean, "Enabled by default."),
    option("tls-hello-fragment", "Fragment TLS hello", boolean),
    range("tls-hello-timeout-ms", "TLS hello timeout (ms)", 1, uint_max, "1000 by default when enabled."),
};

inline constexpr Field ip_override_rule[] = {
    option("ipv4", "IPv4 replacement", text_or_list),
    option("ipv6", "IPv6 replacement", text),
};
inline constexpr Field ip_override_direction[] = {
    group("source-ip", "Source address rule", ip_override_rule),
    group("dest-ip", "Destination address rule", ip_override_rule),
};
inline constexpr const char* ip_directions[] = {"up", "down"};
inline constexpr const char* ip_modes[] = {"source-ip", "dest-ip"};
inline constexpr Field ip_overrider[] = {
    group("up", "Upstream rules", ip_override_direction),
    group("down", "Downstream rules", ip_override_direction),
    select("direction", "Legacy direction", ip_directions),
    select("mode", "Legacy address mode", ip_modes),
    ip_override_rule[0], ip_override_rule[1],
    range("chance", "Chance (%)", 0, 100),
    option("only120", "Preserve final eight IPv6 bits", boolean),
};

inline constexpr const char* tcp_bit_actions[] = {
    "off", "on", "toggle", "packet->cwr", "packet->ece", "packet->urg", "packet->ack",
    "packet->psh", "packet->rst", "packet->syn", "packet->fin", "flip", "switch",
};
inline constexpr Field ip_manipulator[] = {
    range("mtu", "MTU (bytes)", 68, 65535),
    range("stateful-flow-limit", "Stateful flow limit", 32, 1048576),
    Field{.key = "protoswap-tcp", .label = "TCP replacement protocol number", .kind = integer,
          .minimum = 0, .maximum = 255, .aliases = {"protoswap"}},
    range("protoswap-udp", "UDP replacement protocol number", 0, 255),
    option("sni-blender", "Enable SNI blender", boolean),
    range("sni-blender-packets", "SNI blender packet count", 2, 16),
    range("packet-duplicate", "Packet duplication count", 1, 16),
    Field{.key = "preserve-tcp-bitflags", .label = "Preserve TCP flags", .kind = boolean,
          .aliases = {"carry-original-tcp-flags", "bit-transport"}},
    option("source-port-ghost", "Source port ghost", boolean),
    option("dest-port-ghost", "Destination port ghost", boolean),
    option("first-sni", "First SNI", text),
    range("first-sni-count", "First SNI count", 1, 16),
    range("first-sni-replay-delay", "First SNI replay delay (ms)", 0),
    range("first-sni-final-delay", "First SNI final delay (ms)", 0),
    range("first-sni-ttl", "First SNI TTL", 0, 255),
    option("first-sni-random-tcp-sequence", "Random first SNI TCP sequence", boolean),
    option("smuggle-sni", "Smuggle SNI", text),
    target("real-sni-upstream-node", "SNI upstream"),
    range("smuggle-sni-delay-ms", "Smuggle SNI delay (ms)", 0),
    option("overlap-sni", "Overlap SNI", text),
    range("overlap-sni-hold-timeout-ms", "Overlap SNI hold timeout (ms)", 1),
    range("overlap-sni-delay-ms", "Overlap SNI delay (ms)", 0),
    range("overlap-sni-syn-ttl", "Overlap SNI SYN TTL", 1, 255),
    option("synfin-sni", "SYN FIN SNI", text),
    range("synfin-sni-additional-range-min", "Minimum SYN FIN SNI range", 0),
    range("synfin-sni-additional-range-max", "Maximum SYN FIN SNI range", 0),
    range("synfin-sni-hold-timeout-ms", "SYN FIN SNI hold timeout (ms)", 1),
    range("synfin-sni-syn-ttl", "SYN FIN SNI SYN TTL", 0, 255),
    range("synfin-sni-fin-ttl", "SYN FIN SNI FIN TTL", 0, 255),
    range("synfin-sni-fake-ttl", "SYN FIN SNI fake TTL", 0, 255),
    option("synfin-sni-random-syn-checksum", "Random SYN checksum", boolean),
    option("synfin-sni-random-fin-checksum", "Random FIN checksum", boolean),
    option("synfin-sni-random-syn-sequence", "Random SYN sequence", boolean),
    option("synfin-sni-random-fin-sequence", "Random FIN sequence", boolean),
    option("synfin-sni-use-rst", "Use RST", boolean),
    option("ech-sni-trick", "ECH SNI", text),
    range("data-shard-1-delay", "First data shard delay (ms)", 0),
    range("data-shard-2-delay", "Second data shard delay (ms)", 0),
    option("smuggle-fin", "Smuggle FIN", boolean),
    target("real-fin-upstream-node", "FIN upstream"),
    range("fin-sni-delay-ms", "FIN SNI delay (ms)", 0),
    range("fin-pause-timeout-ms", "FIN pause timeout (ms)", 0),
    select("up-tcp-bit-ack", "Upstream ACK flag", tcp_bit_actions),
    select("up-tcp-bit-cwr", "Upstream CWR flag", tcp_bit_actions),
    select("up-tcp-bit-ece", "Upstream ECE flag", tcp_bit_actions),
    select("up-tcp-bit-fin", "Upstream FIN flag", tcp_bit_actions),
    select("up-tcp-bit-psh", "Upstream PSH flag", tcp_bit_actions),
    select("up-tcp-bit-rst", "Upstream RST flag", tcp_bit_actions),
    select("up-tcp-bit-syn", "Upstream SYN flag", tcp_bit_actions),
    select("up-tcp-bit-urg", "Upstream URG flag", tcp_bit_actions),
    select("dw-tcp-bit-ack", "Downstream ACK flag", tcp_bit_actions),
    select("dw-tcp-bit-cwr", "Downstream CWR flag", tcp_bit_actions),
    select("dw-tcp-bit-ece", "Downstream ECE flag", tcp_bit_actions),
    select("dw-tcp-bit-fin", "Downstream FIN flag", tcp_bit_actions),
    select("dw-tcp-bit-psh", "Downstream PSH flag", tcp_bit_actions),
    select("dw-tcp-bit-rst", "Downstream RST flag", tcp_bit_actions),
    select("dw-tcp-bit-syn", "Downstream SYN flag", tcp_bit_actions),
    select("dw-tcp-bit-urg", "Downstream URG flag", tcp_bit_actions),
};

inline constexpr const char* packet_generator_source_ports[] = {"random", "RANDOM"};
inline constexpr const char* packet_generator_protocols[] = {"UDP", "TCP", "ICMP", "ALL", "udp", "tcp", "icmp", "all"};
inline constexpr Field packet_generator[] = {
    Field{.key = "source-ipv4-range", .label = "Source IPv4 range", .kind = text_or_list,
          .hint = "One IPv4 CIDR or a list of CIDR ranges.", .required = true, .aliases = {"source-ip4-range"}},
    option("dest-ipv4", "Destination IPv4 address", text, "", true),
    select("protocol-number", "Protocol", packet_generator_protocols, "", true),
    range("packets-per-ip", "Packets per IP", 1),
    range("duration-ms", "Duration (ms)", 1, int_max, "", true),
    range("dest-port", "Destination port", 0, 65535, "Required for TCP, UDP and ALL."),
    Field{.key = "src-port", .label = "Source port", .kind = port, .hint = "Required for TCP, UDP and ALL.",
          .minimum = 0, .maximum = 65535, .choices = packet_generator_source_ports},
};
inline constexpr Field packet_counter[] = {
    packet_generator[0],
    option("protocol-number", "Protocol", json, "TCP, UDP, ICMP, ALL, an IP protocol number or an array of protocols.", true),
    range("expected-packets-per-ip", "Expected packets per IP", 1),
    range("report-after-ms", "Report after (ms)", 1),
    option("output-file", "Output file", text, "packet-counter-report.txt by default."),
};

inline constexpr Field disturber[] = {
    option("disturb-upstream", "Disturb upstream", boolean, "Enabled by default."),
    option("disturb-downstream", "Disturb downstream", boolean),
    range("chance_instant_close", "Immediate close chance", 0, 100),
    range("chance_middle_close", "Midstream close chance", 0, 100),
    range("chance_payload_corruption", "Payload corruption chance", 0, 100),
    range("chance_payload_loss", "Payload loss chance", 0, 100),
    range("chance_payload_duplication", "Payload duplication chance", 0, 100),
    range("chance_payload_out_of_order", "Out-of-order payload chance", 0, 100),
    range("chance_payload_delay", "Delayed payload chance", 0, 100),
    range("chance_connection_deadhang", "Connection hang chance", 0, 100),
    range("delay_min_ms", "Minimum delay (ms)", 0),
    range("delay_max_ms", "Maximum delay (ms)", 0),
};

inline constexpr Field junk_datagram_sender[] = {
    option("selected-protocols", "Selected protocols", string_list, "Protocol names or all."),
    range("packet-count-perline-min", "Minimum packets per line", 0),
    range("packet-count-perline-max", "Maximum packets per line", 0),
    range("keep-sending-max-ms", "Maximum send duration (ms)", 0),
    range("resend-again-times", "Resend count", 0),
};

inline constexpr const char* packet_tester_transport[] = {"tcp", "udp", "icmp", "raw", "none"};
inline constexpr const char* tester_transport[] = {"tcp", "udp"};
inline constexpr Field tester_ipv4[] = {
    option("source-ip", "Source IPv4 address", text, "", true),
    option("dest-ip", "Destination IPv4 address", text, "", true),
    select("transport", "Transport", packet_tester_transport),
    range("protocol", "IP protocol number", 0, 255),
    range("ttl", "TTL", 1, 255),
    option("worker-affine-flow", "Keep flow on one worker", boolean),
};
inline constexpr Field tester_destination[] = {
    option("address", "Destination address", text),
    range("port", "Destination port", 1, 65535),
    select("protocol", "Transport", tester_transport),
};
inline constexpr Field tester_common[] = {
    option("packet-mode", "Packet mode", boolean),
    option("packet-stateless", "Stateless packets", boolean),
    group("packet-ipv4", "IPv4 packet settings", tester_ipv4),
    range("chunk-count", "Chunk count", 1, 11, "11 by default."),
    range("max-payload-size", "Maximum payload size", 0),
    range("split-payload-delay-ms", "Split payload delay (ms)", 0),
    range("split-payload-burst", "Split payload burst", 1),
};
inline constexpr Field tester_client[] = {
    tester_common[0], tester_common[1], tester_common[2], tester_common[3], tester_common[4],
    tester_common[5], tester_common[6],
    group("dest-context", "Destination context", tester_destination),
    option("allow-early-response", "Allow early response", boolean),
    option("packet-start-immediately", "Start packets immediately", boolean),
    range("packet-start-delay-ms", "Packet start delay (ms)", 0),
};
inline constexpr Field tester_server[] = {
    tester_common[0], tester_common[1], tester_common[2], tester_common[3], tester_common[4],
    tester_common[5], tester_common[6],
    option("packet-init-on-start", "Initialize packets on start", boolean),
    option("streaming-response", "Stream responses", boolean),
};

} // namespace wave::node_settings::detail
