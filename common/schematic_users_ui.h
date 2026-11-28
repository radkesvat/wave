#pragma once

#include "waterwall_schematic.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>

namespace wave {

struct SchematicUserNumberBounds {
    std::uint64_t minimum = 0;
    std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
};

inline constexpr SchematicUserNumberBounds explicit_user_id_bounds{1};
inline constexpr SchematicUserNumberBounds user_stat_interval_bounds{0, std::numeric_limits<int>::max()};
inline constexpr std::uint64_t bytes_per_megabyte = 1'000'000;

constexpr SchematicUserNumberBounds whole_megabyte_bounds(SchematicUserNumberBounds bytes)
{
    const auto minimum = bytes.minimum / bytes_per_megabyte + (bytes.minimum % bytes_per_megabyte != 0);
    return {minimum, bytes.maximum / bytes_per_megabyte};
}

template <typename Row>
std::optional<std::uint64_t> next_sequential_user_id(const std::map<std::uint64_t, Row>& users,
                                                     SchematicUserNumberBounds bounds, bool gigabytes,
                                                     std::uint64_t known_id = 0)
{
    auto highest = known_id <= bounds.maximum ? known_id : 0;
    const auto upper = users.upper_bound(bounds.maximum);
    if (upper != users.begin()) {
        highest = std::max(highest, std::prev(upper)->first);
    }
    if (highest >= bounds.maximum) {
        return std::nullopt;
    }
    bounds.minimum = std::max(highest + 1, bounds.minimum);
    if (gigabytes) {
        const auto megabytes = whole_megabyte_bounds(bounds);
        if (megabytes.minimum > megabytes.maximum) {
            return std::nullopt;
        }
        return megabytes.minimum * bytes_per_megabyte;
    }
    return bounds.minimum;
}

inline constexpr char exhausted_user_ids[] =
    "No higher account ID is available in the configured range. Enter an unused ID.";

inline SchematicUserNumberBounds schematic_user_number_bounds(const nlohmann::json& field,
                                                               SchematicUserNumberBounds defaults)
{
    return {field.value("minimum", defaults.minimum), field.value("maximum", defaults.maximum)};
}

namespace schematic_users_ui_detail {

using Json = nlohmann::json;

inline bool valid_text(const Json& value)
{
    return value.is_string() && schematic_text(value).find('\0') == std::string_view::npos;
}

inline bool optional_text(const Json& object, std::string_view key)
{
    const auto value = object.find(key);
    return value == object.end() || valid_text(*value);
}

inline bool nonempty_text(const Json& object, std::string_view key)
{
    const auto& value = schematic_field(object, key);
    return valid_text(value) && !schematic_text(value).empty();
}

inline bool valid_pointer(std::string_view path)
{
    if (!path.starts_with('/')) {
        return false;
    }
    for (auto position = path.find('~'); position != path.npos; position = path.find('~', position + 2)) {
        if (position + 1 == path.size() || (path[position + 1] != '0' && path[position + 1] != '1')) {
            return false;
        }
    }
    return true;
}

inline bool optional_bound(const Json& field, std::string_view key)
{
    const auto bound = field.find(key);
    return bound == field.end() || bound->is_number_unsigned() ||
           (bound->is_number_integer() && bound->get<std::int64_t>() >= 0);
}

inline constexpr char invalid_editor[] = "Invalid user editor metadata.";

inline std::string validate_number(const Json& field)
{
    const auto gigabytes = field.find("gigabytes");
    const auto& path_field = schematic_field(field, "path");
    const auto path = schematic_text(path_field);
    if (!valid_text(path_field) || !valid_pointer(path) ||
        !nonempty_text(field, "label") || !optional_text(field, "hint") || !optional_text(field, "help") ||
        (gigabytes != field.end() && !gigabytes->is_boolean())) {
        return invalid_editor;
    }
    if (optional_bound(field, "minimum") && optional_bound(field, "maximum")) {
        const auto bounds = schematic_user_number_bounds(field, {});
        if (bounds.minimum <= bounds.maximum) {
            return {};
        }
    }
    return "Invalid bounds for offline user number " + std::string(path) + '.';
}

inline std::string validate_numbers(const Json& list)
{
    if (!list.is_array()) {
        return invalid_editor;
    }
    for (const auto& field : list) {
        if (auto error = validate_number(field); !error.empty()) {
            return error;
        }
    }
    return {};
}

inline std::string validate_groups(const Json& groups)
{
    if (!groups.is_array()) {
        return invalid_editor;
    }
    for (const auto& group : groups) {
        if (!nonempty_text(group, "label")) {
            return invalid_editor;
        }
        if (auto error = validate_numbers(schematic_field(group, "fields")); !error.empty()) {
            return error;
        }
    }
    return {};
}

} // namespace schematic_users_ui_detail

inline std::string validate_schematic_users_ui(const nlohmann::json& users)
{
    using namespace schematic_users_ui_detail;
    if (users.is_null()) {
        return {};
    }
    if (!users.is_object()) {
        return invalid_editor;
    }
    for (const char* key : {"offline_description", "live_description", "starter_label",
                            "starter_compact_label", "starter_help", "live_setup_help", "limits_help"}) {
        if (!optional_text(users, key)) {
            return invalid_editor;
        }
    }
    const auto fields = users.find("fields");
    if (fields != users.end() && !fields->is_object()) {
        return invalid_editor;
    }
    for (const auto& [role, field] : schematic_field(users, "fields").items()) {
        if (!nonempty_text(field, "key") || !nonempty_text(field, "label") || !optional_text(field, "hint") ||
            !optional_text(field, "help") || !optional_text(field, "edit_hint") ||
            !optional_text(field, "edit_help")) {
            return invalid_editor;
        }
        if (role == "id" || role == "group") {
            if (auto error = validate_number(field); !error.empty()) {
                return error;
            }
        }
    }
    if (const auto access = users.find("access_fields"); access != users.end()) {
        if (auto error = validate_numbers(*access); !error.empty()) {
            return error;
        }
    }
    for (const char* key : {"limit_groups", "activity_groups"}) {
        if (const auto groups = users.find(key); groups != users.end()) {
            if (auto error = validate_groups(*groups); !error.empty()) {
                return error;
            }
        }
    }
    return {};
}

} // namespace wave
