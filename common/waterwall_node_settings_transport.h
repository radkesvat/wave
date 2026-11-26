#pragma once

namespace wave::node_settings::detail {

inline constexpr const char* black_hole_modes[] = {
    "passive", "drop", "packet-drop", "silent", "calm", "active", "close", "aggressive", "kill", "kill-connection",
};
inline constexpr Field black_hole[] = {
    Field{.key = "mode", .label = "Mode", .kind = choice, .hint = "Passive by default.",
          .choices = black_hole_modes, .case_insensitive = true},
};
inline constexpr Field connection_fisher[] = {
    range("simultaneous-tries-perline", "Concurrent attempts per line", 1, int_max, "2 by default."),
};
inline constexpr Field stripe_common[] = {
    range("receive-window-bytes", "Receive window (bytes)", 16384, 16777216, "1048576 by default."),
    range("max-buffered-bytes", "Maximum buffered bytes", 65536, 67108864, "4194304 by default."),
    range("setup-timeout-ms", "Setup timeout (ms)", 1, 600000, "10000 by default."),
    range("drain-timeout-ms", "Drain timeout (ms)", 1, 600000, "10000 by default."),
    range("max-sessions", "Maximum sessions", 1, 65536, "1024 by default."),
};
inline constexpr Field stripe_client[] = {
    range("connections", "Transport connections per stream", 1, 32, "", true),
    stripe_common[0], stripe_common[1], stripe_common[2], stripe_common[3], stripe_common[4],
};
inline constexpr Field stripe_server[] = {
    range("max-connections", "Maximum transport connections per stream", 1, 32, "32 by default."),
    stripe_common[0], stripe_common[1], stripe_common[2], stripe_common[3], stripe_common[4],
    range("max-pending-connections", "Maximum pending connections", 1, 65536, "1024 by default."),
};

inline constexpr auto recv_timeout = range("no-recv-timeout-ms", "Receive timeout (ms)", 1, int_max, "30000 by default.");
inline constexpr auto close_timeout = range("close-drain-timeout-ms", "Close drain timeout (ms)", 1, int_max, "30000 by default.");
inline constexpr auto ping_interval = range("ping-interval-ms", "Ping interval (ms)", 1, int_max, "10000 by default.");
inline constexpr auto send_buf_limit = range("send-buffer-limit", "Send buffer limit (bytes)", 4096, 134217728, "524288 by default.");
inline constexpr auto recv_buf_limit = range("receive-buffer-limit", "Receive buffer limit (bytes)", 131072, 268435456, "1048576 by default.");
inline constexpr auto carrier_limit = range("carrier-queue-limit", "Carrier queue limit", 1, 65535, "256 by default.");
inline constexpr Field enet[] = {
    range("send-mtu", "Send MTU (bytes)", 604, 65535, "Uses the core MTU by default."),
    range("no-recv-timeout-ms", "Receive timeout (ms)", 1, 43200000, "30000 by default."),
    range("ping-interval-ms", "Ping interval (ms)", 1, 43200000, "10000 by default."),
    send_buf_limit, recv_buf_limit, carrier_limit, close_timeout,
    range("incoming-bandwidth", "Incoming bandwidth (bytes/sec)", 0, uint_max, "0 is unlimited."),
    range("outgoing-bandwidth", "Outgoing bandwidth (bytes/sec)", 0, uint_max, "0 is unlimited."),
};
inline constexpr Field gns[] = {
    range("send-mtu", "Send MTU (bytes)", 228, 65535, "Uses the core MTU by default."),
    range("send-rate", "Send rate (bytes/sec)", 1024, 104857600, "262144 by default."),
    range("timeout-initial-ms", "Initial timeout (ms)", 1, int_max, "10000 by default."),
    recv_timeout, send_buf_limit, recv_buf_limit, carrier_limit, close_timeout,
};
inline constexpr Field kcp[] = {
    range("send-mtu", "Send MTU (bytes)", 118, 65535, "Minimum 126 when FEC is enabled. Uses the core MTU by default."),
    option("fec", "Forward error correction", boolean),
    range("fec-data-shards", "FEC data shards", 1, 254, "10 by default."),
    range("fec-parity-shards", "FEC parity shards", 1, 254, "3 by default."),
    option("kcp-nodelay", "Low-delay mode", boolean, "Enabled by default."),
    option("kcp-no-congestion-control", "Disable congestion control", boolean, "Enabled by default."),
    range("kcp-initial-cwnd", "Initial congestion window", 1, int_max, "Half the send window by default."),
    range("kcp-interval-ms", "Update interval (ms)", 10, 5000, "10 by default."),
    range("kcp-resend", "Fast resend threshold", 0, int_max, "2 by default."),
    range("kcp-send-window", "Send window (segments)", 1, int_max, "8192 by default."),
    range("kcp-recv-window", "Receive window (segments)", 1, int_max, "8192 by default."),
    range("kcp-ack-batch", "ACK batch count", 0, int_max, "Up to the receive window. 1 by default."),
    range("kcp-rx-minrto-ms", "Minimum retransmission timeout (ms)", 1, int_max, "30 by default."),
    range("kcp-send-buffer-limit", "Send buffer limit (packets)", 0, int_max, "0 derives the limit from the windows."),
    ping_interval, recv_timeout, close_timeout,
};
inline constexpr Field keep_alive[] = {
    range("ping-interval", "Ping interval (ms)", 1, int_max, "30000 by default."),
    option("sensitive-mode", "Require ping replies", boolean),
    range("tolerance-ms", "Reply timeout (ms)", 1, int_max, "90000 by default."),
};

inline constexpr const char* mux_modes[] = {"timer", "counter", "fixed-connections-count"};
inline constexpr std::array mux_common = {
    range("max-children", "Maximum children per parent", 1),
    range("child-buffer-limit", "Child buffer limit (bytes)", 1),
    range("child-buffer-resume-threshold", "Child buffer resume threshold", 1),
    range("parent-buffer-limit", "Parent buffer limit (bytes)", 0),
    range("detached-buffer-limit", "Detached buffer limit (bytes)", 0),
    range("detached-child-limit", "Detached child limit", 0),
    range("parent-write-buffer-pause-threshold", "Parent write pause threshold", 1),
    range("parent-write-buffer-resume-threshold", "Parent write resume threshold", 0),
    range("parent-write-buffer-limit", "Parent write buffer limit", 1),
    option("keepalive", "Keepalive", boolean, "Enabled by default."),
    option("log-main-line-stats", "Log main line statistics", boolean),
};
template <std::size_t Left, std::size_t Right>
constexpr auto combine(const std::array<Field, Left>& left, const std::array<Field, Right>& right)
{
    std::array<Field, Left + Right> fields{};
    const auto remaining = std::ranges::copy(left, fields.begin()).out;
    std::ranges::copy(right, remaining);
    return fields;
}

inline constexpr auto mux_client = combine(std::array{
    select("mode", "Connection mode", mux_modes, "", true),
    range("connection-duration-ms", "Connection duration (ms)", 61, int_max, "Required in timer mode."),
    range("connection-capacity", "Connection capacity", 1, int_max, "Required in counter mode."),
    range("per-worker-connections-count", "Connections per worker", 1, int_max, "Required in fixed-connections-count mode."),
    range("ping-interval", "Ping interval (ms)", 1),
    range("tolerance-ms", "Reply timeout (ms)", 1),
}, mux_common);
inline constexpr auto mux_server = combine(mux_common, std::array{
    range("max-live-children", "Maximum live children", 0),
    range("memory-fallback-max-live-children", "Fallback maximum live children", 1),
    range("initial-child-idle-timeout-ms", "Initial child idle timeout (ms)", 1),
    range("active-child-idle-timeout-ms", "Active child idle timeout (ms)", 1),
    range("memory-high-watermark-percent", "Memory high watermark (%)", 1, 99),
    range("memory-low-watermark-percent", "Memory low watermark (%)", 1, 99),
    range("memory-reserve", "Memory reserve", 0),
});

inline constexpr const char* obfuscator_methods[] = {"xor"};
inline constexpr const char* obfuscator_skip[] = {"none", "ipv4", "transport"};
inline constexpr Field obfuscator[] = {
    select("method", "Method", obfuscator_methods, "", true),
    range("xor_key", "XOR key", 0, 255, "", true),
    select("skip", "Skip headers", obfuscator_skip, "None by default."),
    Field{.key = "tls_record_header", .label = "Use TLS record header", .kind = boolean, .aliases = {"tls_header"}},
};
inline constexpr const char* sequence_modes[] = {"random_node", "random_port"};
inline constexpr const char* acknowledgment_modes[] = {"random_node", "random_port", "random_packet"};
inline constexpr Field tcp_flags[] = {
    option("fin", "FIN", boolean), option("syn", "SYN", boolean), option("rst", "RST", boolean),
    option("psh", "PSH", boolean), option("ack", "ACK", boolean), option("urg", "URG", boolean),
    option("ece", "ECE", boolean), option("cwr", "CWR", boolean),
};
inline constexpr Field fake_tcp_header[] = {
    option("password", "Password", text),
    range("server-port", "Server port", 1, 65535),
    range("client-port", "Client port", 1, 65535),
    range("client-port-range", "Client port range", 1, 65535, "At most 1024 ports.", false, port_range),
    Field{.key = "sequence", .label = "Sequence", .kind = port,
          .minimum = 0, .maximum = uint_max, .choices = sequence_modes},
    Field{.key = "acknowledgment", .label = "Acknowledgment", .kind = port,
          .minimum = 0, .maximum = uint_max, .choices = acknowledgment_modes},
    option("increment-sequence", "Increment sequence", boolean),
    option("increment-acknowledgment", "Increment acknowledgment", boolean),
    group("flags", "TCP flags", tcp_flags, object_list, false, {}, "One to 32 flag combinations."),
    range("window", "Window", 0, 65535),
    range("urgent-pointer", "Urgent pointer", 0, 65535),
    option("options", "TCP options", text, "A hexadecimal string of up to 40 bytes in four-byte groups."),
};

inline constexpr const char* log_tunnel_modes[] = {"log", "file", "tcp-payload-file"};
inline constexpr const char* log_tunnel_levels[] = {"debug", "info", "warning", "error", "fatal"};
inline constexpr const char* log_tunnel_files[] = {"per-payload", "split-direction", "single-file"};
inline constexpr Field logger_tunnel[] = {
    select("mode", "Output", log_tunnel_modes, "", true),
    select("level", "Log level", log_tunnel_levels, "Debug by default in log mode."),
    select("output-mode", "File layout", log_tunnel_files, "Split by direction by default in file modes."),
};

inline constexpr const char* header_data[] = {
    "src_context->port", "line->src_ctx->port", "src_context->protocol", "line->src_ctx->protocol",
    "dest_context->protocol", "line->dest_ctx->protocol", "line-mark", "proxy-protocol", "proxy-protocol-v1", "proxy-protocol-v2",
};
inline constexpr const char* header_override[] = {
    "dest_context->port", "line->dest_ctx->port", "dest_context->protocol", "line->dest_ctx->protocol",
    "line-mark", "proxy-protocol->source-fields",
};
inline constexpr Field header_client[] = {
    Field{.key = "data", .label = "Header data", .kind = port, .required = true,
          .minimum = 0, .maximum = 65535, .choices = header_data},
    option("mark", "Line mark", text, "32 hexadecimal digits. Available when data is line-mark."),
    option("frontend-ipv4", "Frontend IPv4 address", text),
};
inline constexpr Field header_server[] = {
    Field{.key = "override", .label = "Override", .kind = port, .required = true,
          .minimum = 0, .maximum = 65535, .choices = header_override},
};

inline constexpr Field quic_common[] = {
    option("protocol", "ALPN protocol", text, "h3 by default."),
    range("handshake-timeout-ms", "Handshake timeout (ms)", 1, 86400000, "10000 by default."),
    range("idle-timeout-ms", "Idle timeout (ms)", 1, 86400000, "60000 by default."),
    range("close-drain-timeout-ms", "Close drain timeout (ms)", 1, 86400000, "30000 by default."),
    range("max-packet-size", "Maximum packet size", 1200, 1452, "1350 by default."),
    range("max-buffered-bytes", "Maximum buffered bytes", 65536, 67108864, "2097152 by default."),
    range("max-buffered-buffers", "Maximum buffered entries", 1, 65536, "1024 by default."),
    range("receive-window-bytes", "Receive window (bytes)", 16384, 16777216, "1048576 by default."),
    option("verbose", "Verbose logging", boolean),
};
inline constexpr Field quic_client[] = {
    option("sni", "Server name (SNI)", text, "", true),
    option("verify", "Verify server certificate", boolean, "Enabled by default."),
    option("ca-file", "CA certificate bundle", text),
    quic_common[0], quic_common[1], quic_common[2], quic_common[3], quic_common[4],
    quic_common[5], quic_common[6], quic_common[7], quic_common[8],
};
inline constexpr Field quic_server[] = {
    option("cert-file", "Certificate file", text, "", true),
    option("key-file", "Private key file", text, "", true),
    quic_common[0], quic_common[1], quic_common[2], quic_common[3], quic_common[4],
    quic_common[5], quic_common[6], quic_common[7], quic_common[8],
};
inline constexpr Field rtmp_server[] = {
    option("app", "Application name", text, "waterwall by default."),
    option("stream", "Stream name", text, "tunnel by default."),
    range("payload-block-size", "Payload block size", 1, 65536),
    range("handshake-timeout-ms", "Handshake timeout (ms)", 1),
    range("send-stall-timeout-ms", "Send stall timeout (ms)", 1),
};
inline constexpr Field rtmp_client[] = {
    option("tc-url", "RTMP application URL", text, "An rtmp:// or rtmps:// application URL.", true),
    rtmp_server[0], rtmp_server[1], rtmp_server[2], rtmp_server[3], rtmp_server[4],
};
inline constexpr auto reverse_length = range("reverse-secret-length", "Reverse secret length", 1, 1024, "640 by default.");
inline constexpr auto reverse_secret = option("reverse-secret", "Reverse secret", text);
inline constexpr Field reverse_client[] = {
    range("minimum-unused", "Minimum unused connections", 1),
    reverse_length, reverse_secret,
};
inline constexpr Field reverse_server[] = {
    range("activation-min-size", "Minimum activation size", 4, 65536, "64 by default."),
    range("activation-max-size", "Maximum activation size", 4, 65536, "1024 by default."),
    reverse_length, reverse_secret,
};
inline constexpr const char* limiter_identifiers[] = {"vless", "trojan"};
inline constexpr const char* limiter_failure_actions[] = {"passthrough", "pass", "pass-through", "close"};
inline constexpr Field soft_ip_limiter[] = {
    select("identifier", "User identity protocol", limiter_identifiers, "", true),
    select("on-identification-failure", "If identification fails", limiter_failure_actions, "Passthrough by default."),
    range("simultaneous-user-limit", "Simultaneous addresses per user", 1, 6, "", true),
    range("tolerance-ms", "Tolerance (ms)", 1, int_max, "", true),
    option("verbose", "Verbose logging", boolean),
};
inline constexpr const char* rate_limit_modes[] = {"per-connection", "per-line", "all-connections", "all-lines", "per-worker"};
inline constexpr const char* rate_work_modes[] = {"drop", "pause"};
inline constexpr Field speed_limit[] = {
    select("limit-mode", "Rate limit scope", rate_limit_modes, "", true),
    select("work-mode", "When exceeding the rate", rate_work_modes, "", true),
    range("bytes-per-sec", "Bytes per second", 0.5, 0x1p64 - 4096, "Enter exactly one rate.", false, number),
    range("kilo-bytes-per-sec", "KiB per second", 0.5 / 1024, 0x1p54 - 4, "", false, number),
    range("mega-bytes-per-sec", "MiB per second", 0.5 / 1048576, 0x1p44 - 1, "", false, number),
    range("token-recharge-rate", "Token recharge interval (ms)", 1),
};
inline constexpr const char* test_modes[] = {"tcp", "TCP", "udp", "UDP"};
inline constexpr const char* test_directions[] = {"upload", "UPLOAD", "send", "download", "DOWNLOAD", "receive", "bidirectional", "both", "BOTH"};
inline constexpr Field speed_test_client[] = {
    select("mode", "Transport", test_modes, "TCP by default."),
    select("direction", "Direction", test_directions, "Upload by default."),
    range("duration-ms", "Duration (ms)", 1),
    range("warmup-ms", "Warmup (ms)", 0),
    range("report-interval-ms", "Report interval (ms)", 1),
    range("start-delay-ms", "Start delay (ms)", 0),
    range("connection-count", "Connection count", 1),
    range("payload-size", "Payload size (bytes)", 1),
    range("timeout-ms", "Timeout (ms)", 1),
    range("target-bits-per-sec", "Target bits/sec", 0, 0x1p64 - 4096, "", false, number),
    range("udp-target-bits-per-sec", "UDP target bits/sec", 0, 0x1p64 - 4096, "", false, number),
    range("target-megabits-per-sec", "Target megabits/sec", 0, 0x1p64 / 1000000, "", false, number),
    option("json-summary", "Print JSON summary", boolean),
    option("verify-payload", "Verify payload", boolean),
    option("terminate-on-complete", "Terminate on completion", boolean, "Enabled by default."),
};
inline constexpr Field speed_test_server[] = {
    range("report-interval-ms", "Report interval (ms)", 1),
    option("json-summary", "Print JSON summary", boolean),
    option("quiet", "Quiet output", boolean),
};

inline constexpr const char* payload_modes[] = {"stream", "datagram", "dest_context->protocol"};
inline constexpr Field user_controller[] = {
    target("auth-client-node-name", "Authentication client", true),
    select("payload-mode", "Payload mode", payload_modes),
    range("sweep-interval-ms", "Sweep interval (ms)", 1),
    option("verbose", "Verbose logging", boolean),
};

inline constexpr Field api_permission[] = {
    option("config", "Config", text, "", true),
    option("node", "Node", text, "", true),
    option("operations", "Allowed operations", string_list, "List individual operation names.", true),
};
inline constexpr Field api_client[] = {
    option("name", "Client name", text, "", true),
    option("token", "Authentication token", text, "64 hexadecimal digits.", true),
    group("allow", "Permissions", api_permission, object_list, true),
};
inline constexpr double mib = 1024 * 1024;
inline constexpr Field api_server[] = {
    group("clients", "API clients", api_client, object_list, true),
    range("max_request_envelope", "Maximum request envelope (bytes)", 1, 64 * mib),
    range("max_response_envelope", "Maximum response envelope (bytes)", 1024, 64 * mib),
    range("max_input_bytes", "Maximum input (bytes)", 1, 256 * mib),
    range("max_line_memory", "Maximum line memory (bytes)", 1, 1024 * mib),
    range("max_input_fragments", "Maximum input fragments", 1, 65536),
    range("max_queued_requests", "Maximum queued requests", 1, 1024),
    range("max_sessions", "Maximum sessions", 1, 65536),
    range("max_jobs", "Maximum jobs", 1, 65536),
    range("handler_working_memory", "Handler memory (bytes)", 1, 1024 * mib),
    range("max_memory", "Maximum memory (bytes)", 1, 2048 * mib),
    range("max_unauthenticated_memory", "Maximum unauthenticated memory (bytes)", 1, 2048 * mib),
    range("frame_timeout_ms", "Frame timeout (ms)", 1, uint_max),
    range("request_timeout_ms", "Request timeout (ms)", 1, uint_max),
    range("output_timeout_ms", "Output timeout (ms)", 1, uint_max),
    range("idle_timeout_ms", "Idle timeout (ms)", 1, uint_max),
};

} // namespace wave::node_settings::detail
