#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace wave {

inline constexpr std::size_t max_core_config_bytes = 256 * 1024;
inline constexpr int max_core_config_depth = 64;

inline bool valid_core_node_name(std::string_view name)
{
    return name.size() <= 128 && name.find_first_not_of(' ') != name.npos &&
           std::ranges::none_of(name, [](unsigned char ch) { return ch < ' ' || ch == 0x7f; });
}

} // namespace wave
