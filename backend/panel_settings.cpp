#include "panel_settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#endif

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr char default_host[] = "0.0.0.0";
constexpr int default_port = 2045;

fs::path executable_path([[maybe_unused]] const char* executable)
{
#if defined(__linux__)
    return fs::read_symlink("/proc/self/exe");
#elif defined(_WIN32)
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Cannot locate the Wave executable.");
    }
    path.resize(length);
    return fs::canonical(path);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("Cannot locate the Wave executable.");
    }
    return fs::canonical(path.data());
#else
    return fs::canonical(executable);
#endif
}

void write_database(const fs::path& path, const Json& settings, bool replace)
{
    const auto temporary = path.parent_path() / (".wave-db-" + std::to_string(std::random_device{}()));
    if (!fs::create_directory(temporary)) {
        throw std::runtime_error("Cannot create a temporary directory for wave-db.json.");
    }
    try {
        fs::permissions(temporary, fs::perms::owner_all);
        const auto pending = temporary / "wave-db.json";
        std::ofstream output(pending);
        output << settings.dump(2) << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot write wave-db.json.");
        }
        fs::permissions(pending, fs::perms::owner_read | fs::perms::owner_write);
        if (replace) {
            fs::rename(pending, path);
        } else {
            // Leave a database created by a concurrent startup intact.
            std::error_code error;
            fs::create_hard_link(pending, path, error);
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("Cannot create wave-db.json beside the executable: " + error.message());
            }
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        throw;
    }
    fs::remove_all(temporary);
}

Json read_database(const fs::path& path)
{
    if (!fs::is_regular_file(fs::symlink_status(path))) {
        throw std::runtime_error("wave-db.json must be a regular file beside the executable.");
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot read wave-db.json.");
    }
    auto settings = Json::parse(input, nullptr, false);
    if (!settings.is_object()) {
        throw std::runtime_error("wave-db.json must contain a JSON object.");
    }
    return settings;
}

std::string required_text(const Json& tls, const char* key)
{
    const auto field = tls.find(key);
    if (field == tls.end() || !field->is_string() || field->get_ref<const std::string&>().empty()) {
        throw std::runtime_error(std::string("Enabled TLS requires a nonempty tls.") + key + " in wave-db.json.");
    }
    return field->get<std::string>();
}

std::optional<TlsSettings> load_tls(const Json& settings)
{
    const auto tls = settings.find("tls");
    if (tls == settings.end()) {
        return {};
    }
    if (!tls->is_object()) {
        throw std::runtime_error("tls in wave-db.json must be a JSON object.");
    }
    const auto enabled = tls->find("enabled");
    if (enabled == tls->end()) {
        return {};
    }
    if (!enabled->is_boolean()) {
        throw std::runtime_error("tls.enabled in wave-db.json must be true or false.");
    }
    if (!enabled->get<bool>()) {
        return {};
    }
    return TlsSettings{required_text(*tls, "domain"), required_text(*tls, "certificate_chain"),
                       required_text(*tls, "private_key")};
}

} // namespace

PanelSettings load_panel_settings(const char* executable, const fs::path& site_dir)
{
    const auto path = executable_path(executable).parent_path() / "wave-db.json";
    const auto relative = path.lexically_relative(fs::weakly_canonical(fs::absolute(site_dir)));
    if (!relative.empty() && *relative.begin() != "..") {
        throw std::runtime_error("Keep wave-db.json outside the site directory.");
    }
    if (fs::symlink_status(path).type() == fs::file_type::not_found) {
        write_database(path, {{"listen", {{"host", default_host}, {"port", default_port}}}}, false);
    }
    auto settings = read_database(path);
    auto& listen = *settings.emplace("listen", Json::object()).first;
    if (!listen.is_object()) {
        throw std::runtime_error("listen in wave-db.json must be a JSON object.");
    }
    const auto [host, added_host] = listen.emplace("host", default_host);
    const auto [port, added_port] = listen.emplace("port", default_port);
    if (!host->is_string() || host->get_ref<const std::string&>().empty() ||
        std::ranges::any_of(host->get_ref<const std::string&>(), [](unsigned char ch) {
            return ch <= 0x20 || ch == 0x7f;
        })) {
        throw std::runtime_error("listen.host in wave-db.json must be a nonempty address without spaces.");
    }
    if (!port->is_number_integer() || port->get<std::uint64_t>() > 65535) {
        throw std::runtime_error("listen.port in wave-db.json must be an integer from 0 to 65535.");
    }
    if (added_host || added_port) {
        write_database(path, settings, true);
    }
    return {host->get<std::string>(), port->get<int>(), load_tls(settings)};
}

} // namespace wave
