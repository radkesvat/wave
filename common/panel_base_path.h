#pragma once

#include <cstddef>
#include <string_view>

namespace wave {

inline constexpr std::string_view panel_base_path_key = "panel-base-path";
inline constexpr std::string_view panel_setup_prefix = "/_wave-setup";
inline constexpr std::size_t max_panel_base_path_bytes = 128;

constexpr bool has_panel_prefix(std::string_view path, std::string_view prefix)
{
    return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) &&
                              path[prefix.size()] == '/');
}

constexpr bool valid_panel_base_path(std::string_view path)
{
    if (path.size() < 2 || path.size() > max_panel_base_path_bytes || path.front() != '/' ||
        has_panel_prefix(path, panel_setup_prefix)) {
        return false;
    }
    constexpr std::string_view segment_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    path.remove_prefix(1);
    for (;;) {
        const auto slash = path.find('/');
        const auto segment = path.substr(0, slash);
        if (segment.empty() || segment.find_first_not_of(segment_chars) != segment.npos) {
            return false;
        }
        if (slash == path.npos) {
            return true;
        }
        path.remove_prefix(slash + 1);
    }
}

} // namespace wave
