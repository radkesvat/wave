#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace wave::theme {

enum class Preset {
    standard,
    light,
    classic,
    dark,
    darker
};

inline constexpr std::array<std::string_view, 5> preset_keys{
    "default", "light", "classic", "dark", "darker"};

constexpr std::string_view preset_key(Preset preset)
{
    return preset_keys[static_cast<std::size_t>(preset)];
}

constexpr std::optional<Preset> parse_preset(std::string_view key)
{
    for (std::size_t i = 0; i < preset_keys.size(); ++i) {
        if (key == preset_keys[i]) {
            return static_cast<Preset>(i);
        }
    }
    return std::nullopt;
}

} // namespace wave::theme
