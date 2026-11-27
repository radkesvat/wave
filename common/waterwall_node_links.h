#pragma once

#include "waterwall_schematic.h"

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
    const char* target_missing_setting = nullptr;
    const char* activation_setting = nullptr;
    bool activation_is_boolean = false;
    const char* hint = "";
};

std::span<const NodeLinkField> node_link_fields(std::string_view type);
bool allows_node_next(std::string_view type, const nlohmann::json& settings);
bool allows_node_previous(std::string_view type);

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

template <typename Json>
bool node_link_target_type_matches(const NodeLinkField& field, std::string_view target_type,
                                   const Json& target_settings)
{
    return (!field.target_type || target_type == field.target_type) &&
           (!field.target_missing_setting || !target_settings.contains(field.target_missing_setting));
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
