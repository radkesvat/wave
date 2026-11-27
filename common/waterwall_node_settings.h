#pragma once

#include "waterwall_schematic.h"

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
#include <vector>

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
    std::vector<Field> children{};
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

namespace wave::node_settings {

std::span<const Field> fields(std::string_view type);

inline bool valid_value(const nlohmann::json& value, const Field& field)
{
    return (value.is_null() && field.nullable) || detail::setting_value(value, field);
}

std::string validate(std::string_view type, const nlohmann::json& settings);

} // namespace wave::node_settings
