#pragma once

#include "waterwall_schematic.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace wave::core_settings {

using Json = nlohmann::json;

inline constexpr std::size_t max_bytes = 16 * 1024;

inline std::string_view text(const Json& value, std::string_view fallback = "")
{
    const auto* stored = value.get_ptr<const std::string*>();
    return stored ? std::string_view(*stored) : fallback;
}

inline std::string_view text(const Json& object, std::string_view name, std::string_view fallback)
{
    return text(schematic_field(object, name), fallback);
}

inline bool enabled(const Json& object, std::string_view name)
{
    return schematic_field(object, name) == true;
}

inline const Json& specification()
{
    return schematic_field(waterwall_schematic(), "core_settings");
}

inline const Json& sections()
{
    return schematic_field(specification(), "sections");
}

inline const Json& field_at(const Json& spec, const Json::json_pointer& path)
{
    const auto parent = path.parent_pointer().to_string();
    for (const auto& section : spec.at("sections")) {
        for (const auto& group : section.at("groups")) {
            if (group.at("path") == parent) {
                const auto& fields = group.at("fields");
                const auto field = std::ranges::find_if(
                    fields, [&](const Json& descriptor) { return descriptor.at("key") == path.back(); });
                if (field != fields.end()) {
                    return *field;
                }
            }
        }
    }
    static const Json missing;
    return missing;
}

inline bool valid_integer(const Json& field, std::int64_t number)
{
    return (number >= field.at("min").get<std::int64_t>() && number <= field.at("max").get<std::int64_t>()) ||
           (enabled(field, "allow_zero") && number == 0);
}

struct Normalized {
    Json settings;
    std::string issue;
};

namespace detail {

inline std::string case_fold(std::string value, std::string_view mode)
{
    for (auto& ch : value) {
        if (mode == "upper" && ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        } else if (mode == "lower" && ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

inline bool integer_in(const Json& value, std::int64_t minimum, std::int64_t maximum)
{
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return maximum >= 0 && number <= static_cast<std::uint64_t>(maximum) &&
               (minimum <= 0 || number >= static_cast<std::uint64_t>(minimum));
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number >= minimum && number <= maximum;
    }
    return false;
}

inline Json integral_value(const Json& field, const Json& value)
{
    const auto& legacy_max = schematic_field(field, "legacy_float_max");
    if (value.is_number_float() && legacy_max.is_number_integer()) {
        const auto number = value.get<double>();
        if (number >= 0 && number <= legacy_max.get<std::int32_t>() && std::trunc(number) == number) {
            return static_cast<std::int32_t>(number);
        }
    }
    return value;
}

inline bool valid_text(const Json& value, bool allow_empty = false)
{
    const auto* string = value.get_ptr<const std::string*>();
    return string && (allow_empty || !string->empty()) && string->find('\0') == std::string::npos;
}

inline bool string_list(const Json& value)
{
    return value.is_array() && !value.empty() &&
           std::ranges::all_of(value, [](const Json& item) { return valid_text(item); });
}

inline const Json& choice(const Json& field, const Json& value)
{
    const auto& choices = schematic_field(field, "choices");
    const auto found = std::ranges::find_if(
        choices, [&](const Json& option) { return schematic_field(option, "value") == value; });
    static const Json missing;
    return found == choices.end() ? missing : *found;
}

inline std::string_view alias(const Json& field, std::string_view name)
{
    return text(schematic_field(field, "aliases"), name, name);
}

inline Json normalize_choice(const Json& field, const Json& input)
{
    const auto& legacy = schematic_field(field, "legacy_values");
    if (legacy.is_array() && integer_in(input, 0, static_cast<std::int64_t>(legacy.size()) - 1)) {
        return legacy[input.get<std::size_t>()];
    }
    if (input.is_string()) {
        auto value = case_fold(input.get<std::string>(), text(field, "case", ""));
        const auto normalized = alias(field, value);
        return choice(field, normalized).is_null() ? Json{} : Json(normalized);
    }
    return input.is_number_integer() && !choice(field, input).is_null() ? input : Json{};
}

inline std::uint32_t flag_bit(const Json& field, std::string_view name)
{
    const auto& option = choice(field, alias(field, name));
    return option.is_null() ? 0 : option.at("bit").get<std::uint32_t>();
}

inline Json normalize_flags(const Json& field, const Json& value)
{
    const auto& choices = field.at("choices");
    std::uint32_t flags = 0;
    if (value.is_number()) {
        const auto number = integral_value(field, value);
        if (!integer_in(number, 0, std::numeric_limits<std::uint32_t>::max())) {
            return {};
        }
        flags = number.get<std::uint32_t>();
        std::uint32_t mask = 0;
        for (const auto& option : choices) {
            mask |= option.at("bit").get<std::uint32_t>();
        }
        if ((flags & ~mask) != 0) {
            return {};
        }
    } else if (value.is_string()) {
        flags = flag_bit(field, text(value));
        if (flags == 0) {
            return {};
        }
    } else if (value.is_array() || value.is_object()) {
        for (const auto& [key, entry] : value.items()) {
            if (value.is_object()) {
                const auto bit = flag_bit(field, key);
                if (bit == 0 || !entry.is_boolean()) {
                    return {};
                }
                if (entry == true) {
                    flags |= bit;
                }
            } else {
                const auto bit = entry.is_string() ? flag_bit(field, text(entry)) : 0;
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
    for (const auto& option : choices) {
        if ((flags & option.at("bit").get<std::uint32_t>()) != 0) {
            names.push_back(option.at("value"));
        }
    }
    return names;
}

inline Json normalize_field(const Json& field, const Json& value)
{
    const auto kind = text(field, "kind", "");
    if (kind == "integer") {
        auto number = integral_value(field, value);
        return integer_in(number, std::numeric_limits<std::int64_t>::min(),
                          std::numeric_limits<std::int64_t>::max()) &&
                       valid_integer(field, number.get<std::int64_t>())
                   ? number
                   : Json{};
    }
    if (kind == "boolean") {
        return value.is_boolean() ? value : Json{};
    }
    if (kind == "text") {
        if (enabled(field, "accept_csv_array") && string_list(value)) {
            std::string csv;
            for (const auto& item : value) {
                const auto entry = text(item);
                if (entry.find(',') != std::string_view::npos) {
                    return {};
                }
                if (!csv.empty()) {
                    csv += ',';
                }
                csv += entry;
            }
            return csv;
        }
        return valid_text(value, enabled(field, "allow_empty")) ? value : Json{};
    }
    if (kind == "string_list") {
        return string_list(value) ? value : Json{};
    }
    if (kind == "choice") {
        return normalize_choice(field, value);
    }
    if (kind == "flags") {
        return normalize_flags(field, value);
    }
    return {};
}

inline std::string expected_value(const Json& field)
{
    if (const auto message = text(field, "invalid", ""); !message.empty()) {
        return std::string(message);
    }
    const auto kind = text(field, "kind", "");
    if (kind == "integer") {
        const auto range = field.at("min").dump() + " to " + field.at("max").dump();
        return (enabled(field, "allow_zero") ? "must be 0 or a whole number from "
                                             : "must be a whole number from ") +
               range + '.';
    }
    if (kind == "boolean") {
        return "must be true or false.";
    }
    if (kind == "text") {
        return enabled(field, "allow_empty") ? "must be text." : "must be non-empty text.";
    }
    if (kind == "string_list") {
        return "must contain at least one non-empty entry.";
    }
    if (kind == "flags") {
        return "contains invalid flags.";
    }
    return "must be one of the supported options.";
}

inline void report_issue(std::string& issues, const std::string& message)
{
    // The notice travels with each workspace snapshot until the settings are
    // reviewed.
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

inline void normalize_group(const Json& source, const Json::json_pointer& path, std::string_view name,
                            Json& settings, std::string& issue)
{
    settings = Json::object();
    if (!source.is_object()) {
        report_issue(issue, std::string(name.empty() ? "Core settings" : name) + " must be an object.");
        return;
    }

    const auto path_text = path.to_string();
    for (const auto& [key, value] : source.items()) {
        const auto child = path / key;
        const auto child_path = child.to_string();
        const auto label = name.empty() ? key : std::string(name) + '.' + key;
        const Json* descriptor = nullptr;
        bool group_found = false;
        bool preserve_empty = false;
        for (const auto& section : sections()) {
            for (const auto& group : schematic_field(section, "groups")) {
                const auto group_path = text(group, "path", "");
                if (group_path == child_path ||
                    (group_path.starts_with(child_path) && group_path[child_path.size()] == '/')) {
                    group_found = true;
                    preserve_empty |= group_path == child_path && enabled(group, "preserve_empty");
                }
                if (group_path == path_text) {
                    for (const auto& field : schematic_field(group, "fields")) {
                        if (text(field, "key", "") == key) {
                            descriptor = &field;
                            break;
                        }
                    }
                }
            }
        }

        if (group_found) {
            Json normalized;
            normalize_group(value, child, label, normalized, issue);
            if (!normalized.empty() || (preserve_empty && value.is_object())) {
                settings[key] = std::move(normalized);
            }
        } else if (descriptor) {
            auto normalized = normalize_field(*descriptor, value);
            if (!normalized.is_null()) {
                settings[key] = std::move(normalized);
            } else {
                report_issue(issue, label + ' ' + expected_value(*descriptor));
            }
        } else if (const auto reason =
                       text(schematic_field(specification(), "rejected_fields"), child_path, {});
                   !reason.empty()) {
            report_issue(issue, std::string(reason));
        } else if (label.size() > 80) {
            report_issue(issue, "An unsupported WaterWall core setting has a long field name.");
        } else {
            report_issue(issue, "Unsupported WaterWall core setting: " + label + '.');
        }
    }
}

inline bool valid_field(const Json& field)
{
    constexpr auto min_value = std::numeric_limits<std::int64_t>::min();
    constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
    if (!field.is_object() || !valid_text(schematic_field(field, "key")) ||
        !valid_text(schematic_field(field, "label")) || !valid_text(schematic_field(field, "hint"), true) ||
        !valid_text(schematic_field(field, "help"), true) ||
        (field.contains("invalid") && !valid_text(field.at("invalid")))) {
        return false;
    }
    for (const auto* option : {"allow_zero", "allow_empty", "accept_csv_array"}) {
        if (field.contains(option) && !field.at(option).is_boolean()) {
            return false;
        }
    }
    const auto kind = text(field, "kind", "");
    if (kind == "integer") {
        const auto& min = schematic_field(field, "min");
        const auto& max = schematic_field(field, "max");
        if (!integer_in(min, min_value, max_value) || !integer_in(max, min_value, max_value) ||
            min.get<std::int64_t>() > max.get<std::int64_t>()) {
            return false;
        }
    } else if (kind == "choice" || kind == "flags") {
        const auto& options = schematic_field(field, "choices");
        if (!options.is_array() || options.empty()) {
            return false;
        }
        std::set<Json> values;
        std::uint32_t bits = 0;
        for (const auto& option : options) {
            const auto& value = schematic_field(option, "value");
            if ((!valid_text(value) && (kind == "flags" || !integer_in(value, min_value, max_value))) ||
                !valid_text(schematic_field(option, "label")) || !values.insert(value).second ||
                (option.contains("help") && !valid_text(option.at("help"), true))) {
                return false;
            }
            if (kind == "flags") {
                const auto& bit = schematic_field(option, "bit");
                if (!integer_in(bit, 1, std::numeric_limits<std::uint32_t>::max())) {
                    return false;
                }
                const auto flag = bit.get<std::uint32_t>();
                if ((flag & (flag - 1)) != 0 || (bits & flag) != 0) {
                    return false;
                }
                bits |= flag;
            }
        }
        const auto& aliases = schematic_field(field, "aliases");
        if (field.contains("aliases") &&
            (!aliases.is_object() || !std::ranges::all_of(aliases, [&](const Json& value) {
                return valid_text(value) && values.contains(value);
            }))) {
            return false;
        }
        const auto& legacy = schematic_field(field, "legacy_values");
        if (field.contains("legacy_values") &&
            (!legacy.is_array() || !std::ranges::all_of(legacy, [&](const Json& value) {
                return value.is_null() || values.contains(value);
            }))) {
            return false;
        }
        if (field.contains("case") && schematic_field(field, "case") != "upper" &&
            schematic_field(field, "case") != "lower") {
            return false;
        }
    } else if (kind != "boolean" && kind != "text" && kind != "string_list") {
        return false;
    }
    return (kind != "flags" || !field.contains("default") || field.at("default").is_array()) &&
           (!field.contains("legacy_float_max") || integer_in(schematic_field(field, "legacy_float_max"), 0,
                                                              std::numeric_limits<std::int32_t>::max())) &&
           (!field.contains("default") || !normalize_field(field, field.at("default")).is_null());
}

} // namespace detail

inline bool valid_schematic(const Json& spec)
{
    const auto& tabs = schematic_field(spec, "sections");
    const auto& rejected = schematic_field(spec, "rejected_fields");
    if (!tabs.is_array() || tabs.empty() || !detail::valid_text(schematic_field(spec, "config_list_key")) ||
        (spec.contains("rejected_fields") &&
         (!rejected.is_object() ||
          !std::ranges::all_of(rejected, [](const Json& value) { return detail::valid_text(value); })))) {
        return false;
    }
    std::set<std::string> labels;
    std::set<std::string> paths;
    std::set<std::string> fields;
    for (const auto& tab : tabs) {
        const auto& label = schematic_field(tab, "label");
        const auto& groups = schematic_field(tab, "groups");
        if (!detail::valid_text(label) || !labels.insert(label.get<std::string>()).second ||
            !groups.is_array() || groups.empty()) {
            return false;
        }
        for (const auto& group : groups) {
            const auto path = text(group, "path", "");
            const auto& rows = schematic_field(group, "fields");
            if (!detail::valid_text(schematic_field(group, "path"), true) ||
                (!path.empty() && (path.front() != '/' || path.back() == '/' ||
                                   path.find("//") != path.npos || path.find('~') != path.npos)) ||
                !rows.is_array() || rows.empty() ||
                (group.contains("columns") && !detail::integer_in(schematic_field(group, "columns"), 1, 3)) ||
                (group.contains("heading") && !detail::valid_text(schematic_field(group, "heading"))) ||
                (group.contains("toggle") &&
                 (path.empty() || !detail::valid_text(schematic_field(group, "toggle")))) ||
                (group.contains("help") && !detail::valid_text(schematic_field(group, "help"), true)) ||
                (enabled(group, "collapsible") && !group.contains("heading"))) {
                return false;
            }
            for (const auto* option : {"collapsible", "compact", "preserve_empty"}) {
                if (group.contains(option) && !group.at(option).is_boolean()) {
                    return false;
                }
            }
            paths.emplace(path);
            for (const auto& field : rows) {
                if (!detail::valid_field(field) ||
                    !fields
                         .insert((Json::json_pointer(std::string(path)) / std::string(text(field, "key", "")))
                                     .to_string())
                         .second) {
                    return false;
                }
            }
        }
    }
    return std::ranges::none_of(paths, [&](const std::string& path) {
        for (Json::json_pointer group(path); !group.empty(); group.pop_back()) {
            if (fields.contains(group.to_string())) {
                return true;
            }
        }
        return false;
    });
}

inline Normalized normalize(const Json& settings)
{
    constexpr char size_error[] = "Core settings must fit within 16 KiB of compact JSON.";
    if (settings.dump().size() > max_bytes) {
        return {Json::object(), size_error};
    }
    Normalized result;
    detail::normalize_group(settings, Json::json_pointer{}, {}, result.settings, result.issue);
    if (result.settings.dump().size() > max_bytes) {
        detail::report_issue(result.issue, size_error);
    }
    return result;
}

} // namespace wave::core_settings
