#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wave {

inline constexpr std::size_t max_core_config_bytes = 256 * 1024;
inline constexpr int max_core_config_depth = 64;

inline nlohmann::json parse_plain_core_config_json(std::string_view source, std::string_view invalid,
                                                  std::string& error)
{
    using Json = nlohmann::json;
    if (source.find('\0') != source.npos) {
        error = "The config contains a NUL byte.";
        return nullptr;
    }

    std::vector<std::set<std::string>> object_keys;
    auto check = [&](int depth, Json::parse_event_t event, Json& value) {
        if (!error.empty()) {
            return false;
        }
        if (depth > max_core_config_depth) {
            error = "The config exceeds Wave's 64-level nesting limit.";
            return false;
        }
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            object_keys.pop_back();
        } else if (event == Json::parse_event_t::key || event == Json::parse_event_t::value) {
            if (const auto* string = value.get_ptr<const std::string*>();
                string && string->find('\0') != string->npos) {
                error = "Config strings cannot contain a NUL character.";
                return false;
            }
            if (event == Json::parse_event_t::key &&
                !object_keys.back().insert(value.get_ref<const std::string&>()).second) {
                error = "The config contains a duplicate JSON object key.";
                return false;
            }
        }
        return true;
    };
    auto document = Json::parse(source, check, false);
    if (error.empty() && document.is_discarded()) {
        error = invalid;
    }
    return document;
}

inline bool valid_core_node_name(std::string_view name)
{
    return name.size() <= 128 && name.find_first_not_of(' ') != name.npos &&
           std::ranges::none_of(name, [](unsigned char ch) { return ch < ' ' || ch == 0x7f; });
}

} // namespace wave
