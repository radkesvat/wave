#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace wave {

inline constexpr std::size_t max_core_config_name_bytes = 64;

inline std::string_view core_config_name(std::string_view input)
{
    if (input.size() > max_core_config_name_bytes ||
        input.find_first_of("/\\") != input.npos ||
        std::ranges::any_of(input, [](unsigned char c) { return c < ' ' || c == 0x7f; })) {
        return {};
    }
    const auto first = input.find_first_not_of(' ');
    if (first == input.npos) {
        return {};
    }
    const auto name = input.substr(first, input.find_last_not_of(' ') - first + 1);
    return name == "." || name == ".." ? std::string_view{} : name;
}

} // namespace wave
