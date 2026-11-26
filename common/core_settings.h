#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace wave::core_settings {

// WaterWall v1.6.0: core/core_settings.c and core/readme.md.
inline constexpr std::size_t max_bytes = 16 * 1024;

enum class Kind {
    integer,
    boolean,
    text,
    log_level,
    ram_profile,
    domain_strategy,
    lookups,
    domains,
    flags
};

struct Field {
    const char* key;
    const char* label;
    const char* hint;
    Kind kind;
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    bool allow_zero = false;
};

inline bool valid_integer(const Field& field, std::int64_t number)
{
    return (number >= field.minimum && number <= field.maximum) || (field.allow_zero && number == 0);
}

struct Choice {
    const char* value;
    const char* label;
};

inline constexpr auto max_int = std::numeric_limits<std::int32_t>::max();
inline constexpr auto max_count = std::numeric_limits<std::uint32_t>::max();

inline constexpr Field runtime_fields[] = {
    {"workers", "Worker threads", "0 uses the CPU count. WaterWall caps the count at 254.", Kind::integer,
     std::numeric_limits<std::int32_t>::min(), max_int},
    {"ram-profile", "Memory profile", "Server by default", Kind::ram_profile},
    {"mtu", "Default MTU (bytes)", "1500 by default", Kind::integer, 68, 65535},
    {"libs-path", "Tunnel library directory", "libs/", Kind::text},
    {"splice", "TCP splice", "Enabled by default on eligible streams", Kind::boolean},
    {"tcp-tune", "TCP memory tuning", "Enabled by default when splice is enabled", Kind::boolean},
    {"try-enabling-bbr", "Enable TCP BBR", "Attempted by default on Linux", Kind::boolean},
};

inline constexpr Field stack_fields[] = {
    {"tcp-stack-receive-budget", "TCP receive budget (bytes)", "33554432 by default; 0 disables it",
     Kind::integer, 65536, 1073741824, true},
    {"lwip-memory-budget", "Stack memory budget (bytes)", "536870912 by default", Kind::integer, 1048576,
     std::numeric_limits<std::int64_t>::max()},
    {"lwip-max-tcp-flows", "Maximum TCP flows", "65536 by default", Kind::integer, 1, max_count},
    {"lwip-max-udp-flows", "Maximum UDP flows", "65536 by default", Kind::integer, 1, max_count},
    {"lwip-max-tcp-listeners", "Maximum TCP listeners", "1024 by default", Kind::integer, 1, max_count},
    {"lwip-max-tcp-segments", "Maximum TCP segments", "1048576 by default", Kind::integer, 1, max_count},
    {"lwip-max-rx-wrappers", "Maximum receive wrappers", "262144 by default", Kind::integer, 1, max_count},
};

inline constexpr Field log_path{"path", "Log directory", "log/", Kind::text};
inline constexpr Field logger_fields[] = {
    {"loglevel", "Log level", "Info by default", Kind::log_level},
    {"file", "File name", "", Kind::text},
    {"console", "Console output", "Enabled by default", Kind::boolean},
};
inline constexpr Choice loggers[] = {
    {"core", "Core"},
    {"network", "Network"},
    {"dns", "DNS"},
    {"internal", "Internal"},
};

inline constexpr Field dns_fields[] = {
    {"domain-strategy", "Address preference", "Prefer IPv4 by default", Kind::domain_strategy},
    {"servers", "DNS servers", "System resolvers by default. Separate servers with commas.", Kind::text},
    {"domains", "Search domains", "System search domains by default", Kind::domains},
    {"lookups", "Lookup order", "System lookup order by default", Kind::lookups},
};

inline constexpr Field dns_advanced_fields[] = {
    {"timeout-ms", "Initial timeout (ms)", "1000 by default", Kind::integer, 1, max_int},
    {"max-timeout-ms", "Maximum timeout (ms)", "5000 by default", Kind::integer, 1, max_int},
    {"tries", "Attempts per server", "5 by default", Kind::integer, 1, max_int},
    {"query-cache-max-ttl", "Cache TTL limit (seconds)", "1800 by default; 0 disables it", Kind::integer, 0,
     max_int},
    {"ndots", "Minimum dots", "System resolver default", Kind::integer, 0, 15},
    {"udp-port", "UDP port", "Resolver default", Kind::integer, 1, 65535},
    {"tcp-port", "TCP port", "Resolver default", Kind::integer, 1, 65535},
    {"socket-send-buffer-size", "Send buffer (bytes)", "System socket default", Kind::integer, 1, max_int},
    {"socket-receive-buffer-size", "Receive buffer (bytes)", "System socket default", Kind::integer, 1,
     max_int},
    {"edns-packet-size", "EDNS packet size (bytes)", "Resolver default", Kind::integer, 1, 65535},
    {"udp-max-queries", "Queries per UDP socket", "Resolver default", Kind::integer, 0, max_int},
    {"rotate", "Rotate DNS servers", "System resolver default", Kind::boolean},
    {"resolvconf-path", "Resolver configuration path", "/etc/resolv.conf", Kind::text},
    {"hosts-path", "Hosts file path", "/etc/hosts", Kind::text},
    {"sortlist", "Address sort list", "c-ares sortlist syntax", Kind::text},
    {"flags", "Resolver flags", "EDNS and connection reuse by default", Kind::flags},
};

inline constexpr Field failover_fields[] = {
    {"retry-chance", "Retry chance", "10 by default", Kind::integer, 0, 65535},
    {"retry-delay-ms", "Retry delay (ms)", "5000 by default", Kind::integer, 0, max_int},
};

inline constexpr Choice log_levels[] = {
    {"VERBOSE", "Verbose"}, {"DEBUG", "Debug"}, {"INFO", "Info"},     {"WARN", "Warning"},
    {"ERROR", "Error"},     {"FATAL", "Fatal"}, {"SILENT", "Silent"},
};

inline constexpr Choice ram_profiles[] = {
    {"server", "Server (L2)"},   {"client", "Client (M1)"}, {"client-larger", "Larger client (M2)"},
    {"minimal", "Minimal (S1)"}, {"2", "Small (S2)"},       {"5", "Large (L1)"},
};

inline constexpr Choice domain_strategies[] = {
    {"accept-dns-returned-order", "Resolver order"},
    {"prefer-ipv4", "Prefer IPv4"},
    {"prefer-ipv6", "Prefer IPv6"},
    {"only-ipv4", "IPv4 only"},
    {"only-ipv6", "IPv6 only"},
};

inline constexpr Choice lookup_orders[] = {
    {"bf", "DNS, then hosts file"},
    {"fb", "Hosts file, then DNS"},
    {"b", "DNS only"},
    {"f", "Hosts file only"},
};

// Array indices are the c-ares 1.34.8 flag bits used by WaterWall v1.6.0.
inline constexpr Choice dns_flags[] = {
    {"use-vc", "Use TCP"},
    {"primary", "Primary server only"},
    {"ignore-truncated", "Ignore truncation"},
    {"no-recurse", "Disable recursion"},
    {"stayopen", "Reuse connections"},
    {"no-search", "Disable search domains"},
    {"no-aliases", "Disable host aliases"},
    {"no-check-response", "Disable response error checks"},
    {"edns", "Enable EDNS"},
    {"no-default-server", "Disable fallback server"},
    {"dns0x20", "Randomize query letter case"},
};

struct Normalized {
    nlohmann::json settings;
    std::string issue;
};

namespace detail {

using Json = nlohmann::json;

inline std::string case_fold(std::string_view text, bool uppercase = false)
{
    std::string result(text);
    for (auto& ch : result) {
        if (uppercase && ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        } else if (!uppercase && ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

inline bool has_choice(std::span<const Choice> choices, std::string_view value)
{
    return std::ranges::any_of(choices, [value](const Choice& choice) { return choice.value == value; });
}

inline bool integer_in(const Json& value, std::int64_t minimum, std::int64_t maximum)
{
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number <= static_cast<std::uint64_t>(maximum) &&
               (minimum <= 0 || number >= static_cast<std::uint64_t>(minimum));
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number >= minimum && number <= maximum;
    }
    return false;
}

// WaterWall reads ordinary DNS numeric options from cJSON's integer and double
// values, so legacy integral spellings such as 2.0 carry the same meaning as 2.
inline Json dns_integer(const Json& value)
{
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (number >= 0 && number <= max_int && std::trunc(number) == number) {
            return static_cast<std::int32_t>(number);
        }
    }
    return value;
}

inline bool valid_text(const Json& value, bool allow_empty = false)
{
    const auto* text = value.get_ptr<const std::string*>();
    return text && (allow_empty || !text->empty()) && text->find('\0') == std::string::npos;
}

inline bool string_list(const Json& value)
{
    return value.is_array() && !value.empty() &&
           std::ranges::all_of(value, [](const Json& item) { return valid_text(item); });
}

inline unsigned flag_bit(std::string_view name)
{
    constexpr Choice aliases[] = {
        {"usevc", "use-vc"},
        {"tcp", "use-vc"},
        {"igntc", "ignore-truncated"},
        {"norecurse", "no-recurse"},
        {"stay-open", "stayopen"},
        {"nocheckresp", "no-check-response"},
        {"no-dflt-svr", "no-default-server"},
    };
    const auto alias = std::ranges::find(aliases, name, &Choice::value);
    if (alias != std::end(aliases)) {
        name = alias->label;
    }
    for (std::size_t index = 0; index < std::size(dns_flags); ++index) {
        if (name == dns_flags[index].value) {
            return 1U << index;
        }
    }
    return 0;
}

inline Json normalize_flags(const Json& value)
{
    unsigned flags = 0;
    if (value.is_number()) {
        const auto number = dns_integer(value);
        if (!integer_in(number, 0, (1U << std::size(dns_flags)) - 1)) {
            return {};
        }
        flags = number.get<unsigned>();
    } else if (value.is_string()) {
        flags = flag_bit(value.get_ref<const std::string&>());
        if (flags == 0) {
            return {};
        }
    } else if (value.is_array() || value.is_object()) {
        for (const auto& entry : value.items()) {
            if (value.is_object()) {
                const auto bit = flag_bit(entry.key());
                if (bit == 0 || !entry.value().is_boolean()) {
                    return {};
                }
                if (entry.value() == true) {
                    flags |= bit;
                }
            } else {
                const auto* name = entry.value().get_ptr<const std::string*>();
                const auto bit = name ? flag_bit(*name) : 0;
                if (bit == 0) {
                    return {};
                }
                flags |= bit;
            }
        }
    } else {
        return {};
    }

    auto names = Json::array();
    for (std::size_t index = 0; index < std::size(dns_flags); ++index) {
        if ((flags & (1U << index)) != 0) {
            names.push_back(dns_flags[index].value);
        }
    }
    return names;
}

inline Json normalize_field(const Field& field, const Json& value, bool dns)
{
    const std::string_view key = field.key;
    const auto* text = value.get_ptr<const std::string*>();
    switch (field.kind) {
    case Kind::integer: {
        auto number = dns ? dns_integer(value) : value;
        return integer_in(number, std::numeric_limits<std::int64_t>::min(),
                          std::numeric_limits<std::int64_t>::max()) &&
                       valid_integer(field, number.get<std::int64_t>())
                   ? number
                   : Json{};
    }
    case Kind::boolean:
        return value.is_boolean() ? value : Json{};
    case Kind::text:
        if (key == "servers" && string_list(value)) {
            std::string csv;
            for (const auto& item : value) {
                const auto& entry = item.get_ref<const std::string&>();
                if (entry.find(',') != std::string::npos) {
                    return {};
                }
                if (!csv.empty()) {
                    csv += ',';
                }
                csv += entry;
            }
            return csv;
        }
        return valid_text(value, !dns) ? value : Json{};
    case Kind::log_level:
        if (text) {
            auto level = case_fold(*text, true);
            if (has_choice(log_levels, level)) {
                return level;
            }
        }
        return {};
    case Kind::ram_profile:
        if (integer_in(value, 0, 6)) {
            constexpr const char* names[] = {"minimal",       "minimal", nullptr, "client",
                                             "client-larger", nullptr,   "server"};
            const auto* name = names[value.get<int>()];
            return name ? Json(name) : value;
        }
        if (text) {
            auto name = case_fold(*text);
            if (name == "ultralow") {
                name = "minimal";
            }
            if (name != "2" && name != "5" && has_choice(ram_profiles, name)) {
                return name;
            }
        }
        return {};
    case Kind::domain_strategy:
        if (integer_in(value, 0, 4)) {
            return domain_strategies[value.get<int>()].value;
        }
        if (text) {
            auto strategy = case_fold(*text);
            if (has_choice(domain_strategies, strategy)) {
                return strategy;
            }
        }
        return {};
    case Kind::lookups:
        return text && has_choice(lookup_orders, *text) ? value : Json{};
    case Kind::domains:
        return string_list(value) ? value : Json{};
    case Kind::flags:
        return normalize_flags(value);
    }
    return {};
}

inline std::string expected_value(const Field& field, bool dns)
{
    using enum Kind;
    switch (field.kind) {
    case integer: {
        const std::string range = std::to_string(field.minimum) + " to " + std::to_string(field.maximum);
        if (field.allow_zero) {
            return "must be 0 or a whole number from " + range + '.';
        }
        return "must be a whole number from " + range + '.';
    }
    case boolean:
        return "must be true or false.";
    case text:
        if (std::string_view(field.key) == "servers") {
            return "must be a non-empty comma-separated list of servers.";
        }
        return dns ? "must be non-empty text." : "must be text.";
    case domains:
        return "must contain at least one non-empty search domain.";
    case flags:
        return "contains invalid resolver flags.";
    default:
        return "must be one of the supported options.";
    }
}

inline void report_issue(std::string& issues, const std::string& message)
{
    // The notice travels with each workspace snapshot until the settings are reviewed.
    if (issues.size() >= 400) {
        constexpr std::string_view more = " Additional fields need review.";
        if (!issues.ends_with(more)) {
            issues += more;
        }
        return;
    }
    if (!issues.empty()) {
        issues += ' ';
    }
    issues += message;
}

// Walk only fields WaterWall 1.6.0 understands. Return the usable subset so
// existing databases can be reviewed without overwriting their saved values.
inline Json normalize_group(const Json& source, std::string_view path, std::string& issue)
{
    auto settings = Json::object();
    if (!source.is_object()) {
        report_issue(issue, std::string(path.empty() ? "Core settings" : path) + " must be an object.");
        return settings;
    }

    for (const auto& [key, value] : source.items()) {
        const auto name = path.empty() ? key : std::string(path) + '.' + key;
        const bool dns = path == "dns" || path == "dns.server-failover";
        if ((path.empty() && (key == "log" || key == "misc" || key == "dns")) ||
            (path == "log" && has_choice(loggers, key)) || (path == "dns" && key == "server-failover")) {
            auto group = normalize_group(value, name, issue);
            if (!group.empty() || (value.is_object() && name == "dns.server-failover")) {
                settings[key] = std::move(group);
            }
            continue;
        }

        const Field* field = nullptr;
        auto find = [&](std::span<const Field> fields) {
            const auto it = std::ranges::find(fields, key, &Field::key);
            return it == fields.end() ? nullptr : &*it;
        };
        if (path == "misc") {
            field = find(runtime_fields);
            if (!field) {
                field = find(stack_fields);
            }
        } else if (path == "log" && key == log_path.key) {
            field = &log_path;
        } else if (path.starts_with("log.")) {
            field = find(logger_fields);
        } else if (path == "dns") {
            field = find(dns_fields);
            if (!field) {
                field = find(dns_advanced_fields);
            }
        } else if (path == "dns.server-failover") {
            field = find(failover_fields);
        }

        if (field) {
            auto normalized = normalize_field(*field, value, dns);
            if (!normalized.is_null()) {
                settings[key] = std::move(normalized);
            } else {
                report_issue(issue, name + ' ' + expected_value(*field, dns));
            }
        } else if (path.empty() && key == "configs") {
            report_issue(issue, "Wave supplies configs from the enabled profiles.");
        } else if (path.empty() && key == "domain-strategy") {
            report_issue(issue, "Set the address preference under dns.domain-strategy.");
        } else if (name.size() > 80) {
            report_issue(issue, "An unsupported WaterWall core setting has a long field name.");
        } else {
            report_issue(issue, "Unsupported WaterWall core setting: " + name + '.');
        }
    }
    return settings;
}

} // namespace detail

inline Normalized normalize(const nlohmann::json& settings)
{
    constexpr char size_error[] = "Core settings must fit within 16 KiB of compact JSON.";
    Normalized result;
    if (settings.dump().size() > max_bytes) {
        return {nlohmann::json::object(), size_error};
    }
    result.settings = detail::normalize_group(settings, {}, result.issue);
    if (result.settings.dump().size() > max_bytes) {
        detail::report_issue(result.issue, size_error);
    }
    return result;
}

} // namespace wave::core_settings
