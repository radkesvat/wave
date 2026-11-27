#include "waterwall_schematic_catalog.h"

#include "private_temp_directory.h"
#include "waterwall_schematic.h"
#include "waterwall_schematic_file.h"
#include "waterwall_tool.h"

#include <openssl/evp.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view github_api = "https://api.github.com/repos/radkesvat/wave";
constexpr std::string_view catalog_path = "/contents/backend/schematics?ref=main";
constexpr std::string_view file_prefix = "waterwall-";
constexpr std::string_view file_suffix = ".json";
constexpr std::size_t max_listing_bytes = 2 * 1024 * 1024;
constexpr auto request_timeout = 12s;
constexpr auto refresh_timeout = 2min;
constexpr std::string_view cancelled = "WaterWall schematic check cancelled.";

std::string_view release_version(std::string_view name)
{
    if (name.size() <= file_prefix.size() + file_suffix.size() || !name.starts_with(file_prefix) ||
        !name.ends_with(file_suffix)) {
        return {};
    }
    name.remove_prefix(file_prefix.size());
    name.remove_suffix(file_suffix.size());
    return valid_waterwall_version(name) ? name : std::string_view{};
}

std::array<unsigned, 3> version_parts(std::string_view version)
{
    std::array<unsigned, 3> parts{};
    for (auto& part : parts) {
        const auto number = version.substr(0, version.find('.'));
        std::from_chars(number.data(), number.data() + number.size(), part);
        version.remove_prefix(std::min(version.size(), number.size() + 1));
    }
    return parts;
}

bool trusted(const fs::path& path, bool directory)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) &&
           (info.st_uid == ::geteuid() || info.st_uid == 0) && !(info.st_mode & (S_IWGRP | S_IWOTH));
}

bool missing(const fs::path& path)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) != 0 && errno == ENOENT;
}

std::optional<std::string> file_contents(const fs::path& path, std::size_t limit)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > limit) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!input.read(contents.data(), static_cast<std::streamsize>(size)) || input.peek() != EOF ||
        input.bad()) {
        return {};
    }
    return contents;
}

bool is_blob_id(std::string_view value)
{
    return value.size() == 40 && std::ranges::all_of(value, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

bool matches_blob(const fs::path& path, std::size_t size, std::string_view id)
{
    if (!trusted(path, false)) {
        return false;
    }
    const auto contents = file_contents(path, size);
    if (!contents || contents->size() != size) {
        return false;
    }
    // Git hashes the object header and the exact bytes returned by the blob endpoint.
    auto git_object = "blob " + std::to_string(size);
    git_object.push_back('\0');
    git_object += *contents;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
    unsigned digest_size = 0;
    if (EVP_Digest(git_object.data(), git_object.size(), digest.data(), &digest_size, EVP_sha1(), nullptr) !=
            1 ||
        digest_size * 2 != id.size()) {
        return false;
    }
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t i = 0; i < digest_size; ++i) {
        if (hex[digest[i] >> 4] != id[i * 2] || hex[digest[i] & 15] != id[i * 2 + 1]) {
            return false;
        }
    }
    return true;
}

std::string fetch(std::string url, const char* accept, const fs::path& path, std::size_t size,
                  std::chrono::steady_clock::time_point deadline, std::stop_token stop)
{
    const auto time_left =
        std::chrono::ceil<std::chrono::seconds>(deadline - std::chrono::steady_clock::now());
    if (time_left <= 0s) {
        return "WaterWall schematic check timed out.";
    }
    const auto timeout = std::min(request_timeout, time_left);
    return run_waterwall_tool({"curl", "-q", "--fail", "--silent", "--show-error", "--proto", "=https",
                               "--connect-timeout", "4", "--max-time", std::to_string(timeout.count()),
                               "--max-filesize", std::to_string(size), "--user-agent", "Wave", "--header",
                               std::string("Accept: ") + accept, std::move(url)},
                              path, size, timeout, stop);
}

std::shared_ptr<const WaterWallSchematic> prepare_cached_schematic(const fs::path& path)
{
    if (!trusted(path, false)) {
        return {};
    }
    try {
        return prepare_waterwall_schematic_file(path);
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace

std::string fetch_waterwall_schematics(const fs::path& bin, std::string_view selected_version,
                                       const Json& selected_packages, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return std::string(cancelled);
    }
    if (!trusted(bin, true)) {
        return "Cannot use the WaterWall schematic cache directory.";
    }
    const PrivateTempDirectory staging(bin, ".waterwall-catalog-");
    if (staging.path().empty()) {
        return "Cannot create a private WaterWall schematic staging directory.";
    }
    const auto deadline = std::chrono::steady_clock::now() + refresh_timeout;
    const auto listing = staging.path() / "catalog.json";
    if (const auto issue = fetch(std::string(github_api) + std::string(catalog_path),
                                 "application/vnd.github+json", listing, max_listing_bytes, deadline, stop);
        !issue.empty()) {
        return stop.stop_requested() ? std::string(cancelled)
                                     : "Could not fetch the Wave schematic list from GitHub.";
    }
    const auto body = file_contents(listing, max_listing_bytes);
    bool too_deep = false;
    const auto catalog = body ? Json::parse(
                                    *body,
                                    [&](int depth, auto, Json&) {
                                        too_deep |= depth > 16;
                                        return !too_deep;
                                    },
                                    false)
                              : Json{};
    if (too_deep || !catalog.is_array()) {
        return "GitHub returned an invalid Wave schematic list.";
    }
    std::string feedback;
    for (const auto& item : catalog) {
        if (stop.stop_requested()) {
            return std::string(cancelled);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return "WaterWall schematic check timed out.";
        }
        const auto name = schematic_text(schematic_field(item, "name"));
        const auto version = release_version(name);
        if (schematic_field(item, "type") != "file" || version.empty()) {
            continue;
        }
        const auto id = schematic_text(schematic_field(item, "sha"));
        const auto& bytes = schematic_field(item, "size");
        if (!is_blob_id(id) || !bytes.is_number_integer() || bytes <= 0 ||
            bytes > max_waterwall_schematic_bytes) {
            feedback = "GitHub returned invalid file metadata for " + std::string(name) + '.';
            continue;
        }
        const auto size = bytes.get<std::size_t>();
        const auto directory = waterwall_release_directory(bin, version);
        if (!trusted(directory, true) && !missing(directory)) {
            feedback = "Cannot use the WaterWall release directory for " + std::string(version) + '.';
            continue;
        }
        const auto destination = waterwall_schematic_path(bin, version);
        if (!trusted(destination, false) && !missing(destination)) {
            feedback = "Cannot use the cached WaterWall schematic " + std::string(name) + '.';
            continue;
        }
        if (matches_blob(destination, size, id)) {
            if (!prepare_cached_schematic(destination)) {
                feedback = "Failed to validate " + std::string(name) + '.';
            }
            continue;
        }
        const auto candidate = staging.path() / name;
        const auto issue = fetch(std::string(github_api) + "/git/blobs/" + std::string(id),
                                 "application/vnd.github.raw+json", candidate, size, deadline, stop);
        const auto parsed = issue.empty() && matches_blob(candidate, size, id)
                                ? prepare_cached_schematic(candidate)
                                : nullptr;
        if (!parsed) {
            feedback = "Could not download or validate " + std::string(name) + '.';
            continue;
        }
        if (stop.stop_requested()) {
            return std::string(cancelled);
        }
        const auto current = version != selected_version && trusted(directory / "Waterwall", false)
                                 ? prepare_cached_schematic(destination)
                                 : nullptr;
        const auto* packages = version == selected_version ? &selected_packages
                               : current                   ? &waterwall_schematic(*current).at("packages")
                                                           : nullptr;
        if (packages && *packages != waterwall_schematic(*parsed).at("packages")) {
            feedback = "The published archive metadata changed for selected or installed WaterWall " +
                       std::string(version) + ". The cached schematic was kept.";
            continue;
        }
        if ((::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) || !trusted(directory, true)) {
            feedback = "Cannot use the WaterWall release directory for " + std::string(version) + '.';
            continue;
        }
        std::error_code error;
        fs::rename(candidate, destination, error);
        if (error) {
            feedback = "Could not publish " + std::string(name) + " in the WaterWall schematic cache.";
        }
    }
    return feedback;
}

std::vector<std::string> cached_waterwall_versions(const fs::path& bin)
{
    std::vector<std::string> result;
    if (!trusted(bin, true)) {
        return result;
    }
    std::error_code error;
    const fs::directory_iterator end;
    for (fs::directory_iterator entry(bin, error); !error && entry != end; entry.increment(error)) {
        const auto version = entry->path().filename().string();
        if (valid_waterwall_version(version) && trusted(entry->path(), true) &&
            prepare_cached_schematic(waterwall_schematic_path(bin, version))) {
            result.push_back(version);
        }
    }
    std::ranges::sort(result, std::greater{}, version_parts);
    return result;
}

bool is_newer_waterwall_version(std::string_view candidate, std::string_view current)
{
    return version_parts(candidate) > version_parts(current);
}

} // namespace wave
