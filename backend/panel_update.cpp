#include "panel_update.h"

#include "private_temp_directory.h"
#include "waterwall_tool.h"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr char tags_url[] = "https://api.github.com/repos/radkesvat/wave/tags";
constexpr std::size_t tags_per_page = 100;
constexpr std::size_t response_limit = 256 * 1024;
constexpr std::chrono::seconds request_timeout{12};
constexpr char cancelled[] = "Panel update check cancelled.";

struct PanelVersion {
    std::array<unsigned, 3> parts{};
    std::string number{};
    bool beta;
};

std::optional<PanelVersion> parse_version(std::string_view value)
{
    constexpr std::string_view beta_suffix = "-beta";
    if (value.starts_with('v')) {
        value.remove_prefix(1);
    }
    const bool beta = value.ends_with(beta_suffix);
    if (beta) {
        value.remove_suffix(beta_suffix.size());
    }
    const auto version_number = value;
    PanelVersion version{.beta = beta};
    for (std::size_t index = 0; index < version.parts.size(); ++index) {
        const auto separator = value.find('.');
        const auto number = value.substr(0, separator);
        const bool last = index + 1 == version.parts.size();
        if ((separator == std::string_view::npos) != last || number.empty() ||
            (number.size() > 1 && number.front() == '0')) {
            return {};
        }
        const auto end = number.data() + number.size();
        const auto result = std::from_chars(number.data(), end, version.parts[index]);
        if (result.ec != std::errc{} || result.ptr != end) {
            return {};
        }
        if (!last) {
            value.remove_prefix(separator + 1);
        }
    }
    version.number = version_number;
    return version;
}

bool newer_version(const PanelVersion& candidate, const PanelVersion& current)
{
    return candidate.parts > current.parts || (candidate.parts == current.parts && !candidate.beta && current.beta);
}

} // namespace

PanelUpdateCheck check_panel_update(std::string_view installed_version, std::stop_token stop)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + request_timeout;
    if (stop.stop_requested()) {
        return {{}, cancelled};
    }
    const auto installed = parse_version(installed_version);
    if (!installed) {
        return {{}, "The installed Wave panel version is invalid."};
    }
    constexpr char temporary_error[] = "Could not create a temporary directory for the panel update check.";
    std::error_code error;
    const auto parent = fs::temp_directory_path(error);
    if (error) {
        return {{}, temporary_error};
    }
    const PrivateTempDirectory temporary(parent, "wave-panel-check-");
    const auto& directory = temporary.path();
    if (directory.empty()) {
        return {{}, temporary_error};
    }

    constexpr char fetch_error[] = "Could not fetch the latest Wave tag from GitHub.";
    constexpr char invalid_list[] = "GitHub returned an invalid Wave tag list.";
    std::optional<PanelVersion> latest;
    bool saw_tags = false;
    for (std::size_t page = 1;; ++page) {
        const auto remaining = std::chrono::floor<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            return {{}, fetch_error};
        }
        const auto page_text = std::to_string(page);
        const auto url = std::string(tags_url) + "?per_page=" + std::to_string(tags_per_page) + "&page=" + page_text;
        const auto listing = directory / ("tags-" + page_text + ".json");
        const auto fetch = run_waterwall_tool(
            {"curl", "-q", "--fail", "--silent", "--show-error", "--proto", "=https", "--connect-timeout", "4",
             "--max-time", std::to_string(std::chrono::duration<double>(remaining).count()), "--max-filesize",
             std::to_string(response_limit), "--user-agent", "Wave", "--header", "Accept: application/vnd.github+json",
             url},
            listing, response_limit, remaining, stop);
        if (stop.stop_requested()) {
            return {{}, cancelled};
        }
        if (!fetch.empty()) {
            return {{}, fetch_error};
        }

        std::ifstream input(listing, std::ios::binary);
        const auto tags = Json::parse(input, nullptr, false);
        if (!tags.is_array() || tags.size() > tags_per_page) {
            return {{}, invalid_list};
        }
        saw_tags = saw_tags || !tags.empty();
        for (const auto& tag : tags) {
            const auto name = tag.find("name");
            if (name == tag.end() || !name->is_string()) {
                return {{}, invalid_list};
            }
            if (auto version = parse_version(name->get_ref<const std::string&>());
                version && (!latest || newer_version(*version, *latest))) {
                latest = std::move(version);
            }
        }
        if (tags.size() < tags_per_page) {
            break;
        }
    }
    if (stop.stop_requested()) {
        return {{}, cancelled};
    }
    if (Clock::now() >= deadline) {
        return {{}, fetch_error};
    }
    if (!latest && saw_tags) {
        return {{}, "GitHub returned no Wave tags matching MAJOR.MINOR.PATCH with an optional v prefix and -beta suffix."};
    }
    if (!latest) {
        return {};
    }
    return {latest->number + (latest->beta ? " beta" : ""), {}, newer_version(*latest, *installed)};
}

} // namespace wave
