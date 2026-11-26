#pragma once

#include <charconv>
#include <initializer_list>
#include <set>

// WaterWall v1.6.0 tunnel constructors and their configuration helpers.
namespace wave::node_settings::detail {
namespace security {

inline constexpr double uint32_max = 4294967295;
inline constexpr double uint64_max = 18446744073709551615.0;
inline constexpr double int64_max = 9223372036854775807.0;

constexpr Field text(const char* key, const char* label, const char* hint = "", bool required = false,
                     std::array<const char*, 2> aliases = {})
{
    return {.key = key,
            .label = label,
            .kind = Kind::text,
            .hint = hint,
            .required = required,
            .aliases = aliases};
}

constexpr Field bounded(const char* key, const char* label, double minimum, double maximum,
                        const char* hint = "", bool required = false, std::array<const char*, 2> aliases = {})
{
    return {.key = key,
            .label = label,
            .kind = Kind::integer,
            .hint = hint,
            .required = required,
            .minimum = minimum,
            .maximum = maximum,
            .aliases = aliases};
}

constexpr Field positive(const char* key, const char* label, const char* hint = "", bool required = false)
{
    return {
        .key = key, .label = label, .kind = Kind::integer, .hint = hint, .required = required, .minimum = 1};
}

constexpr Field milliseconds(const char* key, const char* label, const char* hint = "")
{
    return {.key = key, .label = label, .kind = Kind::integer, .hint = hint, .minimum = 0};
}

constexpr Field choice(const char* key, const char* label, std::span<const char* const> choices,
                       const char* hint = "", bool required = false, std::array<const char*, 2> aliases = {},
                       bool case_insensitive = false)
{
    return {.key = key,
            .label = label,
            .kind = Kind::choice,
            .hint = hint,
            .required = required,
            .choices = choices,
            .aliases = aliases,
            .case_insensitive = case_insensitive};
}

constexpr Field object(const char* key, const char* label, std::span<const Field> children,
                       const char* hint = "", bool required = false)
{
    return {.key = key,
            .label = label,
            .kind = Kind::object,
            .hint = hint,
            .required = required,
            .children = children};
}

constexpr Field objects(const char* key, const char* label, std::span<const Field> children,
                        const char* hint = "", bool required = false)
{
    return {.key = key,
            .label = label,
            .kind = Kind::object_list,
            .hint = hint,
            .required = required,
            .children = children};
}

inline constexpr const char* backup_periods[] = {"hourly", "daily", "weekly"};
inline constexpr const char* reality_algorithms[] = {"chacha20-poly1305", "aes-gcm", "chacha20poly1305",
                                                     "aes256gcm", "aes-256-gcm"};
inline constexpr const char* nonce_policies[] = {"auto", "sequence", "counter", "random"};
inline constexpr const char* protocols[] = {"tcp",     "udp",           "dest_context->protocol",
                                            "connect", "udp-associate", "line->dest_ctx->protocol"};
inline constexpr const char* dynamic_ports[] = {"dest_context->port", "line->dest_ctx->port"};
inline constexpr const char* proxy_dynamic_ports[] = {"dest_context->port"};
inline constexpr const char* proxy_modes[] = {"connect", "http"};
inline constexpr const char* http1_modes[] = {"single",      "split", "classic",        "legacy",
                                              "half-duplex", "dual",  "dual-connection"};
inline constexpr const char* http_proxy_methods[] = {"GET",   "HEAD",   "POST",   "PUT",
                                                     "PATCH", "DELETE", "OPTIONS"};
inline constexpr const char* body_modes[] = {"none", "fixed", "chunked"};
inline constexpr const char* placements[] = {"query",           "header",       "cookie",       "path",
                                             "query-parameter", "query-param",  "param",        "http-header",
                                             "cookies",         "path-segment", "path-template"};
inline constexpr const char* tls_versions[] = {"TLSv1", "TLSv1.1", "TLSv1.2", "TLSv1.3", "1.0",    "1.1",
                                               "1.2",   "1.3",     "TLS1.0",  "TLS1.1",  "TLS1.2", "TLS1.3"};
inline constexpr const char* fragment_modes[] = {"counter", "timed"};
inline constexpr const char* vmess_suites[] = {"aes-128-gcm", "chacha20-poly1305", "auto", "none", "zero"};
inline constexpr const char* counter_policies[] = {"wrap", "strict"};
inline constexpr const char* packet_encodings[] = {"none", "packetaddr"};
inline constexpr const char* wireguard_modes[] = {"packet", "connection"};
inline constexpr const char* payload_modes[] = {"stream", "datagram"};

inline constexpr Field named_auth{"auth-client-node-name", "Authentication client", Kind::node_target};
inline constexpr Field fallback{"fallback-node-name", "Fallback", Kind::node_target};
inline constexpr Field fallback_delay =
    milliseconds("fallback-intentional-delay-ms", "Fallback delay (ms)", "Default 7. 0 disables the delay.");
inline constexpr Field fallback_jitter =
    milliseconds("fallback-intentional-delay-jitter-ms", "Fallback jitter (ms)",
                 "Default 1. Used when fallback delay is nonzero.");
inline constexpr Field verbose{"verbose", "Verbose logging", Kind::boolean, "Default false."};
inline constexpr Field dns =
    object("dns-behaviour", "DNS policy", dns_policy, "Local target lookup is disabled when omitted.");
inline constexpr Field resolved_address = text(
    "target-address", "Target address", "IP, hostname, or destination context.", true, {"address", "target"});
inline constexpr Field resolved_port{
    "port",     "Target port",
    Kind::port, "Integer 1–65535 or dest_context->port / line->dest_ctx->port.",
    true,       1,
    65535,      dynamic_ports};
inline constexpr Field protocol =
    choice("protocol", "Target protocol", protocols, "Default TCP.", false, {"proto"}, true);
inline constexpr Field first_payload_timeout =
    bounded("first-payload-timeout-ms", "First payload timeout (ms)", 0, uint32_max, "Default 400.");
inline constexpr Field connect{"connect", "Allow TCP requests", Kind::boolean, "Default true."};
inline constexpr Field udp{"udp", "Allow UDP requests", Kind::boolean, "Default true."};
inline constexpr Field sweep = positive("sweep-interval-ms", "Account sweep interval (ms)", "Default 1000.");
inline constexpr Field sni = text("sni", "Server name (SNI)", "1–255 bytes.", true);
inline constexpr Field verify{"verify", "Verify server certificate", Kind::boolean, "Default true."};
inline constexpr Field ech_override =
    text("ech-sni-trick", "ECH GREASE SNI", "Optional SNI override, 1–255 bytes.");
inline constexpr Field ech_config =
    text("ech-config-list", "ECH configuration", "Base64 ECHConfigList. Excludes ECH GREASE SNI.");
inline constexpr Field hybrid_group{"x25519mlkem768", "Advertise hybrid key exchange", Kind::boolean,
                                    "Default true."};
inline constexpr Field tls_client_alpns{"alpns", "Offered ALPN protocols", Kind::string_list,
                                        "Default h2 and http/1.1. An empty array disables ALPN."};
inline constexpr Field uuid = text("uuid", "UUID", "RFC 4122 UUID.", true, {"id", "user-id"});
inline constexpr Field vmess_uuid =
    text("uuid", "UUID or short ID", "UUID or an exact UTF-8 ID of 1–30 bytes.", true, {"id", "user-id"});

inline constexpr Field auth_peer[] = {
    text("name", "Client credential name", "", true),
    text("secret", "Client shared secret", "", true),
    {"allow-stats-push", "Allow statistics reporting", Kind::boolean, "Default false."},
    {"allow-user-pull", "Allow user snapshots", Kind::boolean, "Default false."},
    {"allow-user-write", "Allow user updates", Kind::boolean, "Default false."},
    positive("session-idle-timeout-ms", "Session idle timeout (ms)", "Defaults to the server setting."),
};
inline constexpr Field auth_audit[] = {
    text("path", "Audit file path", "Path to the active audit log.", true),
    bounded("max-file-size", "Maximum file size (bytes)", 16384, 1073741824, "Default 16777216."),
    bounded("max-files", "Maximum files", 2, 32, "Default 7 (including the active file)."),
};

inline constexpr Field http_client_version{"http-version", "HTTP version", Kind::json,
                                           "1, 2, \"1.1\", \"2\", or \"both\". Default HTTP/2."};
inline constexpr Field http_server_version{"http-version", "HTTP version", Kind::json,
                                           "1, 2, \"1.1\", \"2\", or \"both\". Default both."};
inline constexpr Field http1_mode =
    choice("http1-mode", "HTTP/1.1 transport", http1_modes, "Default single.", false, {}, true);
inline constexpr Field http1_split{"http1-split", "Enable split transport (legacy)", Kind::boolean,
                                   "Used when HTTP/1.1 transport is omitted."};
inline constexpr Field http_path = text("path", "Request path", "Default /.");
inline constexpr Field http_method =
    text("method", "Request method", "HTTP method such as GET or POST. Default POST.");
inline constexpr Field content_type = text("content-type", "Content type");
inline constexpr Field http_headers{"headers", "HTTP headers", Kind::object,
                                    "Object of header names and string values."};
inline constexpr Field http_upgrade{"upgrade", "Enable upgrade", Kind::boolean,
                                    "Enabled by default when both versions are allowed."};
inline constexpr Field upgrade_protocol = text("upgrade-protocol", "Upgrade protocol");
inline constexpr Field upgrade_request{"upgrade-request-headers", "Upgrade request headers", Kind::object};
inline constexpr Field upgrade_response{"upgrade-response-headers", "Upgrade response headers", Kind::object};
inline constexpr Field websocket{"websocket", "WebSocket transport", Kind::boolean};
inline constexpr Field websocket_origin = text("websocket-origin", "WebSocket origin");
inline constexpr Field websocket_subprotocol = text("websocket-subprotocol", "WebSocket subprotocol");
inline constexpr Field http_close =
    bounded("close-drain-timeout-ms", "Close drain timeout (ms)", 1, 86400000);

inline constexpr Field split_side[] = {http_method, http_path, text("value", "Direction value"),
                                       http_headers};
inline constexpr Field split_fields[] = {
    object("upload", "Upload settings", split_side),
    object("download", "Download settings", split_side),
    text("upload-method", "Upload method"),
    text("download-method", "Download method"),
    text("upload-path", "Upload path"),
    text("download-path", "Download path"),
    {"upload-headers", "Upload headers", Kind::object},
    {"download-headers", "Download headers", Kind::object},
    choice("id-placement", "Session ID placement", placements, "Default query.", false, {}, true),
    text("id-name", "Session ID name", "Default wwid."),
    choice("direction-placement", "Direction placement", placements, "Default query.", false, {}, true),
    text("direction-name", "Direction name", "Default wwdir."),
    text("upload-value", "Upload direction value", "Default upload."),
    text("download-value", "Download direction value", "Default download."),
    {"cache-bypass", "Enable cache bypass", Kind::boolean, "Default true."},
    text("cache-bypass-name", "Cache bypass field", "Default wwcb."),
    text("token", "Shared token"),
    choice("token-placement", "Token placement", placements, "Default header.", false, {}, true),
    text("token-name", "Token name", "Default X-Waterwall-Token."),
};
inline constexpr Field split =
    object("split", "Split transport settings", split_fields, "Applies with HTTP/1.1 split transport.");

inline constexpr Field local_proxy_user[] = {text("username", "Username", "", true),
                                             text("password", "Password", "", true)};
inline constexpr Field max_proxy_header =
    bounded("max-header-bytes", "Maximum header size (bytes)", 1024, 65536, "Default 32768.");
inline constexpr Field proxy_header_timeout =
    positive("header-timeout-ms", "Header deadline (ms)", "Default 15000.");
inline constexpr Field proxy_idle_timeout =
    positive("idle-timeout-ms", "Idle timeout (ms)", "Default 300000.");

inline constexpr Field shaping_scope[] = {
    bounded("first-application-records", "First application records", 1, 1024, "", true)};
inline constexpr Field shaping_delay[] = {
    bounded("probability", "Delay probability (%)", 0, 100, "", true),
    {"ms", "Delay (ms)", Kind::json, "An integer from 0 to 1000 or a pair [minimum, maximum].", true},
};
inline constexpr Field shaping_outcome[] = {
    bounded("probability", "Outcome probability (%)", 1, 100, "", true),
    {"padding-bytes", "Padding (bytes)", Kind::json,
     "An integer from 1 to 4096 or a pair [minimum, maximum]."},
    object("delay", "Delay", shaping_delay),
};
inline constexpr Field shaping[] = {
    object("scope", "Record scope", shaping_scope, "", true),
    objects("outcomes", "Outcomes", shaping_outcome,
            "1–16 outcomes. Their probabilities must sum to 100 or less.", true),
};
inline constexpr Field tls_shaping = object("tls13-record-shaping", "TLS 1.3 record shaping", shaping);
inline constexpr Field fragment[] = {
    choice("mode", "Fragmentation scope", fragment_modes, "", true),
    bounded("count", "Payload count", 0, uint32_max, "Required in counter mode."),
    bounded("duration-ms", "Duration (ms)", 0, uint32_max, "Required in timed mode."),
    bounded("bypass_chance", "Bypass chance (%)", 0, 100, "Default 0."),
    {"cuts", "Cuts", Kind::array,
     "Up to 64 [offset, delay_ms, chance_percent] entries. Offsets must increase.", true},
    {"wait-for-est", "Wait for transport establishment", Kind::boolean, "Default true."},
    {"tls-hello-fragment", "Split the TLS ClientHello", Kind::boolean, "Default false."},
    bounded("tls-hello-timeout-ms", "TLS ClientHello timeout (ms)", 1, uint32_max,
            "Requires TLS ClientHello splitting. Default 1000."),
};
inline constexpr Field reality_password = text("password", "Shared password", "1–32 bytes.", true);
inline constexpr Field reality_algorithm = choice("algorithm", "Encryption algorithm", reality_algorithms,
                                                  "Default chacha20-poly1305.", false, {"method"}, true);
inline constexpr Field reality_salt =
    text("salt", "Key derivation salt", "1–32 bytes. Default waterwall-reality.");
inline constexpr Field kdf_iterations =
    bounded("kdf-iterations", "Key derivation iterations", 1, 1000000, "Default 12000.");
inline constexpr Field wireguard_key = text("privatekey", "Local private key", "32 bytes in Base64.", true);
inline constexpr Field wireguard_public = text("publickey", "Remote public key", "32 bytes in Base64.", true);
inline constexpr Field wireguard_ranges =
    text("allowedips", "Allowed IP ranges", "Up to 16 comma-separated CIDRs.", true);
inline constexpr Field wireguard_psk = text("presharedkey", "Preshared key", "32 bytes in Base64.");
inline constexpr Field wireguard_keepalive =
    bounded("persistentkeepalive", "Persistent keepalive (seconds)", 0, 65535, "Default 0 (disabled).");
inline constexpr Field wireguard_client_peer[] = {
    wireguard_public,
    wireguard_ranges,
    wireguard_psk,
    wireguard_keepalive,
    text("endpoint", "Server endpoint", "host:port or [IPv6]:port.", true),
};
inline constexpr Field wireguard_server_peer[] = {
    wireguard_public,
    wireguard_ranges,
    wireguard_psk,
    wireguard_keepalive,
};

} // namespace security

inline constexpr Field authentication_client[] = {
    security::text("name", "Client credential name", "Matches a credential entry on AuthenticationServer.",
                   true),
    security::text("secret", "Client shared secret", "", true),
    security::milliseconds("ping-interval-ms", "Ping interval (ms)", "Default 60000."),
    security::milliseconds("pull-interval-ms", "User snapshot interval (ms)",
                           "Default 300000. 0 disables periodic pulls."),
    security::milliseconds("push-interval-ms", "Usage report interval (ms)",
                           "Default 300000. 0 disables periodic reports."),
    security::milliseconds("reconnect-interval-ms", "Reconnect interval (ms)", "Default 5000."),
    security::milliseconds("request-timeout-ms", "Request timeout (ms)",
                           "Default 120000. 0 disables the timeout."),
    security::positive("max-pending-requests", "Maximum pending requests", "Default 64."),
    security::bounded("sync-max-users", "Maximum synchronized users", 1, security::uint64_max,
                      "Default 100000."),
    security::bounded("sync-max-bytes", "Maximum synchronized bytes", 1, security::uint64_max,
                      "Default 268435456."),
    {"traffic-multiplier", "Traffic multiplier", Kind::number, "At most three decimal places. Default 1.",
     false, 0.001, 1000},
    security::verbose,
};

inline constexpr Field authentication_server[] = {
    security::text("db-path", "Users database path", "Wave assigns this path when saving."),
    security::positive("file-save-rate-ms", "File save interval (ms)", "Wave uses 10000 when omitted."),
    security::positive("session-idle-timeout-ms", "Session idle timeout (ms)", "Default 600000."),
    security::bounded("snapshot-max-bytes", "Maximum snapshot bytes", 1, security::int64_max,
                      "Default 268435456."),
    security::positive("snapshot-idle-timeout-ms", "Snapshot idle timeout (ms)", "Default 60000."),
    security::choice("normal-backups", "Backup interval", security::backup_periods,
                     "Requires a backup directory."),
    security::text("normal-backups-path", "Backup directory", "Requires a backup interval."),
    security::positive("normal-backups-count-limit", "Maximum backup files",
                       "Requires normal backups. Default 10."),
    security::objects("auth-clients", "Authentication client credentials", security::auth_peer,
                      "Requires at least one credential with a unique name.", true),
    security::object("audit-log", "Administrative audit log", security::auth_audit),
    security::verbose,
};

inline constexpr Field encryption[] = {
    security::text("password", "Shared password",
                   "At most 32 bytes in stream mode when derivation iterations exceed 1.", true),
    {"packet-mode", "Packet mode", Kind::boolean, "Default false (stream mode)."},
    {.key = "algorithm",
     .label = "Encryption algorithm",
     .kind = Kind::json,
     .hint = "Use \"chacha20-poly1305\" (2) or \"aes-gcm\" (3) for streams and \"xchacha20-poly1305\" (4) "
             "for packets.",
     .aliases = {"method"}},
    security::text("salt", "Key derivation salt", "At most 32 bytes. Default waterwall-encryption."),
    security::kdf_iterations,
    security::bounded("max-frame-size", "Maximum plaintext frame (bytes)", 1, 16356,
                      "Stream mode only. Default 16356."),
};

inline constexpr Field http_client[] = {
    security::text("host", "HTTP host", "", true),
    security::http_client_version,
    security::http_path,
    security::text("scheme", "URI scheme", "Default https."),
    security::bounded("port", "Host port", 1, 65535, "Defaults to 443 for https and 80 otherwise."),
    security::http_method,
    security::text("user-agent", "User agent", "Default WaterWall/1.x."),
    security::content_type,
    security::http_headers,
    security::http_upgrade,
    security::upgrade_protocol,
    security::upgrade_request,
    security::upgrade_response,
    security::websocket,
    security::websocket_origin,
    security::websocket_subprotocol,
    security::text("websocket-extensions", "WebSocket extensions"),
    security::http1_mode,
    security::http1_split,
    security::split,
    security::http_close,
    {"stay-alive", "Keep client line alive", Kind::boolean},
    security::verbose,
};

inline constexpr Field http_server[] = {
    security::http_server_version,
    security::text("host", "Expected host"),
    security::http_path,
    security::http_method,
    security::bounded("status", "Response status", 100, 599, "Default 200."),
    security::content_type,
    security::http_headers,
    {"no-split-upload-buffering-limit", "Disable split upload buffer limit", Kind::boolean},
    security::http_upgrade,
    security::upgrade_protocol,
    security::upgrade_request,
    security::upgrade_response,
    security::websocket,
    security::websocket_origin,
    security::websocket_subprotocol,
    security::http1_mode,
    security::http1_split,
    security::split,
    security::http_close,
    security::verbose,
};

inline constexpr Field http_proxy_client[] = {
    security::choice("mode", "HTTP proxy mode", security::proxy_modes, "Default connect."),
    security::text("target-address", "Origin address", "IP, hostname, or dest_context->address. 1–255 bytes.",
                   true),
    {"port", "Origin port", Kind::port, "Integer 1–65535 or dest_context->port.", true, 1, 65535,
     security::proxy_dynamic_ports},
    security::text("username", "Proxy username", "Supply with a password."),
    security::text("password", "Proxy password", "Supply with a username."),
    security::dns,
    security::http_headers,
    security::choice("method", "Request method", security::http_proxy_methods,
                     "HTTP mode only. Default POST."),
    security::text("path", "Origin path", "HTTP mode only. Default /."),
    security::choice("body-mode", "Request body mode", security::body_modes,
                     "HTTP mode only. Default chunked. HEAD requires none."),
    security::bounded("content-length", "Fixed content length", 0, 9007199254740991.,
                      "Required only for fixed body mode."),
    security::max_proxy_header,
    security::proxy_header_timeout,
    security::positive("connect-response-timeout-ms", "CONNECT response timeout (ms)",
                       "CONNECT mode only. Default 30000."),
    security::proxy_idle_timeout,
    security::verbose,
};

inline constexpr Field http_proxy_server[] = {
    {"no-auth", "Allow unauthenticated access", Kind::boolean,
     "Exclusive with local users and AuthenticationClient."},
    security::objects("users", "Local proxy credentials", security::local_proxy_user,
                      "At least one entry when using local credentials."),
    security::named_auth,
    security::fallback,
    security::sweep,
    security::max_proxy_header,
    security::bounded("max-pending-bytes", "Maximum pending bytes", 65536, 1048576, "Default 262144."),
    security::proxy_header_timeout,
    security::positive("connect-timeout-ms", "Connect timeout (ms)", "Default 30000."),
    security::proxy_idle_timeout,
    security::verbose,
};

inline constexpr Field reality_client[] = {
    security::sni,
    security::verify,
    security::tls_client_alpns,
    security::ech_override,
    security::ech_config,
    security::hybrid_group,
    security::object("fragment", "Output fragmentation", security::fragment),
    security::tls_shaping,
    security::verbose,
    security::reality_password,
    security::reality_algorithm,
    security::reality_salt,
    security::kdf_iterations,
};

inline constexpr Field reality_server[] = {
    {"destination", "Destination", Kind::node_target, "Node for visitor traffic.", true},
    security::reality_password,
    security::reality_algorithm,
    security::reality_salt,
    security::kdf_iterations,
    security::bounded("sniffing-attempts", "Sniffing attempts", 1, 1024, "Default 8.", false,
                      {"sniffing-counter"}),
    security::choice("tls12-gcm-server-nonce-policy", "TLS 1.2 GCM nonce policy", security::nonce_policies,
                     "Default auto.", false, {}, true),
};

inline constexpr Field socks5_client[] = {
    security::resolved_address,
    security::resolved_port,
    security::protocol,
    security::dns,
    security::verbose,
    security::text("user", "Proxy username", "Supply with a password. 1–255 bytes.", false, {"username"}),
    security::text("pass", "Proxy password", "Supply with a username. 1–255 bytes.", false, {"password"}),
};

inline constexpr Field socks5_server[] = {
    security::named_auth,
    {"no-auth", "Allow unauthenticated access", Kind::boolean, "Exclusive with AuthenticationClient."},
    security::connect,
    {"udp", "Allow UDP requests", Kind::boolean, "Default false."},
    security::text("ipv4", "Advertised UDP IPv4 address", "Required when UDP requests are enabled.", false,
                   {"udp-ipv4"}),
    security::sweep,
    security::verbose,
};

inline constexpr Field tls_client[] = {
    security::sni,
    security::tls_client_alpns,
    security::verify,
    security::hybrid_group,
    security::ech_config,
    security::ech_override,
    security::object("fragment", "Output fragmentation", security::fragment),
    security::tls_shaping,
    security::verbose,
};

inline constexpr Field tls_server[] = {
    security::text("cert-file", "Certificate file", "", true),
    security::text("key-file", "Private key file", "", true),
    security::text("sni", "Expected server name (SNI)"),
    {"select-alpns", "Selected ALPN protocols", Kind::array,
     "Protocol strings or objects with a value string. Default h2 and http/1.1."},
    {"alpns", "Selected ALPN protocols (legacy)", Kind::array, "Prefer select-alpns."},
    security::choice("min-version", "Minimum TLS version", security::tls_versions, "Default TLSv1.2.", false,
                     {}, true),
    security::choice("max-version", "Maximum TLS version", security::tls_versions, "Default TLSv1.3.", false,
                     {}, true),
    security::text("ciphers", "OpenSSL cipher list", "Default HIGH:!aNULL:!MD5."),
    {"prefer-server-ciphers", "Prefer server ciphers", Kind::boolean, "Default false."},
    security::milliseconds("session-timeout", "Session lifetime (seconds)", "Default 300."),
    security::milliseconds("handshake-timeout-ms", "Handshake timeout (ms)", "Default 60000. 0 disables it."),
    security::text("session-cache", "Session cache", "none, off, builtin, or builtin:<size>. Default none."),
    security::milliseconds("session-cache-size", "Session cache size",
                           "Used with builtin cache. Default 20480."),
    {"session-tickets", "Enable session tickets", Kind::boolean, "Default true."},
    security::fallback,
    security::fallback_delay,
    security::fallback_jitter,
    security::tls_shaping,
    security::verbose,
};

inline constexpr Field trojan_client[] = {
    security::text("password", "Password", "Choose a password or a SHA-224 digest.", false, {"pass"}),
    security::text("sha224", "SHA-224 password digest", "56 hexadecimal characters.", false,
                   {"password-sha224", "password_sha224"}),
    security::resolved_address,
    security::resolved_port,
    security::protocol,
    security::first_payload_timeout,
    security::dns,
    security::verbose,
};

inline constexpr Field trojan_server[] = {
    security::text("password", "Local password", "Use local credentials or an AuthenticationClient.", false,
                   {"pass"}),
    {"passwords", "Local passwords", Kind::array,
     "Password strings or objects with password/pass and an optional username."},
    {.key = "users",
     .label = "Local users",
     .kind = Kind::array,
     .hint = "Password strings or objects with password/pass and an optional username.",
     .aliases = {"clients"}},
    security::named_auth,
    security::fallback,
    security::fallback_delay,
    security::fallback_jitter,
    security::connect,
    security::udp,
    security::sweep,
    security::verbose,
};

inline constexpr Field vless_client[] = {
    security::uuid,     security::resolved_address,      security::resolved_port,
    security::protocol, security::first_payload_timeout, security::dns,
    security::verbose,
};

inline constexpr Field vless_server[] = {
    security::text("uuid", "Local UUID", "RFC 4122 UUID.", false, {"id"}),
    {"uuids", "Local UUIDs", Kind::array, "UUID strings or user objects."},
    {.key = "users",
     .label = "Local users",
     .kind = Kind::array,
     .hint = "UUID strings or objects with UUID/id/user-id and an optional username.",
     .aliases = {"clients"}},
    security::named_auth,
    security::fallback,
    security::fallback_delay,
    security::fallback_jitter,
    security::connect,
    security::udp,
    security::sweep,
    security::verbose,
};

inline constexpr Field vmess_client[] = {
    security::vmess_uuid,
    security::resolved_address,
    security::resolved_port,
    security::protocol,
    security::first_payload_timeout,
    security::dns,
    security::choice("security", "Payload security", security::vmess_suites, "Default aes-128-gcm.", false,
                     {}, true),
    {"chunk-masking", "Mask chunk lengths", Kind::boolean, "Defaults to true except in zero mode."},
    {"global-padding", "Pad chunks", Kind::boolean, "Defaults to true in encrypted modes."},
    {"authenticated-length", "Authenticate lengths", Kind::boolean, "Encrypted modes only. Default false."},
    security::choice("encrypted-counter-policy", "Encrypted counter policy", security::counter_policies,
                     "Default wrap."),
    {"send-termination", "Send termination frame", Kind::boolean, "Default true."},
    security::choice("packet-encoding", "UDP packet encoding", security::packet_encodings, "Default none."),
    security::verbose,
};

inline constexpr Field vmess_server[] = {
    security::text("uuid", "Local UUID or short ID", "UUID or an exact UTF-8 ID of 1–30 bytes.", false,
                   {"id", "user-id"}),
    {"uuids", "Local IDs", Kind::array, "UUID/short-ID strings or user objects."},
    {.key = "users",
     .label = "Local users",
     .kind = Kind::array,
     .hint = "UUID/short-ID strings or objects with UUID/id/user-id and an optional username.",
     .aliases = {"clients"}},
    security::named_auth,
    security::connect,
    security::udp,
    security::positive("handshake-timeout-ms", "Handshake deadline (ms)", "Default 10000."),
    security::positive("auth-max-pending", "Maximum pending authentication jobs", "Default 4096."),
    security::positive("auth-pending-max-bytes", "Pending authentication budget (bytes)",
                       "Default 67108864."),
    security::positive("auth-cache-max-bytes", "Authentication cache budget (bytes)", "Default 67108864."),
    security::positive("replay-max-entries", "Maximum replay entries", "Default 262144."),
    security::fallback,
    security::fallback_delay,
    security::fallback_jitter,
    security::choice("encrypted-counter-policy", "Encrypted counter policy", security::counter_policies,
                     "Default wrap."),
    {"allowed-security", "Allowed payload security", Kind::string_list,
     "Nonempty, unique list from aes-128-gcm, chacha20-poly1305, none, zero."},
    {"send-termination", "Send termination frame", Kind::boolean, "Default true."},
    {"packetaddr", "Accept PacketAddr", Kind::boolean, "Default true."},
    security::bounded("packetaddr-max-destinations", "Maximum PacketAddr destinations", 1, 1024,
                      "Default 64."),
    security::verbose,
};

inline constexpr Field wireguard_client[] = {
    security::wireguard_key,
    security::objects("peers", "Server peer", security::wireguard_client_peer, "Exactly one peer.", true),
};

inline constexpr Field wireguard_server[] = {
    security::choice("mode", "WireGuard mode", security::wireguard_modes, "", true),
    security::wireguard_key,
    security::objects("peers", "Static peers", security::wireguard_server_peer,
                      "Required in packet mode. 1–32 peers."),
    security::named_auth,
    security::bounded("udp-idle-timeout-ms", "UDP idle timeout (ms)", 1, security::uint32_max,
                      "Connection mode only. Default 300000."),
    security::bounded("mtu", "Packet-to-connection MTU", 68, 65535, "Connection mode only."),
    security::bounded("max-pending-bytes", "Packet-to-connection pending bytes", 1024, 67108864,
                      "Connection mode only. Default 2097152."),
    {.key = "fake-dns",
     .label = "Fake DNS",
     .kind = Kind::json,
     .hint = "Connection mode only. Boolean or fake DNS settings object.",
     .aliases = {"mapdns", "fake_dns"}},
    security::choice("payload-mode", "User controller payload mode", security::payload_modes,
                     "Connection mode only."),
    security::sweep,
    security::verbose,
};

namespace security {

inline const nlohmann::json& value(const nlohmann::json& settings, const char* key)
{
    static const nlohmann::json missing;
    const auto found = settings.find(key);
    return found == settings.end() ? missing : *found;
}

inline bool has_any(const nlohmann::json& settings, std::initializer_list<const char*> keys)
{
    return std::ranges::any_of(keys, [&](const auto* key) { return settings.contains(key); });
}

inline std::string lowercase(const nlohmann::json& setting)
{
    if (!setting.is_string()) {
        return {};
    }
    auto result = setting.get<std::string>();
    std::ranges::transform(result, result.begin(), lower_ascii);
    return result;
}

inline bool any_of(std::string_view value, std::initializer_list<std::string_view> values)
{
    return std::ranges::find(values, value) != values.end();
}

inline bool fits(const nlohmann::json& setting, std::size_t maximum)
{
    return setting.is_string() && !setting.get_ref<const std::string&>().empty() &&
           setting.get_ref<const std::string&>().size() <= maximum;
}

inline std::string validate_encryption(const nlohmann::json& settings)
{
    const bool packet = value(settings, "packet-mode") == true;
    const auto* algorithm_key = field_key(settings, encryption[2]);
    const auto& algorithm = value(settings, algorithm_key);
    if (settings.contains(algorithm_key)) {
        const auto name = lowercase(algorithm);
        const bool compatible =
            packet ? algorithm == 4 || name == "xchacha20-poly1305"
                   : algorithm == 2 || algorithm == 3 ||
                         any_of(name, {"chacha20-poly1305", "chacha20poly1305", "chacha20", "chacha",
                                       "aes-gcm", "aes256gcm", "aes-256-gcm", "aes256-gcm"});
        if (!compatible) {
            return packet ? "Packet encryption requires XChaCha20-Poly1305."
                          : "Stream encryption requires ChaCha20-Poly1305 or AES-GCM.";
        }
    }
    if (packet && settings.contains("max-frame-size")) {
        return "max-frame-size applies only to stream mode.";
    }
    if (!packet && value(settings, "kdf-iterations") != 1 &&
        value(settings, "password").get_ref<const std::string&>().size() > 32) {
        return "The shared password must be at most 32 bytes when stream mode uses multiple derivation "
               "iterations.";
    }
    if (const auto& salt = value(settings, "salt");
        salt.is_string() && salt.get_ref<const std::string&>().size() > 32) {
        return "salt must contain at most 32 bytes.";
    }
    return {};
}

inline std::string validate_http_framing(const nlohmann::json& settings)
{
    const auto& version = value(settings, "http-version");
    const auto name = lowercase(version);
    const bool http1 = version == 1 || any_of(name, {"1", "1.1", "http1", "http1.1"});
    const bool http2 = version == 2 || any_of(name, {"2", "2.0", "http2", "h2"});
    if (settings.contains("http-version") && !http1 && !http2 &&
        !any_of(name, {"both", "any", "auto", "1.1+2"})) {
        return "http-version must select HTTP/1.1, HTTP/2, or both.";
    }
    const auto transport = lowercase(value(settings, "http1-mode"));
    const bool split_mode = any_of(transport, {"split", "half-duplex", "dual", "dual-connection"}) ||
                            (!settings.contains("http1-mode") && value(settings, "http1-split") == true);
    if (split_mode && (!http1 || value(settings, "websocket") == true)) {
        return "Split transport requires HTTP/1.1 and excludes WebSocket transport.";
    }
    return {};
}

inline std::string validate_http_proxy_client(const nlohmann::json& settings)
{
    if (!fits(value(settings, "target-address"), 255)) {
        return "Origin address must contain 1–255 bytes.";
    }
    if (settings.contains("username") != settings.contains("password")) {
        return "Proxy username and password must be supplied together.";
    }
    if (value(settings, "mode") != "http") {
        return has_any(settings, {"method", "path", "body-mode", "content-length"})
                   ? "Method, path, body mode, and content length require HTTP mode."
                   : "";
    }
    if (value(value(settings, "dns-behaviour"), "resolve") == true ||
        settings.contains("connect-response-timeout-ms")) {
        return "Local DNS resolution and CONNECT response timeout require CONNECT mode.";
    }
    if ((value(settings, "body-mode") == "fixed") != settings.contains("content-length")) {
        return "Content length is required only for fixed body mode.";
    }
    if (value(settings, "method") == "HEAD" && value(settings, "body-mode") != "none") {
        return "The HEAD method requires body mode none.";
    }
    return {};
}

inline std::string validate_proxy_auth(std::string_view type, const nlohmann::json& settings)
{
    const bool auth_client = settings.contains("auth-client-node-name");
    if (type == "Socks5Server" || type == "HttpProxyServer") {
        const bool no_auth = value(settings, "no-auth") == true;
        const bool users = type == "HttpProxyServer" && settings.contains("users");
        if (static_cast<int>(no_auth) + auth_client + users != 1) {
            return type == "Socks5Server"
                       ? "Choose an AuthenticationClient or explicitly enable unauthenticated access."
                       : "Choose local users, an AuthenticationClient, or explicitly enable unauthenticated "
                         "access.";
        }
        if (users && value(settings, "users").empty()) {
            return "Enter at least one local user.";
        }
        if (type == "Socks5Server" && value(settings, "udp") == true &&
            !has_any(settings, {"ipv4", "udp-ipv4"})) {
            return "An advertised IPv4 address is required when UDP is enabled.";
        }
        return {};
    }

    const bool trojan = type == "TrojanServer";
    const char* single = trojan ? "password" : "uuid";
    const char* alias = trojan ? "pass" : "id";
    const char* multiple = trojan ? "passwords" : "uuids";
    const bool legacy_user_id = type == "VmessServer" && settings.contains("user-id");
    if (static_cast<int>(settings.contains(single)) + settings.contains(alias) + legacy_user_id > 1 ||
        (settings.contains("users") && settings.contains("clients"))) {
        return "Provide only one spelling for each local credential setting.";
    }
    const bool local = legacy_user_id || has_any(settings, {single, alias, multiple, "users", "clients"});
    if (auth_client == local) {
        return "Choose local credentials or an AuthenticationClient.";
    }
    for (const auto* key : {multiple, "users", "clients"}) {
        if (settings.contains(key) && value(settings, key).empty()) {
            return std::string(key) + " must contain at least one credential.";
        }
    }
    return {};
}

inline std::string validate_vmess_server(const nlohmann::json& settings)
{
    if (!settings.contains("allowed-security")) {
        return {};
    }
    const auto& suites = value(settings, "allowed-security");
    if (suites.empty() || suites.size() > 4) {
        return "Choose one to four allowed payload security modes.";
    }
    for (auto suite = suites.begin(); suite != suites.end(); ++suite) {
        if (!any_of(suite->get_ref<const std::string&>(),
                    {"aes-128-gcm", "chacha20-poly1305", "none", "zero"}) ||
            std::find(suites.begin(), suite, *suite) != suite) {
            return "Choose unique values from aes-128-gcm, chacha20-poly1305, none, and zero.";
        }
    }
    if (value(settings, "connect") == false && suites.size() == 1 && suites[0] == "zero") {
        return "UDP requests require a framed payload security mode.";
    }
    return {};
}

inline int tls_version(const nlohmann::json& setting, int fallback)
{
    const auto name = lowercase(setting);
    if (name.empty()) {
        return fallback;
    }
    return name == "tlsv1" ? 0 : name.back() - '0';
}

inline bool scalar_or_pair(const nlohmann::json& setting, int minimum, int maximum)
{
    const auto in_range = [&](const auto& item) {
        return item.is_number_integer() && item >= minimum && item <= maximum;
    };
    return in_range(setting) || (setting.is_array() && setting.size() == 2 && in_range(setting[0]) &&
                                 in_range(setting[1]) && setting[0] <= setting[1]);
}

inline std::string validate_tls_shaping(const nlohmann::json& shaping, bool takeover)
{
    const auto& outcomes = value(shaping, "outcomes");
    if (outcomes.empty() || outcomes.size() > 16) {
        return "TLS record shaping requires 1–16 outcomes.";
    }
    int probability = 0;
    for (const auto& outcome : outcomes) {
        const bool padding = outcome.contains("padding-bytes");
        if (padding && !scalar_or_pair(value(outcome, "padding-bytes"), 1, 4096)) {
            return "TLS padding must be an integer from 1 to 4096 or an ordered pair in that range.";
        }
        const auto& delay = value(outcome, "delay");
        const auto& ms = value(delay, "ms");
        if (!delay.is_null() && !scalar_or_pair(ms, 0, 1000)) {
            return "TLS delay must be an integer from 0 to 1000 or an ordered pair in that range.";
        }
        const bool delays = value(delay, "probability") > 0 && (ms.is_array() ? ms[1] > 0 : ms > 0);
        if (!padding && !delays) {
            return "Each TLS record shaping outcome must apply padding or a delay.";
        }
        if (takeover && delays) {
            return "RealityClient record shaping supports padding only.";
        }
        probability += value(outcome, "probability").get<int>();
    }
    return probability <= 100 ? "" : "TLS record shaping probabilities must sum to 100 or less.";
}

inline std::string validate_tls(std::string_view type, const nlohmann::json& settings)
{
    if (const auto& shaping = value(settings, "tls13-record-shaping"); shaping.is_object()) {
        if (auto issue = validate_tls_shaping(shaping, type == "RealityClient"); !issue.empty()) {
            return issue;
        }
    }
    if (type == "TlsServer") {
        if (!value(settings, "alpns").empty() && !value(settings, "select-alpns").empty()) {
            return "Choose alpns or select-alpns.";
        }
        for (const auto* key : {"alpns", "select-alpns"}) {
            for (const auto& entry : value(settings, key)) {
                if (!fits(entry.is_object() ? value(entry, "value") : entry, 255)) {
                    return "Each selected ALPN protocol must contain 1–255 bytes.";
                }
            }
        }
        if (tls_version(value(settings, "min-version"), 2) > tls_version(value(settings, "max-version"), 3)) {
            return "The minimum TLS version must be at most the maximum TLS version.";
        }
        if (settings.contains("session-cache")) {
            const auto cache = lowercase(value(settings, "session-cache"));
            if (!any_of(cache, {"none", "off", "builtin"})) {
                int size = 0;
                if (!cache.starts_with("builtin:")) {
                    return "Session cache must be none, off, builtin, or builtin:<size>.";
                }
                const auto parsed = std::from_chars(cache.data() + 8, cache.data() + cache.size(), size);
                if (parsed.ec != std::errc{} || parsed.ptr != cache.data() + cache.size() || size < 0) {
                    return "Builtin session cache size must be a nonnegative integer.";
                }
            }
        }
        return {};
    }

    if (!fits(value(settings, "sni"), 255)) {
        return "sni must contain 1–255 bytes.";
    }
    std::set<std::string_view> alpn_names;
    std::size_t alpn_wire_bytes = 0;
    for (const auto& entry : value(settings, "alpns")) {
        if (!fits(entry, 255)) {
            return "Each offered ALPN protocol must contain 1–255 bytes.";
        }
        const auto& name = entry.get_ref<const std::string&>();
        if (!alpn_names.insert(name).second) {
            return "Offered ALPN protocols must be unique.";
        }
        alpn_wire_bytes += 1 + name.size();
    }
    if (alpn_wire_bytes > 65533) {
        return "The encoded ALPN offer must fit within 65533 bytes.";
    }
    if (settings.contains("ech-sni-trick") && !fits(value(settings, "ech-sni-trick"), 255)) {
        return "ECH GREASE SNI must contain 1–255 bytes.";
    }
    if (settings.contains("ech-sni-trick") && settings.contains("ech-config-list")) {
        return "Choose an ECH configuration or an ECH GREASE SNI.";
    }
    if (const auto& fragment = value(settings, "fragment"); fragment.is_object()) {
        const bool timed = value(fragment, "mode") == "timed";
        if (!fragment.contains(timed ? "duration-ms" : "count") ||
            fragment.contains(timed ? "count" : "duration-ms")) {
            return "Fragmentation in timed mode requires duration. Counter mode requires count.";
        }
        if (fragment.contains("tls-hello-timeout-ms") && value(fragment, "tls-hello-fragment") != true) {
            return "The TLS ClientHello timeout requires TLS ClientHello splitting.";
        }
    }
    return {};
}

inline bool wireguard_key_value(const nlohmann::json& setting)
{
    if (!setting.is_string()) {
        return false;
    }
    const std::string_view key = setting.get_ref<const std::string&>();
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return key.size() == 44 && key.back() == '=' && alphabet.find(key[42]) % 4 == 0 &&
           key.substr(0, 42).find_first_not_of(alphabet) == key.npos;
}

inline std::string validate_wireguard(std::string_view type, const nlohmann::json& settings)
{
    if (!wireguard_key_value(value(settings, "privatekey"))) {
        return "The WireGuard private key must be a canonical Base64 string encoding 32 bytes.";
    }
    const auto& peers = value(settings, "peers");
    for (const auto& peer : peers) {
        if (!wireguard_key_value(value(peer, "publickey")) ||
            (peer.contains("presharedkey") && !wireguard_key_value(value(peer, "presharedkey")))) {
            return "WireGuard peer keys must be canonical Base64 strings encoding 32 bytes.";
        }
    }
    if (type == "WireGuardClient") {
        return peers.size() == 1 ? "" : "WireGuardClient requires exactly one peer.";
    }
    if (value(settings, "mode") == "connection") {
        if (!settings.contains("auth-client-node-name") || has_any(settings, {"peers", "presharedkey"})) {
            return "Connection mode requires an AuthenticationClient and excludes static peers.";
        }
        return {};
    }
    if (settings.contains("auth-client-node-name") || peers.empty() || peers.size() > 32) {
        return "Packet mode requires 1–32 static peers and excludes an AuthenticationClient.";
    }
    if (std::ranges::any_of(peers, [](const auto& peer) { return peer.contains("endpoint"); })) {
        return "Static server peers learn their endpoints from incoming traffic.";
    }
    return {};
}

} // namespace security

inline std::string validate_security_settings(std::string_view type, const nlohmann::json& settings)
{
    using security::any_of;
    using security::has_any;
    using security::value;

    if (type == "AuthenticationClient") {
        if (const auto& multiplier = value(settings, "traffic-multiplier"); multiplier.is_number()) {
            const double rate = multiplier.get<double>();
            if (std::round(rate * 1000) / 1000 != rate) {
                return "Traffic multiplier supports up to three decimal places.";
            }
        }
    } else if (type == "AuthenticationServer") {
        if (settings.contains("normal-backups") != settings.contains("normal-backups-path") ||
            (settings.contains("normal-backups-count-limit") && !settings.contains("normal-backups"))) {
            return "Supply a backup interval and directory together. Backup count requires both.";
        }
        const auto& clients = value(settings, "auth-clients");
        if (clients.empty()) {
            return "Enter at least one AuthenticationClient credential.";
        }
        std::set<std::string_view> names;
        for (const auto& client : clients) {
            if (!names.insert(client["name"].get_ref<const std::string&>()).second) {
                return "AuthenticationClient credential names must be unique.";
            }
        }
    } else if (any_of(type, {"EncryptionClient", "EncryptionServer"})) {
        return security::validate_encryption(settings);
    } else if (any_of(type, {"HttpClient", "HttpServer"})) {
        return security::validate_http_framing(settings);
    } else if (type == "HttpProxyClient") {
        return security::validate_http_proxy_client(settings);
    } else if (type == "Socks5Client") {
        if (has_any(settings, {"user", "username"}) != has_any(settings, {"pass", "password"})) {
            return "Proxy username and password must be supplied together.";
        }
    } else if (type == "TrojanClient") {
        if (has_any(settings, {"password", "pass"}) ==
            has_any(settings, {"sha224", "password-sha224", "password_sha224"})) {
            return "Supply either a password or a SHA-224 digest.";
        }
        const auto& digest = value(settings, field_key(settings, trojan_client[1]));
        if (digest.is_string()) {
            const auto& hex = digest.get_ref<const std::string&>();
            if (hex.size() != 56 || hex.find_first_not_of("0123456789abcdefABCDEF") != hex.npos) {
                return "The SHA-224 digest must contain exactly 56 hexadecimal characters.";
            }
        } else if (value(settings, field_key(settings, trojan_client[0])) == "") {
            return "The password must contain at least one byte.";
        }
    } else if (type == "VmessClient") {
        const auto protocol = security::lowercase(value(settings, field_key(settings, security::protocol)));
        const auto security_mode = security::lowercase(value(settings, "security"));
        if (value(settings, "packet-encoding") == "packetaddr" &&
            (protocol.empty() || any_of(protocol, {"tcp", "connect"}))) {
            return "PacketAddr requires a UDP-capable protocol.";
        }
        if (security_mode == "zero" &&
            (any_of(protocol, {"udp", "udp-associate"}) || value(settings, "global-padding") == true ||
             value(settings, "chunk-masking") == true || value(settings, "authenticated-length") == true)) {
            return "Zero security mode requires unframed TCP.";
        }
        if (security_mode == "none" && value(settings, "authenticated-length") == true) {
            return "Authenticated length requires encrypted security.";
        }
    } else if (any_of(type,
                      {"HttpProxyServer", "Socks5Server", "TrojanServer", "VlessServer", "VmessServer"})) {
        if ((type == "Socks5Server" ? value(settings, "udp") != true : value(settings, "udp") == false) &&
            type != "HttpProxyServer" && value(settings, "connect") == false) {
            return "Enable at least one of TCP and UDP.";
        }
        if (type == "VmessServer") {
            if (auto issue = security::validate_vmess_server(settings); !issue.empty()) {
                return issue;
            }
        }
        return security::validate_proxy_auth(type, settings);
    } else if (any_of(type, {"RealityClient", "RealityServer"})) {
        const auto& password = value(settings, "password").get_ref<const std::string&>();
        const auto& salt = value(settings, "salt");
        if (password.size() > 32 ||
            (salt.is_string() && (salt == "" || salt.get_ref<const std::string&>().size() > 32))) {
            return "Reality passwords and explicitly configured salts must contain 1–32 bytes.";
        }
        if (settings.contains("max-frame-size")) {
            return "WaterWall 1.6.0 Reality rejects the obsolete max-frame-size field.";
        }
        if (type == "RealityClient") {
            return security::validate_tls(type, settings);
        }
    } else if (any_of(type, {"TlsClient", "TlsServer"})) {
        return security::validate_tls(type, settings);
    } else if (any_of(type, {"WireGuardClient", "WireGuardServer"})) {
        return security::validate_wireguard(type, settings);
    }
    return {};
}

} // namespace wave::node_settings::detail
