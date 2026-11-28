#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace wave {

struct WaterWallSchematic;

inline constexpr std::size_t max_waterwall_schematic_bytes = 2 * 1024 * 1024;

inline const nlohmann::json& schematic_field(const nlohmann::json& object, std::string_view key)
{
    static const nlohmann::json empty;
    const auto entry = object.find(key);
    return entry == object.end() ? empty : *entry;
}

inline std::string_view schematic_text(const nlohmann::json& entry)
{
    const auto* text = entry.get_ptr<const std::string*>();
    return text ? std::string_view(*text) : std::string_view{};
}

// Candidates can be prepared off-thread. The workspace serializes activation and active readers.
std::shared_ptr<const WaterWallSchematic> prepare_waterwall_schematic(nlohmann::json document);
void activate_waterwall_schematic(std::shared_ptr<const WaterWallSchematic> schematic) noexcept;
const nlohmann::json& waterwall_schematic(const WaterWallSchematic& schematic);
std::string set_waterwall_schematic(nlohmann::json document);
void clear_waterwall_schematic();
const nlohmann::json& waterwall_schematic();
std::string_view waterwall_version();
std::string_view users_starter_management_slot(const nlohmann::json& schematic);
std::span<const std::string_view> waterwall_node_types();
const nlohmann::json& waterwall_node(std::string_view type);
bool valid_waterwall_version(std::string_view version);

} // namespace wave
