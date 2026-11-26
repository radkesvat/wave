#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace wave {

enum class NodeLinkKind {
    branch,
    reference
};

struct NodeLinkField {
    const char* key;
    const char* label;
    NodeLinkKind kind;
    const char* target_type = nullptr;
    const char* collection = nullptr;
    const char* object = nullptr;
    std::array<const char*, 2> aliases{};
    bool string_alias_fallback = false;
    bool target_requires_unpaired = false;
    const char* activation_setting = nullptr;
    bool activation_is_boolean = false;
};

namespace node_links_detail {

using enum NodeLinkKind;

// These names reach the owning config's NodeManager, including embedded DNS
// resolvers. APIServer's config/node selectors address a separate registry.
inline constexpr NodeLinkField auth_client{"auth-client-node-name", "Authentication client", reference,
                                           "AuthenticationClient"};
inline constexpr NodeLinkField fallback{"fallback-node-name", "Fallback", branch};
inline constexpr NodeLinkField fallback_aliases{.key = "fallback-node-name",
                                                .label = "Fallback",
                                                .kind = branch,
                                                .aliases = {"fallback-node", "fallback"}};
inline constexpr NodeLinkField dns{"dns-next", "DNS branch", branch};
inline constexpr NodeLinkField nested_dns{
    .key = "dns-next", .label = "DNS branch", .kind = branch, .object = "dns-behaviour"};

inline constexpr NodeLinkField auth[] = {auth_client};
inline constexpr NodeLinkField fallback_only[] = {fallback_aliases};
inline constexpr NodeLinkField proxy_server[] = {auth_client, fallback};
inline constexpr NodeLinkField protocol_server[] = {auth_client, fallback_aliases};
inline constexpr NodeLinkField resolver[] = {dns};
inline constexpr NodeLinkField resolver_owner[] = {nested_dns};
inline constexpr NodeLinkField bridge[] = {{"pair", "Pair", reference, "Bridge"}};
inline constexpr NodeLinkField half_duplex_client[] = {{"upload", "Upload", branch},
                                                       {"download", "Download", branch}};
inline constexpr NodeLinkField half_duplex_server[] = {{.key = "pair",
                                                        .label = "Pair",
                                                        .kind = reference,
                                                        .target_type = "HalfDuplexServer",
                                                        .target_requires_unpaired = true}};
inline constexpr NodeLinkField ip_manipulator[] = {{.key = "real-sni-upstream-node",
                                                    .label = "SNI upstream",
                                                    .kind = branch,
                                                    .activation_setting = "smuggle-sni"},
                                                   {.key = "real-fin-upstream-node",
                                                    .label = "FIN upstream",
                                                    .kind = branch,
                                                    .activation_setting = "smuggle-fin",
                                                    .activation_is_boolean = true}};
inline constexpr NodeLinkField packet_split[] = {{"up", "Up", branch}, {"down", "Down", branch}};
inline constexpr NodeLinkField reality_server[] = {{"destination", "Destination", branch}};
inline constexpr NodeLinkField router[] = {
    {.key = "target", .label = "Rule target", .kind = branch, .collection = "rules"}, nested_dns};
inline constexpr NodeLinkField sniff_router[] = {{.key = "next",
                                                  .label = "Route target",
                                                  .kind = branch,
                                                  .collection = "routes",
                                                  .aliases = {"target"},
                                                  .string_alias_fallback = true}};

struct NodeTypeLinks {
    std::string_view type;
    std::span<const NodeLinkField> fields;
};

inline constexpr NodeTypeLinks node_types[] = {
    {"Bridge", bridge},
    {"ConnectionToPackets", resolver_owner},
    {"DomainResolver", resolver},
    {"HalfDuplexClient", half_duplex_client},
    {"HalfDuplexServer", half_duplex_server},
    {"HttpProxyClient", resolver_owner},
    {"HttpProxyServer", proxy_server},
    {"IpManipulator", ip_manipulator},
    {"PacketSplitStream", packet_split},
    {"RealityServer", reality_server},
    {"Router", router},
    {"SniffRouter", sniff_router},
    {"Socks5Client", resolver_owner},
    {"Socks5Server", auth},
    {"TcpConnector", resolver_owner},
    {"TcpUdpConnector", resolver_owner},
    {"TlsServer", fallback_only},
    {"TrojanClient", resolver_owner},
    {"TrojanServer", protocol_server},
    {"UdpConnector", resolver_owner},
    {"UdpStatelessSocket", resolver_owner},
    {"UserController", auth},
    {"VlessClient", resolver_owner},
    {"VlessServer", protocol_server},
    {"VmessClient", resolver_owner},
    {"VmessServer", protocol_server},
    {"WireGuardServer", auth},
};

} // namespace node_links_detail

inline std::span<const NodeLinkField> node_link_fields(std::string_view type)
{
    using namespace node_links_detail;
    const auto found = std::ranges::find(node_types, type, &NodeTypeLinks::type);
    return found != std::end(node_types) ? found->fields : std::span<const NodeLinkField>{};
}

template <typename Json> const char* node_link_key(const Json& container, const NodeLinkField& field)
{
    const char* first_present = nullptr;
    for (const auto* key : {field.key, field.aliases[0], field.aliases[1]}) {
        if (!key) {
            continue;
        }
        const auto value = container.find(key);
        if (value != container.end()) {
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

template <typename Json, typename Visitor> void visit_node_links(Json& node, Visitor&& visit)
{
    const auto type = node.find("type");
    const auto settings = node.find("settings");
    if (type == node.end() || !type->is_string() || settings == node.end() || !settings->is_object()) {
        return;
    }
    for (const auto& field : node_link_fields(type->template get_ref<const std::string&>())) {
        auto* container = &*settings;
        if (field.object) {
            const auto object = settings->find(field.object);
            if (object == settings->end() || !object->is_object()) {
                continue;
            }
            container = &*object;
        }
        if (field.collection) {
            const auto collection = container->find(field.collection);
            if (collection != container->end() && collection->is_array()) {
                for (auto& row : *collection) {
                    visit(row, field, node_link_key(row, field));
                }
            }
        } else {
            const auto* key = node_link_key(*container, field);
            if (container->contains(key)) {
                visit(*container, field, key);
            }
        }
    }
}

template <typename Json> void erase_node_link(Json& container, const NodeLinkField& field)
{
    if (!container.is_object()) {
        return;
    }
    container.erase(field.key);
    for (const auto* alias : field.aliases) {
        if (alias) {
            container.erase(alias);
        }
    }
}

template <typename Json> bool allows_node_next(std::string_view type, const Json& settings)
{
    return type != "PacketSplitStream" &&
           (type != "HalfDuplexClient" || (!settings.contains("upload") && !settings.contains("download"))) &&
           (type != "HalfDuplexServer" || !settings.contains("pair"));
}

template <typename Json>
bool node_link_target_type_matches(const NodeLinkField& field, std::string_view target_type,
                                   const Json& target_settings)
{
    return (!field.target_type || target_type == field.target_type) &&
           (!field.target_requires_unpaired || !target_settings.contains("pair"));
}

template <typename Json> bool node_link_active(const Json& container, const NodeLinkField& field)
{
    if (!field.activation_setting) {
        return true;
    }
    const auto value = container.find(field.activation_setting);
    return value != container.end() &&
           (field.activation_is_boolean ? value->is_boolean() && *value == true : value->is_string());
}

} // namespace wave
